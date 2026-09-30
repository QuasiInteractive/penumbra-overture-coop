#!/usr/bin/env python3
"""
master_server.py - tiny master server for the Penumbra: Overture co-op mod.

Hosts that set `public=1` + `master_server=host:port` in multiplayer.cfg send a
Register beacon here every 20 s; the in-game "Internet" server browser asks for
the live list. Raw UDP, one datagram per message, no state on the browser side.
Wire structs are the cNetMaster* records in ../NetworkPackets.h ('MASTER SERVER
protocol'); this file is the reference implementation of that protocol and is
deliberately stdlib-only (Python 3.6+), one file, no config file.

Usage
-----
  python3 master_server.py                 # listen on udp/7779, log to stdout
  python3 master_server.py --port 7779 --bind 0.0.0.0 --expiry 60 --verbose
  python3 master_server.py --dump          # ask a RUNNING master (default
                                           # 127.0.0.1:7779) for its table and
                                           # print it; --host/--port pick another

Protocol (all integers little-endian; the mod runs on same-endian x86 peers)
---------------------------------------------------------------------------
  every packet starts with   <BIH   type, magic 'PNMS' (0x504E4D53), version 1
  1 Register    host->master  <BIHHBBBH32s32s   game port, players, max,
                                                 flags (bit0 = password),
                                                 game protocol version,
                                                 server name, map name    (78 B)
  2 Unregister  host->master  <BIHH             game port                (9 B)
  3 List        browser->master <BIHH           browser's game protocol  (9 B)
  4 Entries     master->browser <BIHB + N*entry  N <= 10 per datagram    (8 + 77N B)
      entry = <4sHBBBHH32s32s  ip4 (network order bytes), game port, players,
                               max, flags, protocol version, age seconds,
                               server name, map name

The server is keyed by (SOURCE ip, given game port): a host cannot know its
public address, and the source PORT is whatever its NAT chose for the
discovery socket, so only the ip comes from the envelope. Entries expire
--expiry seconds (default 60 = 3 missed beacons) after the last Register.
List replies are rate limited to --rate (default 5) per second per source ip
and capped at --max-entries (default 100) servers. Anything malformed - short,
wrong magic/version, unknown type, wrong length for its type - is dropped
(counted in the periodic stats line, never answered).

Deploy (any VPS, or your own PC with udp/7779 forwarded)
--------------------------------------------------------
  /etc/systemd/system/penumbra-master.service:

    [Unit]
    Description=Penumbra co-op master server
    After=network-online.target
    Wants=network-online.target

    [Service]
    ExecStart=/usr/bin/python3 /opt/penumbra/master_server.py --port 7779
    Restart=on-failure
    RestartSec=5
    User=nobody
    DynamicUser=yes
    NoNewPrivileges=yes
    ProtectSystem=strict
    ProtectHome=yes

    [Install]
    WantedBy=multi-user.target

  sudo systemctl daemon-reload && sudo systemctl enable --now penumbra-master
  sudo ufw allow 7779/udp        # or the equivalent on your firewall
  journalctl -u penumbra-master -f
"""

import argparse
import logging
import select
import signal
import socket
import struct
import sys
import time

MAGIC = 0x504E4D53          # 'PNMS'
VERSION = 1

T_REGISTER = 1
T_UNREGISTER = 2
T_LIST = 3
T_ENTRIES = 4

FLAG_PASSWORD = 1

HEADER = struct.Struct("<BIH")
REGISTER = struct.Struct("<BIHHBBBH32s32s")
UNREGISTER = struct.Struct("<BIHH")
LIST = struct.Struct("<BIHH")
ENTRIES_HDR = struct.Struct("<BIHB")
ENTRY = struct.Struct("<4sHBBBHH32s32s")

MAX_PER_IP = 8                   # distinct game ports one source ip may list
MAX_TABLE = 1000                 # total entries; new registrations refused when full
MAX_ENTRIES_PER_DATAGRAM = 10   # 8 + 10*77 = 778 bytes, well under any MTU
NAME_LEN = 32

assert HEADER.size == 7
assert REGISTER.size == 78
assert UNREGISTER.size == 9
assert LIST.size == 9
assert ENTRIES_HDR.size == 8
assert ENTRY.size == 77

log = logging.getLogger("master")


def cstr(raw):
    """Fixed NUL-padded wire field -> printable str (never trusts the sender)."""
    raw = raw.split(b"\0", 1)[0][:NAME_LEN - 1]
    text = raw.decode("latin-1")
    return "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in text)


def pack_name(text):
    return text.encode("latin-1", "replace")[:NAME_LEN - 1].ljust(NAME_LEN, b"\0")


class Server:
    """One registered host."""

    __slots__ = ("ip", "port", "players", "max_players", "flags",
                 "protocol", "name", "map", "first_seen", "last_seen")

    def __init__(self, ip, port):
        self.ip = ip
        self.port = port
        self.players = 0
        self.max_players = 0
        self.flags = 0
        self.protocol = 0
        self.name = ""
        self.map = ""
        self.first_seen = self.last_seen = time.monotonic()

    def entry_bytes(self, now):
        age = int(now - self.last_seen)
        age = max(0, min(age, 0xFFFF))
        return ENTRY.pack(socket.inet_aton(self.ip), self.port, self.players,
                          self.max_players, self.flags, self.protocol, age,
                          pack_name(self.name), pack_name(self.map))


class RateLimiter:
    """Token bucket per source ip: `rate` replies per second, burst = rate."""

    def __init__(self, rate):
        self.rate = float(rate)
        self.buckets = {}   # ip -> [tokens, last_refill]

    def allow(self, ip, now):
        b = self.buckets.get(ip)
        if b is None:
            b = self.buckets[ip] = [self.rate, now]
        tokens = min(self.rate, b[0] + (now - b[1]) * self.rate)
        b[1] = now
        if tokens < 1.0:
            b[0] = tokens
            return False
        b[0] = tokens - 1.0
        return True

    def prune(self, now):
        stale = [ip for ip, b in self.buckets.items() if now - b[1] > 60.0]
        for ip in stale:
            del self.buckets[ip]


class Master:
    def __init__(self, bind, port, expiry, rate, max_entries):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((bind, port))
        self.sock.setblocking(False)
        self.expiry = float(expiry)
        self.max_entries = int(max_entries)
        self.limiter = RateLimiter(rate)
        self.reg_limiter = RateLimiter(1.0)   # new registrations per second per ip
        self.servers = {}       # (ip, port) -> Server
        self.stats = {"register": 0, "unregister": 0, "list": 0,
                      "malformed": 0, "ratelimited": 0}
        self.running = True

    # -- packet handlers --------------------------------------------------

    def handle(self, data, addr, now):
        ip = addr[0]
        if len(data) < HEADER.size:
            return self.malformed(addr, "short")
        ptype, magic, ver = HEADER.unpack_from(data)
        if magic != MAGIC or ver != VERSION:
            return self.malformed(addr, "magic/version %08x/%d" % (magic, ver))

        if ptype == T_REGISTER:
            if len(data) != REGISTER.size:
                return self.malformed(addr, "register len %d" % len(data))
            (_, _, _, gport, players, maxp, flags, proto,
             name, mapname) = REGISTER.unpack(data)
            if gport == 0:
                return self.malformed(addr, "register port 0")
            key = (ip, gport)
            sv = self.servers.get(key)
            fresh = sv is None
            if fresh:
                # abuse limits: a new entry costs a token (1/s per source ip),
                # at most MAX_PER_IP entries per ip, MAX_TABLE overall
                if not self.reg_limiter.allow(ip, now):
                    self.stats["ratelimited"] += 1
                    return
                if sum(1 for k in self.servers if k[0] == ip) >= MAX_PER_IP:
                    return self.malformed(addr, "too many servers from this ip")
                if len(self.servers) >= MAX_TABLE:
                    return self.malformed(addr, "table full")
                sv = self.servers[key] = Server(ip, gport)
            sv.players, sv.max_players = players, maxp
            sv.flags, sv.protocol = flags, proto
            sv.name, sv.map = cstr(name), cstr(mapname)
            sv.last_seen = now
            self.stats["register"] += 1
            log.log(logging.INFO if fresh else logging.DEBUG,
                    "%s %s:%d '%s' map='%s' %d/%d proto=%d%s (%d listed)",
                    "REGISTER " if fresh else "beacon   ", ip, gport, sv.name,
                    sv.map, players, maxp, proto,
                    " [pw]" if flags & FLAG_PASSWORD else "", len(self.servers))

        elif ptype == T_UNREGISTER:
            if len(data) != UNREGISTER.size:
                return self.malformed(addr, "unregister len %d" % len(data))
            gport = UNREGISTER.unpack(data)[3]
            sv = self.servers.pop((ip, gport), None)
            self.stats["unregister"] += 1
            if sv is not None:
                log.info("UNREGISTER %s:%d '%s' (%d listed)", ip, gport, sv.name,
                         len(self.servers))

        elif ptype == T_LIST:
            if len(data) != LIST.size:
                return self.malformed(addr, "list len %d" % len(data))
            if not self.limiter.allow(ip, now):
                self.stats["ratelimited"] += 1
                log.debug("rate-limited LIST from %s:%d", *addr)
                return
            proto = LIST.unpack(data)[3]
            self.stats["list"] += 1
            self.send_list(addr, now)
            log.debug("LIST from %s:%d (proto %d) -> %d server(s)", addr[0],
                      addr[1], proto, min(len(self.servers), self.max_entries))

        else:
            return self.malformed(addr, "type %d" % ptype)

    def malformed(self, addr, why):
        self.stats["malformed"] += 1
        log.debug("dropped malformed packet from %s:%d: %s", addr[0], addr[1], why)

    def send_list(self, addr, now):
        # Newest beacon first, so a browser that only gets the first datagram
        # sees the liveliest servers.
        live = sorted(self.servers.values(), key=lambda s: -s.last_seen)
        live = live[:self.max_entries]
        chunks = [live[i:i + MAX_ENTRIES_PER_DATAGRAM]
                  for i in range(0, len(live), MAX_ENTRIES_PER_DATAGRAM)] or [[]]
        for chunk in chunks:
            payload = ENTRIES_HDR.pack(T_ENTRIES, MAGIC, VERSION, len(chunk))
            payload += b"".join(s.entry_bytes(now) for s in chunk)
            try:
                self.sock.sendto(payload, addr)
            except OSError as exc:
                log.warning("sendto %s:%d failed: %s", addr[0], addr[1], exc)
                return

    # -- housekeeping -----------------------------------------------------

    def expire(self, now):
        dead = [k for k, s in self.servers.items() if now - s.last_seen > self.expiry]
        for k in dead:
            sv = self.servers.pop(k)
            log.info("EXPIRED  %s:%d '%s' (last beacon %.0fs ago, %d listed)",
                     sv.ip, sv.port, sv.name, now - sv.last_seen, len(self.servers))
        self.limiter.prune(now)
        self.reg_limiter.prune(now)

    def run(self):
        last_expire = last_stats = time.monotonic()
        log.info("listening on udp/%d (expiry %.0fs, max %d entries, %g list/s per ip)",
                 self.sock.getsockname()[1], self.expiry, self.max_entries,
                 self.limiter.rate)
        while self.running:
            readable, _, _ = select.select([self.sock], [], [], 1.0)
            now = time.monotonic()
            if readable:
                # Drain everything queued; the socket is non-blocking.
                for _ in range(256):
                    try:
                        data, addr = self.sock.recvfrom(2048)
                    except (BlockingIOError, InterruptedError):
                        break
                    except ConnectionResetError:
                        continue    # Windows: ICMP unreachable from a dead peer
                    except OSError as exc:
                        log.warning("recvfrom: %s", exc)
                        break
                    if addr[0].count(".") != 3:
                        continue    # IPv4 only: the entry carries 4 address bytes
                    self.handle(data, addr, now)
            if now - last_expire >= 1.0:
                self.expire(now)
                last_expire = now
            if now - last_stats >= 300.0:
                log.info("stats: %d listed, %s", len(self.servers),
                         ", ".join("%s=%d" % kv for kv in sorted(self.stats.items())))
                last_stats = now
        self.sock.close()
        log.info("stopped")


# -- --dump: query a running master and print its table ---------------------

def dump(host, port, timeout, protocol):
    """Send one List and print every entry that arrives within `timeout` s.
    Returns the number of servers printed."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.25)
    try:
        target = (socket.gethostbyname(host), port)
    except OSError as exc:
        print("cannot resolve %s: %s" % (host, exc), file=sys.stderr)
        return -1
    s.sendto(LIST.pack(T_LIST, MAGIC, VERSION, protocol), target)
    rows = {}
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            continue
        except OSError:
            break
        if addr != target or len(data) < ENTRIES_HDR.size:
            continue
        ptype, magic, ver, count = ENTRIES_HDR.unpack_from(data)
        if ptype != T_ENTRIES or magic != MAGIC or ver != VERSION:
            continue
        avail = (len(data) - ENTRIES_HDR.size) // ENTRY.size
        for i in range(min(count, avail, MAX_ENTRIES_PER_DATAGRAM)):
            e = ENTRY.unpack_from(data, ENTRIES_HDR.size + i * ENTRY.size)
            ip4, gport, players, maxp, flags, proto, age, name, mapname = e
            rows[(socket.inet_ntoa(ip4), gport)] = (players, maxp, flags, proto,
                                                    age, cstr(name), cstr(mapname))
    s.close()
    print("%-21s %-31s %-20s %-7s %-5s %-5s %s" % (
        "address", "name", "map", "players", "proto", "age", "flags"))
    for (ip, gport), (players, maxp, flags, proto, age, name, mapname) in sorted(rows.items()):
        print("%-21s %-31s %-20s %-7s %-5d %-5d %s" % (
            "%s:%d" % (ip, gport), name, mapname, "%d/%d" % (players, maxp),
            proto, age, "[pw]" if flags & FLAG_PASSWORD else ""))
    print("%d server(s)" % len(rows))
    return len(rows)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="0.0.0.0", help="listen address (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=7779, help="udp port (default 7779)")
    ap.add_argument("--expiry", type=float, default=60.0,
                    help="seconds without a beacon before a server is dropped (default 60)")
    ap.add_argument("--rate", type=float, default=5.0,
                    help="max List replies per second per source ip (default 5)")
    ap.add_argument("--max-entries", type=int, default=100,
                    help="most servers ever returned to one List (default 100)")
    ap.add_argument("--verbose", "-v", action="store_true",
                    help="log every beacon, list and dropped packet")
    ap.add_argument("--dump", action="store_true",
                    help="client mode: print the table of a running master and exit")
    ap.add_argument("--host", default="127.0.0.1", help="--dump: master host (default 127.0.0.1)")
    ap.add_argument("--timeout", type=float, default=1.0, help="--dump: collect for N seconds")
    ap.add_argument("--protocol", type=int, default=0,
                    help="--dump: game protocol version to send in the List (informational)")
    args = ap.parse_args(argv)

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)-7s %(message)s",
                        datefmt="%Y-%m-%d %H:%M:%S", stream=sys.stdout)

    if args.dump:
        return 0 if dump(args.host, args.port, args.timeout, args.protocol) >= 0 else 1

    if args.port <= 0 or args.port > 65535:
        ap.error("--port must be 1..65535")
    if args.rate <= 0 or args.expiry <= 0 or args.max_entries <= 0:
        ap.error("--rate, --expiry and --max-entries must be positive")

    try:
        master = Master(args.bind, args.port, args.expiry, args.rate, args.max_entries)
    except OSError as exc:
        print("cannot bind udp/%d on %s: %s" % (args.port, args.bind, exc), file=sys.stderr)
        return 1

    def stop(signum, _frame):
        log.info("signal %d, stopping", signum)
        master.running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    master.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
