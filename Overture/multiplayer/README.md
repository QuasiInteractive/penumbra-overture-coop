# Penumbra Overture — Phase 1 Multiplayer

All dedicated multiplayer source lives in this folder. Work here first; game glue is listed below.

## Files in this folder

| File | Role |
|------|------|
| `NetworkPackets.h` | Packed ENet payloads (join / leave / player state) + discovery ping/pong, protocol magic/version, `kNetDiscoveryPort`. |
| `NetworkManager.h` / `.cpp` | Listen-server host, client join, ~20 Hz pose relay, LAN/Hamachi discovery (raw UDP broadcast), `multiplayer.cfg`, F9/F10/F11. |
| `BodySync.h` / `.cpp` | Phase 5 shared physics: host-authoritative body replication. Name-hash identity, map-load census (host/guest verify), host state batches, guest apply. |
| `GhostPlayer.h` / `.cpp` | Remote peer visuals only (mesh + marker light + flashlight). Not a real `cPlayer`. |
| `multiplayer.cfg.example` | Copy next to `overture.exe` as `multiplayer.cfg`. |
| `models/` | Drop ghost `.dae` meshes here; runtime resource dir is `multiplayer/models` (cwd = exe folder). |

Build flag: `PENUMBRA_MULTIPLAYER` (CMake option, default ON when vcpkg `enet` is found). Without it, `NetworkManager` is a stub.

## Architecture (Phase 1)

- **Transport:** ENet UDP, 2 channels (0 = reliable control, 1 = unsequenced pose).
- **Topology:** listen-server. Host = PlayerID `1`; guests get `2+`. Host relays guest pose to others and applies locally.
- **Authority:** none. Pose is camera eye position + pitch/yaw + flashlight bit. Same map assumed on all peers.
- **Tick:** ~20 Hz local snapshot send (`kSendPeriodSeconds`).

## Game glue (outside this folder — edit carefully)

| Location | What |
|----------|------|
| `../Init.cpp` / `Init.h` | Owns `mpNetworkManager`; `Startup()` after input exists; `Update()` each frame; config port load/save. |
| `../MainMenu.cpp` / `MainMenu.h` | Multiplayer menu states, host/join UI, IP typing widget (`#ifdef PENUMBRA_MULTIPLAYER`). |
| `../CMakeLists.txt` | Globs `multiplayer/*.cpp`, links ENet, defines `PENUMBRA_MULTIPLAYER`. |
| `../vcpkg.json` | Declares `enet` (+ SDL/OpenAL audio deps). |

## Runtime controls

- **F11** — toggle host on default port (7777).
- **F10** — join `127.0.0.1:<port>`.
- **F9** — LAN/Hamachi discovery scan (~1.5s window; results logged, feed the server browser).
- **Menu** — Multiplayer → Host / Join.
- **`multiplayer.cfg`** — `host=1`, `join=HOST:PORT`, `port=`, `server_name=`, `max_players=`, `ghost_models=a.dae,b.dae`, `ghost_body_y=`.

## Discovery (server browser backend)

Raw UDP (not ENet) on `kNetDiscoveryPort` **7778** — a fixed side port, NOT
SO_REUSEADDR on the game port (on Windows, unicast to a twice-bound port is
bind-order dependent and could steal ENet traffic; see NetworkPackets.h).
Browser broadcasts a ping to 255.255.255.255, 127.0.0.1, and every
interface's directed broadcast (`ip | ~mask` via GetAdaptersInfo) — the last
one is what makes Hamachi/Radmin/ZeroTier work, since the virtual LAN is its
own interface and the global broadcast usually picks the wrong NIC. Hosts
answer with name / map / players / real game port; version-mismatched servers
are listed with `mbVersionMatch=false` so the UI can grey them out.

## Explicit non-goals (Phase 1)

No map/load sync, inventory, door/entity interaction, AI sync, interpolation, or cross-endian packets.

## Suggested next work

1. Pose interpolation / smoothing on `GhostPlayer::ApplyState`.
2. Map name handshake so join fails clearly if worlds differ.
3. Sync a small set of interactables (doors, levers) with reliable packets.
4. Extract MainMenu multiplayer UI widgets into this folder if the menu file stays too large.

## Build reminder

Sibling layout: `PenumbraDev/{HPL1Engine,OALWrapper,dependencies,PenumbraOverture-master}`. Configure with vcpkg toolchain so `enet` resolves. Win32 MSVC deps need `Newton.lib` under `dependencies/lib/win32`.
