# Penumbra Overture — Co-op Multiplayer

All dedicated multiplayer source lives in this folder. Work here first; game glue is listed below.

## Files in this folder

| File | Role |
|------|------|
| `NetworkPackets.h` | Packed ENet payloads (player state, bodies, enemies, script/item events, join/leave) + discovery ping/pong, protocol magic/version (`kNetProtocolVersion`), `kNetSendPeriodSeconds`, `kNetDiscoveryPort`. |
| `NetworkManager.h` / `.cpp` | Listen-server host, client join, 30 Hz state relay, LAN/Hamachi discovery (raw UDP broadcast), `multiplayer.cfg` (read + the `player_name` writer), F9/F10/F11, per-frame ghost updates, the ghost preview mode, player names + the party event feed (v13). |
| `NetworkPackets.h` | Packed ENet payloads (player state, bodies, enemies, script/item events, join/leave, v14 MapReady + WorldSnapshot sections, v15 Challenge/Auth + `NetAuthDigest`) + discovery ping/pong, protocol magic/version (`kNetProtocolVersion`), `kNetSendPeriodSeconds`, `kNetDiscoveryPort`, disconnect reasons; master-server protocol (`cNetMaster*`, magic `PNMS`). |
| `NetworkManager.h` / `.cpp` | Listen-server host, client join, 30 Hz state relay, LAN/Hamachi discovery (raw UDP broadcast), master-server register/list (internet browser), `multiplayer.cfg`, F9/F10/F11, per-frame ghost updates, the ghost preview mode; v15 join authentication, role gating (`IsAllowedFrom`), payload validation, strikes, rate limits (see Security). |
| `tools/master_server.py` | Reference master server (Python 3, stdlib only) for the Internet tab — see 'Public servers'. |
| `BodySync.h` / `.cpp` | Shared physics: host-authoritative body replication. Name-hash identity, map-load census (host/guest verify), host state batches (awake deltas, reliable sleep edges, a 2/tick unreliable keyframe trickle of sleeping bodies), guest apply: per-body seq guard; moving states are **velocity** targets (COM P-term + host-motion feed-forward, clamped, gravity pre-compensated — never a per-frame SetMatrix, so Newton's contacts keep bodies out of walls), hard snap only when > 1.5 m / ~150 deg off or stuck 0.5 s (jointed 1.5 s, gentler clamps); rest poses pin (exact pose, zero motion, frozen), a pinned body woken by guest-local physics is eased back after 0.5 s, and a body the host is silent on for 1 s is pinned at its last pose. Ragdoll bones are not indexed/streamed (census unchanged). Held-object prediction is velocity-driven too. |
| `GhostPlayer.h` / `.cpp` | Remote peer visuals (skinned mesh + marker light + flashlight). Interpolation buffer on the sender's clock, clip selection from the wire velocity/flags, weight-preserving crossfades, gait-scaled playback. Not a real `cPlayer`. |
| `VoiceChat.h` / `.cpp` | v16 proximity voice chat: OpenAL capture (push-to-talk / level-gated open mic) -> Opus 16 kHz 20 ms frames -> `cNetVoice` packets; per-remote-player Opus decoder + OpenAL streaming source at the ghost's head with a 3-packet jitter buffer, PLC and inverse-distance gain. Raw `<AL/al.h>` on the engine's context; stubbed without `PENUMBRA_VOICE`. |
| `multiplayer.cfg.example` | Copy next to `overture.exe` as `multiplayer.cfg`. |
| `tools/syntax_check.sh` + `tools/syntax_stubs/` | Linux `g++ -fsyntax-only` harness with fake third-party headers (enet, Newton, AngelScript, GL, SDL, opus, AL) — `--no-voice` / `--no-mp` check the stub halves. |
| `models/` | Ghost `.dae` meshes + `<name>_<clip>.dae` clips + `<name>_clips.json` (gait table); runtime resource dir is `multiplayer/models` (cwd = exe folder). Every `<name>.dae` (no `_`) with clips here is a player character (see 'Characters'). |

Build flag: `PENUMBRA_MULTIPLAYER` (CMake option, default ON when vcpkg `enet` is found). Without it, `NetworkManager` is a stub.

## Architecture (v0.10+)

ENet UDP, 2 channels (0 = reliable control/events, 1 = unsequenced streams),
listen-server topology: the host is PlayerID `1`, guests get `2+`, the host
relays guest packets to the other guests. The host is **authoritative** for
physics bodies (guests forward grab/push intent and receive body states),
for enemies (guests puppet their local enemies from host EnemyState batches
and report their hits) and for the party's shared script state (variable
writes, entity activation, door locks, item pickups/drops/consumption and
breakable damage are replicated as reliable events; map changes move the
whole party). Player state, bodies and enemies stream at **30 Hz**
(`kNetSendPeriodSeconds`); every wire layout change bumps
`kNetProtocolVersion` (currently **19**), and the connect handshake refuses
mismatched builds (then the v15 password challenge, see **Security**).

Remote players are drawn as **ghosts** (`cGhostPlayer`): each `cNetPlayerState`
carries the sender's feet position, view pitch/yaw, body-local planar
velocity, an on-ground/crouch/run/jump flag byte and a sequence number. The
receiver buffers states stamped `seq * kNetSendPeriodSeconds` (the sender's
own clock, immune to receive jitter), renders `ghost_interp_ms` (100 ms)
behind the newest one with interpolated position and shortest-arc yaw,
extrapolates up to 250 ms across gaps and snaps on teleports. Clips are
chosen from the wire velocity (direction sectors with hysteresis, run =
shift + speed), stance and ground contact (jump on the airborne edge, land
on the ground edge, crouch survives jumps), and switched with linear
`FadeIn`/`FadeOut` ramps whose weights always sum to 1 (the engine never
normalizes). Locomotion clips play at `real speed / gait speed` (clamped
0.6..1.8) so the feet do not slide; gaits come from the built-in table,
the model's `<name>_clips.json`, then `ghost_gait_*` in `multiplayer.cfg`.

### Party health (protocol v12)

`cNetPlayerState` carries the sender's health (`mHealth`, 0-100) and an
`eNetPlayerFlag_Dead` bit (`cPlayer::IsDead()`), filled in
`BuildLocalSnapshot`. Every receiver — host and guests, relay is a plain
copy — keeps the newest value per remote id (`cNetworkManager::
GetGhostHealth`, `GetPartyStatus`); the map is keyed by id, not by ghost
entity, so it survives our own map change and is erased when the peer
leaves. Consumers:

- **Enemy AI (host)** — `iGameEnemy::GetFocusHealth()` returns the mirrored
  value (0 = dead or gone), `UpdateCheckForPlayer` never offers a dead ghost
  as a sight candidate, and the host-side ghost footstep triggers
  (`TriggerHandler.cpp`) stop for a dead guest. A dog that just killed a
  guest eats/idles instead of re-biting a corpse.
- **HUD** — `cPlayer::DrawPartyHud()` (called from `cPlayer::OnDraw`, after
  the health filter) projects a point 2 m above each ghost's rendered feet
  and draws a 60x6 bar (red..green) + the player's name (`<name> dead`),
  hidden behind the camera or beyond 25 m, fading from 15 m; not drawn over
  the inventory, notebook, panel or death menu.
- **Inventory** — `cInventory::DrawParty()` lists every connected player
  (by name) with health and a bar in the free column right of the slot grid.

### Player names + party panel (protocol v13)

`multiplayer.cfg` `player_name=` (printable ASCII, 24 chars max, sanitised
on load, on menu input and on every wire arrival —
`cNetworkManager::SanitizePlayerName`). The Start screen's **Multiplayer**
button opens a "What's your username?" screen when no name is set (Enter
or **Save** writes the key with `UpdateMultiplayerCfgKey`, which rewrites
only that line and keeps every other line of the file; **Change name** on
the Multiplayer screen edits it later, live sessions included).

Wire: reliable `cNetPlayerName` (type **26**: id + `char[24]`, NUL-padded,
not necessarily NUL-terminated — receivers scan bounded; v17 appends the
`uint8_t mCharacter` slot, v19: + `msCharacter[24]` name, 51 B, see 'Characters'). A guest sends its
name right after `PlayerJoin` (it knows its id then, and sends even an
empty one so the host can announce the join); the host stores names per
peer id (the id BYTE from a guest is ignored, the peer it came from is the
truth), sends the table to a new peer at connect and re-broadcasts the whole
table (one packet per player, its own name as id 1) whenever a name arrives
or a guest leaves. Since v17 every accepted guest has an entry, named or not
(an empty name still carries the character slot). `DropRemotePlayer` erases. `GetPlayerName(id)` returns
`"Player <id>"` for anyone without a name. The discovery pong's server name
falls back to `"<player_name>'s game"` when `server_name` is empty.

**Party panel** — `cPlayer::DrawPartyPanel()`, top-left of the HUD, only
while hosting or in a live session (never in single-player), same screen
guards as `DrawPartyHud`: one line per member (ourselves included, id
order, `(host)` after id 1) with a 50x5 health bar, `dead` in red; below it
the last 4 feed lines from `cNetworkManager::GetPartyEvents()`, each
dropped after 6 s (fading over the last 1.5 s). Feed sources
(`AddPartyEvent`): `<name> joined` when a name arrives for a new id (a
guest lists the party it finds in its first 2 s silently), `<name> left`
from `DropRemotePlayer` (only for announced ids), `<name> died` /
`<name> respawned` from the mirrored-health transitions (100..1 -> 0 and
0 -> >0) in the state handler, `<old> is now <new>` on a rename.

### Co-op death rule (`coop_respawn=1`, default)

`cPlayerDeath::Update` decides ONCE, at the frame the vanilla sequence
would open `cDeathMenu`: if the session is live (hosting with a connected
guest, or a synced client), `coop_respawn` is on and at least one OTHER
member's mirrored health is > 0, the death menu is skipped. The usual fade
plays; after 3 s the player gets 40 health, is teleported to the nearest
living member's last wire feet position (+5 cm, the spawn-at-host offset),
stands up, is un-hidden and the death state is reset (hpl.log:
"respawned next to player N"). Re-evaluated every frame from the 3 s mark:
if everybody else died or left meanwhile the vanilla death menu opens; if a
living member has no position yet (between maps) it waits up to 15 s, then
gives up to the death menu. Offline the code path is the original two
lines — single-player is unchanged.

## Characters (four-player lobbies, protocol v17; picker v18)

**One of each, never a duplicate.** The host owns a slot table: slot 0 is
the host itself, every accepted guest (`HostAcceptPeer`) gets the lowest
free slot, and the slot is freed when the guest goes (`DropRemotePlayer`,
reached from every disconnect path: leave, timeout, kick, and the guard
sweep for a peer slot that died without an event; `HostGame`/`Disconnect`
reset the table). A slot is an index into the character list below. The
shipped list is ordered `phillip, fisherman, red, malik`
(`kGhostCharacterOrder` in NetworkManager.cpp; other auto-discovered
characters follow alphabetically), so the host always plays Philip, the
game's main character, and a guest without a preference gets The
Fisherman, Red and Malik in join order; a player who leaves frees the
character for the next joiner. Since v19 an explicit `ghost_models=` is put
in that same order too (per-entry `ghost_body_ys` move with it; hpl.log says
`ghost_models reordered to the fixed character order`): an old
`ghost_models=malik.dae,phillip.dae` made the host Malik. Since v18 a guest
can pick (below).

**Display names (v18).** Players never see file names:
`cNetworkManager::GetCharacterDisplayName` maps `phillip` -> Philip,
`fisherman` -> The Fisherman, `red` -> Red, `malik` -> Malik, and any other
base name to itself with the first letter capitalised (`kCharacterDisplayNames`
at the tail of NetworkManager.cpp). Used by the party panel
(`Deadl (The Fisherman)`, `Host (Philip) (host)`), the picker and the feed.

**Picker (v18).** The host is **always slot 0 (Philip), locked**: the host
lobby (and the Multiplayer screen while hosting) reads `You play Philip
(the host's character)` instead of a picker. A guest has a preference —
`character=<file base name>` in `multiplayer.cfg` (empty / absent = no
preference; printable ASCII, 24 characters max, matched
case-insensitively) — shown and changed by a `Character: < Red >` line on
the **Multiplayer** screen and on the **Direct-connect / join status**
screen (so a guest waiting in the menu for the host to launch can still
pick) and in the **server browser**. `<` / `>` step to the previous / next
selectable character, wrapping (clicking the name steps forward); on the
browser and join screens a row of chips under it (`Philip (host)`,
`Red (taken)`, free ones clickable) picks directly. Never
Philip (the host's, by name since v19), and while connected as a guest never one
another player holds per the name table; offline every non-host character
is offered. One selectable character = the click re-sends it; none = the
click does nothing. The choice is written with `UpdateMultiplayerCfgKey`
(every other cfg line kept) and, when connected, sent to the host at once.
The same line in the in-game Esc menu (Multiplayer screen) swaps a
connected guest live. Suffixes: `(taken)` = the preference is held by
somebody else (first come, first served); `(playing X)` = the host has not
moved us (request in flight, or refused, e.g. its list lacks the
character); `(host only)` / `(not installed)` = a hand-edited `character=`
naming entry 0 / a character missing here. No preference = `< any >`
offline, the assigned character when connected.

**Request flow (v18, `cNetCharacterRequest`, type 30, 25 B, reliable ch0,
guest -> host only).** It carries the character's **file base name**, not
a slot, so machines whose lists are ordered differently still agree on the
character. `HostAcceptPeer` still gives a new guest the lowest free slot at
once (a guest always has a character); the guest sends its request right
after its name (after `PlayerJoin`) when `character=` is set, and again on
every pick while connected. The host (`HostHandleCharacterRequest` ->
`HostApplyCharacterRequest`) matches the name case-insensitively against
**its** list; if it exists, is not slot 0, is inside the slot range and is
free (or already the requester's), the requester moves there, its old slot
is free again, the host rebuilds its own ghost of it, adds `<name> now
plays Red` to the feed and re-broadcasts the name table — which rebuilds
the ghost on every guest through `OnPlayerSlotReceived` (and puts the same
feed line there). Anything else is ignored (hpl.log, once per connection:
`guest N asked for character 'x' - ignored: taken by another player`) and
the table stays as it is. Two guests asking for the same character in the
same frame: packets are handled one at a time, the first wins, the second
is ignored and its line shows `(taken)`. Per peer at most one request is
applied per 0.25 s (`kNetCharRequestCooldown`); requests inside the
cooldown replace the pending one, which `UpdatePeerGuards` applies — the
last pick wins and a spamming guest cannot flood the table. Validation: a
request must be exactly 25 bytes with a non-empty printable-ASCII,
NUL-bounded name (`ValidateEventPacket`), else it is a strike like any
malformed packet; it is a guest-only type in `IsAllowedFrom`; before the
guest is accepted it is dropped by the auth gate (`packet before
authentication`) and a peer without a slot never moves.

**Player cap = number of characters.** The effective cap is
`min(max_players, character count)` (`GetMaxPlayers()`): CONNECT refuses
one more peer with `kNetDisconnectFull` ("Host REFUSED: the server is full
(one player per character)." on the join screen), `HostAcceptPeer` refuses
too if no slot is free (belt and braces), and the discovery pong, the master
listing and the host lobby's `Players n/max` all show the effective cap.
hpl.log at host start: `multiplayer: player cap 4 = min(max_players 4,
4 character(s))`. `max_players=8` with four characters still means four.

**Wire.** The v13 name table carries it: `cNetPlayerName` grew
`uint8_t mCharacter` (the slot; 255 = unknown, what a guest sends about
itself — the host never reads it). The host fills it for every entry (its
own = 0) and re-broadcasts the table on every join/leave/rename as before
(v18: and on every accepted character request);
`ValidateEventPacket` accepts `mCharacter < 32` or 255. Guests keep the slot
per id (theirs included). **v19:** the entry also carries
`msCharacter[24]`, the slot's file base name in the host's list (51 bytes
now); a guest looks it up in its own list and stores ITS index for that
character, so lists in another order still agree on who is who.

**Model choice.** A ghost for PlayerID N draws `list[slot mod count]` on
every machine, where a guest's slot is its own index of the host's
character name (v19). A state packet can
beat the table, so until the slot is known the ghost uses the old guess
`(N-1) mod count`; when the slot arrives or changes and the guess was a
different mesh, `OnPlayerSlotReceived` deletes the ghost and the next state
packet re-creates it through `EnsureGhost` (seq/health/move state live
outside the ghost; only the interpolation buffer refills, ~33 ms). The
preview ghost keeps `ghost_preview_model`. `GetPlayerCharacterName(id)`
("fisherman") is shown in the party panel through its display name:
`Deadl (The Fisherman)`, the host's line `Host (Philip) (host)`.

**Every machine needs the same character files.** Since v19 the name
travels with the slot, so a different ORDER is harmless; a guest that lacks
the host's character logs `WARNING the host's character 'x' is not in our
multiplayer/models` once and falls back to `slot mod M` (that player then
looks like somebody else on that machine only).

The list itself — `cNetworkManager::ResolveGhostModels` (called from
`Startup`, right after `multiplayer/models` became a resource dir; cfg
`host=1` now opens the server after it, so the cap is final) builds it once:

1. **`ghost_models=a.dae,b.dae,...`** (or `ghost_model=x.dae`) in
   `multiplayer.cfg`: that order, minus entries the engine's file searcher
   cannot resolve (same lookup `cMeshManager::CreateMesh` uses; one
   `ghost_models entry N 'x.dae' not found - skipped` line each) — a cfg
   naming four characters works before all four are exported. A single
   `ghost_model=` means one character = a solo host since v17 (logged as a
   WARNING at host start).
2. **Otherwise (the shipped default): auto-discovery.** Every
   `multiplayer/models/<name>.dae` whose `<name>` contains no `_` and that
   has at least one `<name>_<clip>.dae` sibling (`malik.dae` +
   `malik_idle.dae`, ...) is a character. Files are enumerated with the
   engine's `iLowLevelResources::FindFilesInDir` (`_wfindfirst` on Windows,
   `opendir` elsewhere) and sorted by an ASCII-lowercase compare (exact
   bytes break ties; case-only duplicates collapse), so the order does not
   depend on the file system or the C locale, then put in
   `kGhostCharacterOrder` (phillip, fisherman, red, malik; the rest
   alphabetically after them). A lone `.dae` without clips (a prop) is
   ignored.
3. **Still empty** (folder missing): `phillip.dae`, `malik.dae` (host =
   Philip; a host without its models therefore takes two players).

hpl.log: `multiplayer: N character(s) (<source>): a.dae, b.dae, ... — the
host gives every player a different one`, plus a note when fewer than four
exist (the lobby then holds fewer players). **Adding a character** = export
`<name>.dae` + its `<name>_<clip>.dae` clips (+ `.mat`/`.tga`,
`<name>_clips.json`) into `multiplayer/models/` on **every** machine (see
above); it also raises the player cap by one, up to `max_players`. A mesh
that fails to load still leaves that player as the marker light only.

`ghost_body_ys` / `ghost_body_ys_crouch` index the final list (a cfg list
with exactly one value per `ghost_models` entry drops the values of skipped
entries so it stays aligned; shorter lists keep their modulo meaning).
`ghost_preview_model` indexes the final list too.

### More than two players

- **Map transitions.** Two doors at once are likely with four players. A
  guest whose own transition is armed no longer drops a party `MapChange`:
  it keeps the newest one and re-evaluates it on its census frame (landed on
  the same map = discarded, else it follows). The host wins every race: a
  guest `MapChange` that arrives while the host is armed, following, fading,
  or has no census for its current world yet (`IsHostMapBusy`) is neither
  applied nor relayed (`guest N level transition refused - host is
  mid-transition`), and the host re-beacons its map once settled (the census
  frame always beacons). So every `MapChange` a guest receives is the host's
  destination and the party converges on it; following never re-announces,
  so nothing can loop. A `ChangeMap` onto the map we already stand on no
  longer arms (it produced no census, so the arm stuck), and an arm with no
  fade running and no census for 5 s is dropped (`own level transition never
  landed on a new world - unarmed`).
- **Body snapshot.** `cBodySync::BuildSnapshotChunk` (the MapReady joiner's
  body poses) is read-only for the shared delta bookkeeping (`m_mapSent`).
  It used to prime `mbWasEnabled`/last pose for everybody, which swallowed the
  reliable awake->asleep rest pose for guests already connected whenever a
  3rd/4th player joined.
- **Enemy sight candidates** hold the local player + 31 ghosts (was 8).
- Party panel, HUD bars, inventory party list, co-op respawn target, voice
  streams and the name table are per-id maps/vectors with no player cap.
- The host lobby shows `Players n/max` (v17: max = `min(max_players,
  characters)`, default 4); the browser rows show the same `players/max`.

## World snapshot (v14: late join, reconnect, host save/load)

Before v14 the only "state" a joiner got was a body-pose snapshot sent at
CONNECT time — before the guest had a world, so it was silently dropped —
and nothing carried script vars, entity actives, door locks, taken/party
items or timers. Now the guest asks once it has a world, and the host answers
with everything a save game would carry for that map.

**Trigger.** The guest sends `eNetPacketType_MapReady` (24, reliable ch0,
`cNetMapReady`) at the moment its physics census is *paired* with the host's
for the world it stands in (match or not). Two hooks, one `SendMapReady()`:
(1) the frame the guest's own census is computed while the host's is already
known (menu launch via beacon, a followed `MapChange`, the guest reloading its
own save), and (2) a host census arriving while the guest already stands in
a world (reconnect while in-game, host save/load or new game on the same
map). Both run one frame after the load, i.e. after `OnStart`/`OnLoad`/
`PreUpdate`, which is exactly what the host's state must overwrite. The host
(`HandleMapReady`) refuses a stale generation or another map (the beacon
already told the guest where to go; it asks again after following) and
otherwise calls `SendWorldSnapshot(peer)`. The CONNECT-time body snapshot is
gone; host save/load, new game and death-menu Continue need no extra hook
because each one produces a new census generation.

**Content** (`eNetPacketType_WorldSnapshot` = 25, host -> one guest,
reliable ch0, chunked, `cNetSnapshotHdr` + entries, <= 1200 B payload per
chunk; every section is sent as at least one chunk so "none" and "not sent"
differ): local vars, global vars (`cNetSnapVar`); every non-enemy
`iGameEntity` (`cNetSnapEntity`: name hash, type, Active/Locked/Lit flags,
health); taken item hashes (whole session); party inventory names (host
inventory + what other guests hold); the enemy roster (`cNetEnemyState`,
same entries as the stream); the host's local timers (`cNetSnapTimer`).
Body poses travel as ordinary reliable `ObjectState` chunks between `Begin`
and `End` — same channel, so ordering is guaranteed. `Begin.mCount` =
section chunks, `End.mCount` = body chunks (both informational).

**Guest apply.** Chunks are buffered (gen guard `IsRemoteGen`, id guard, a
new `Begin` discards a half snapshot, > 10 s without `End` drops it) and
applied atomically on `End` under `gbNetScriptApplying` (RAII), so the
world's `OnUpdate` never sees a half state and nothing echoes back. Order:
vars -> entities (`SetActive`, `SetLocked`, `SetLit` with the lit-change
callback silenced, health for alive breakables; then entities present here
but absent on the host: Item/Object/Door/DoorPanel/SwingDoor/Lamp are
deactivated — that is how the host's own picks and broken objects reach a
joiner — other types are only logged) -> enemies (dead on the host: death
callback cleared, ragdoll at the host's spot; alive: puppet + target +
health) -> taken items + `ApplyTakenItems()`, party set replaced -> local
timers replaced. Never creates entities, never touches the guest's own
inventory, never applies while `mbLocalMapChangeArmed`.

**Related fixes.** (5a) `NetOnScriptEvent(RemoveItem)` erases from the
party set on the *consuming* machine too; `NetOnItemPicked` records our own
picks in the taken set. (5b) `Add*Var` double-apply: the engine now fires
the var callback *after* the increment; the host is the single authority —
it broadcasts the resulting absolute `Set`; a guest applies its own add
optimistically and forwards the delta only from a *player-driven* script
context (`cNetScriptPlayerScope` around pick/interact/examine, player
collide callbacks, inventory pickup/use/combine, message-box, numerical
panel and lamp lit-change callbacks); symmetric scripts (`OnStart`,
`OnLoad`, `OnUpdate`, timers, entity collide) stay silent on guests. The
host does not blind-relay guest Adds and the guest never runs an enemy's
death script (the host's copy did). (5c) The enemy stream is chunked
(`kMaxEnemiesPerBatch` = 40, one seq per chunk), stamped with the map
generation and gen-guarded on the guest, with a per-batch hash map instead
of a roster scan; the seq guard resets on every new census generation /
world change. Also new: `eNetScriptOp_LampLit` (9) replicates `SetLampLit`
live (mlVal bit0 = lit, bit1 = fade).

**Log lines** (hpl.log). Guest: `MapReady sent gen=N map '...' (census
C/0xXXXXXXXX)`, `world snapshot begin id=I gen=N (S section chunk(s))`,
`world snapshot end id=I: S section chunk(s), B body chunk(s) - applying`,
`world snapshot id=I applied: vars L/G, entities E (A active changed, D
deactivated: absent on host, U unknown here), doors D, lamps L, taken K,
party P, enemies N (+X dead), timers T`. Host: `guest 2 ready on gen N
(census C/0x... vs ours C/0x...)`, `world snapshot -> peer 2: C chunks, B
bytes (id=I gen=N; vars L/G, entities E, taken K, party P, enemies N,
timers T, bodies B chunks, skipped S)`. Record the chunks/bytes line on the
largest level to replace the design estimate (~25 KB in ~45 chunks).

## Security (protocol v15: a listen server on the open internet)

Before v15 the host trusted every byte from every peer: a guest could send
`PlayerJoin`/`VersionAck` and rewrite the host's own id, spawn any `.ent`
by file name, move the party to any map, deal unbounded damage, and the
discovery port answered every ping (a UDP reflector). v15 closes that so a
**port-forwarded public server** (game port + `kNetDiscoveryPort`) is an
acceptable thing to run, with a public lobby listing it.

**Join flow** (all reliable ch0, in this order):

1. ENet CONNECT carries `kNetConnectData` (magic ^ version) — an old build
   or a foreign client is refused with `kNetDisconnectBadVersion` before
   anything flows. Then the player cap (`max_players`, v17: at most one
   player per character) is enforced *at CONNECT*: every
   connected slot counts, accepted or still authenticating, so a burst of
   half-open joins cannot exceed it either (`kNetDisconnectFull`).
2. The host sends `cNetChallenge` (type **28**, a 16-byte per-connection
   nonce) and nothing else. The peer has **no wire id yet**: it is left out
   of every send loop (`PeerLive`), gets no census/beacon/name table, and
   any packet from it other than the answer is dropped (and counted).
3. The guest's FIRST packet is `cNetAuth` (type **27**): its
   `kNetProtocolVersion`, its name, and `NetAuthDigest(join_password,
   nonce, version)` — a 128-bit mix (four FNV-1a-style lanes with
   cross-lane feedback, murmur3 finalizer, two re-mix rounds; no external
   libs, same bytes on both machines). The password never travels in the
   clear and a captured answer is useless against the next nonce. It is
   *not* a cryptographic hash: with a captured (nonce, digest) pair the
   password can be guessed offline like any unsalted challenge scheme, so
   a public server wants a real password.
4. The host compares against `NetAuthDigest(server_password, nonce,
   version)`. Match: the guest gets its id (`AllocGuestId`, ids of departed
   guests are reused first, so the `uint8_t` counter can never exhaust),
   then the usual `VersionAck`, `PlayerJoin`, census, map beacon and name
   table. Mismatch, or no answer within `kNetAuthTimeoutSeconds` (5 s):
   `kNetDisconnectBadAuth`. `server_password=` empty means an **open**
   server — the exchange still runs with the empty password so there is
   one code path. The guest takes its password from `join_password=` or
   `cNetworkManager::SetJoinPassword()` (the join screen / lobby).
5. On DISCONNECT the per-peer record is erased and the id returned to the
   pool; a refused or kicked peer's later packets are dropped until its
   slot dies. The join screen shows the reason (`GetJoinFailReason`):
   wrong password / server full / kicked.

**Role gating.** `cNetworkManager::IsAllowedFrom(type, authorIsHost)` is
the one table, consulted in `Service` before any dispatch or relay: the
host drops `PlayerJoin`, `PlayerLeave`, `VersionAck`, `ObjectState`,
`BodyCensus`, `BodyGrabDeny`, `EnemyState`, `EnemyEvent`, `PlayerDamage`,
`WorldSnapshot` and `Challenge` from a guest; a guest drops `BodyGrab*`,
`BodyPush`, `EnemyDamage`, `MapReady`, `Auth` and (v18) `CharacterRequest`
from the host. Unknown
types (and the reserved `ChatMessage`) are dropped from either side. **Add
every new packet type to that table** (voice, type 29, and the v18
character request, type 30, included).

**Validation** (`ValidateEventPacket`, both roles, clamps in place so the
host relays the sanitised bytes; `ValidateGuestPacket` adds the host-only
checks). Every wire string is printable ASCII within its field.
`ItemDrop.msFile` and a guest's `MapChange.msMap` must be a *bare* file
name (no `/`, `\`, `:`, `..`, no Windows-reserved characters) ending in
`.ent` / `.dae`, and on the host must resolve through the engine's
`cFileSearcher` (the same lookup `cWorld3D::CreateEntity` /
`LoadWorld3D` use) or the packet is dropped, not relayed. All floats must
be finite; positions within 20 km; drop impulse <= 15, throw <= 30, push
<= 50 (scaled, not dropped); enemy/entity/player damage clamped to 0..200;
`ScriptEvent.mOp` must be a known op; grab mass multiplier 0.1..20; a
guest's grab target / push point must lie within 50 m of its last
validated `PlayerState` position (a guest that never sent a state can not
touch anything). v18: a `CharacterRequest` must be exactly 25 bytes with a
non-empty printable name (NUL-bounded; the host then only acts for an
accepted peer that holds a slot, see 'Characters'). A short packet of a
known type is malformed. The host's
own outgoing traffic is never validated, but guests apply the same shape
checks to what the host sends (a hostile host cannot make a guest load
`..\x.dae`).

**Strikes + rate limit.** Every violation is logged once per (peer, type)
in hpl.log — `guest N (a.b.c.d:port): dropped packet type T - why` —
and counted; `kNetMaxStrikes` (20) = `kNetDisconnectKicked`. Strikes
decay one per 5 s of clean traffic. More than `kNetMaxReliablePerSec`
(200) reliable packets from one peer in a second is a strike and the
excess is dropped for the rest of that second.

**Discovery reflector.** The host answers only pings whose magic *and*
version match (a mismatched browser no longer gets a greyed-out row; it
does not see the server), at most 5 pongs per source address per second
and 60 per second in total; the pong is one fixed-size packet, so the
port can no longer amplify.

**Left as is / known limits.** No encryption or integrity on the game
stream (ENet has none): anyone who can sniff the link can read it. A guest
with the password is still trusted for *its own* actions (it can kill an
enemy fast with 200-damage hits, or pocket items it did not reach) — the
validation bounds what a packet can do, not whether the player deserved
to. The host is fully trusted by guests for game state (by design; it owns
the simulation). No brute-force delay on the password: the 5 s timeout and
one attempt per connection are the only throttle.

## Voice chat (protocol v16)

`cVoiceChat` (`VoiceChat.h/.cpp`), owned by `cNetworkManager`, created in
`Startup()` when `voice_enabled=1` and the build has `PENUMBRA_VOICE`
(CMake: vcpkg `opus` found). Opus and OpenAL are initialised only while a
session is live (`UpdateVoice`, called from `Update` after `UpdateGhosts`)
and torn down in `Disconnect` — single-player never touches the
microphone, and everything dies before `cGame` (the AL context) does
(`cInit::Exit` deletes the network manager first).

**Send.** `VoiceTalk` (**V**, `cActionKeyboard`, registered next to the
F9-F11 actions) is polled with `IsTriggerd` every frame. On the first press
the capture device is opened lazily — `voice_capture_device` if set, else
the system default (`alcCaptureOpenDevice(NULL, 16000, AL_FORMAT_MONO16,
4096)`), then the default with a 1 s ring, then every enumerated capture
device; each ALC error is logged, and with none open the mic is retried every
5 s while the key is held (HUD hint "NO MIC") — and started
(`alcCaptureStart`, its ALC error checked). While held, `alcCaptureSamples`
drains the ring every frame (`ALC_CAPTURE_SAMPLES` counts sample frames)
into a pending PCM buffer (capped at 1 s — a frame stall while talking drops
the oldest audio, logged once) which is cut into 320-sample frames and
Opus-encoded (VOIP, 24 kbps VBR, complexity 5, wideband; at most 10 frames
per Update). Two frames go into one `cNetVoice` packet (a lone frame waits
at most 30 ms for its twin, and the last partial packet is flushed on
release), total payload <= `kNetVoiceMaxPayload` (400 B). Release stops and
drains the capture ring; the device stays open for the next press.
`voice_open_mic=1` skips the key: the mic runs continuously and frames go
out while the RMS level is above `voice_gate_db` (default -45 dBFS, 0.4 s
hold). Packets go out unsequenced on ch1 immediately (`SendUnreliableEvent`;
guest -> host, host -> every guest).

**Relay.** The host stamps the author from the **peer** id (`RelayVoice`,
the byte in the packet is never trusted — same rule as `PlayerState`),
forwards the packet to the other accepted guests (`PeerLive`) and plays it
itself. Voice passes the v15 gate like every other packet: type 29 is
both-direction in `IsAllowedFrom`, and `ValidateEventPacket` refuses a
packet whose payload exceeds `kNetVoiceMaxPayload`, whose frame count is
not 1..2, or whose frame table does not add up exactly to the packet
length (a strike on the host side, a once-logged drop on the guest side).
Voice rides ch1, so the reliable-packet rate limit never sees it.

**Receive.** `OnVoicePacket` validates the frame table (count 1..2,
per-frame `uint16` lengths within the packet) and arms the HUD "(talking)"
timer — independent of playback, so "(talking)" without sound points at the
AL side. Then per author: a stale or duplicate `mSeq` (int16 difference <= 0
while the burst is running) is dropped; a gap of 1..3 packets is concealed
with `opus_decode(NULL)` PLC frames; a bigger gap, or the first packet after
> 0.5 s of silence, resets the decoder state and the jitter buffer. Decoded
frames wait in a per-player queue (max 25); playback starts when 6 frames
(3 packets) are in, or after 150 ms for a short utterance, or as soon as the
sender goes quiet. Every frame `Update` reads the source state first, then
unqueues processed AL buffers (8 per source, 160 ms), queues waiting frames,
`alSourcePlay`s a source that is not playing (burst start, or AL_STOPPED
after an underrun), and, when the queue is empty while the sender is still
active, generates up to 5 PLC frames before letting the stream idle (the
next packet re-primes). Each stream is one `AL_FORMAT_MONO16` streaming
source placed every frame at the ghost's render feet + 1.6 m
(`SetRemoteHeadPos`, `AL_SOURCE_RELATIVE` off). While no head position is
known (ghost not spawned yet, or none for 0.5 s: another map) the source is
listener-relative at the origin — centered, at `voice_volume`, never silent
because a ghost is missing. The gain is computed in software —
`voice_volume * ref / (ref + (clamp(d, 2, 25) - 2))`, ref 2 m, max 25 m,
`d` measured against the AL listener (the engine's camera) — because the
engine runs the AL context with the distance model set to NONE
(`LowLevelSoundOpenAL.cpp`, it attenuates its own sounds itself); AL still
pans by position. Our sources get `AL_ROLLOFF_FACTOR 0`, a no-op under every
AL distance model, so a future global-model change cannot double-attenuate.
(Until this fix the source was "pinned" with `alSourcei(src,
AL_SOURCE_DISTANCE_MODEL, AL_NONE)`: 0x200 is the `alEnable` capability, not
a source property — OpenAL Soft raised AL_INVALID_ENUM, the setup check
freed the stream, and no remote voice ever played.)

**Errors.** Source/buffer creation is judged by the names AL returns
(`alIsSource` / `alIsBuffer`), not by `alGetError` alone: the wrapper's
stream thread shares the context's error slot. A failed creation (e.g. no
free source: the engine reserves its own — Init probes one and warns) is
logged once and retried for that player every 2 s. A per-frame AL error is
logged once and tolerated; the stream is rebuilt only when its source is
gone or errors persist 10 frames, and after three rebuilds PLAYBACK is off
for the session (hint "VOICE OFF") — the microphone keeps sending. No AL
context / no Opus encoder at Init = voice off for the session.

**Diagnostics.** Every stage logs once to `hpl.log` (` voice:` lines): the
AL context/device and its mono-source count, the first push-to-talk press
(proves the `VoiceTalk` action works; warns when not joined yet), the
capture device list, the
device opened (or each ALC error), capture started, first samples (or "NO
samples" after 1 s = blocked by the Windows microphone privacy switch), the
first packet handed to the network, the first three sent bursts with the
mic level (pure digital silence is called out), the first packet received
from each player, stream created, playback started (distance, gain,
listener gain), the first underrun, the first three received bursts per
player (packets, frames played, PLC, underruns, distance, gain) and a
per-stream summary on close.

**HUD.** `IsPlayerTalking(id)` (packet within the last 0.35 s; our own id
= mic live) and `IsMicOpen()` feed `cPlayer::DrawPartyPanel`: `(talking)`
after a friend's name, `[MIC]` on our line. `cVoiceChat::GetStatusHint()`
returns "NO MIC" / "MIC SILENT" / "VOICE OFF" (or NULL) for our line.

**Cfg.** `voice_enabled=1`, `voice_volume=1.0` (0..2), `voice_open_mic=0`,
`voice_gate_db=-45` (-70..-10), `voice_capture_device=` (empty = default).
Wire: `eNetPacketType_Voice` = 29 (27/28 belong to the v15 auth
handshake), `cNetVoice` = 5 bytes, constants `kNetVoice*` in
`NetworkPackets.h`.

## Game glue (outside this folder — edit carefully)

| Location | What |
|----------|------|
| `../Init.cpp` / `Init.h` | Owns `mpNetworkManager`; `Startup()` after input exists; `Update()` each frame; config port load/save. |
| `../Player.cpp` / `Player.h` | `DrawPartyHud()` — world-anchored party health bars in `OnDraw`; `DrawPartyPanel()` — top-left names/health panel + event feed (v13), `(talking)` / `[MIC]` voice indicators (v16), `(<character>)` after each name (v17; v18: display name, `Deadl (The Fisherman)`). |
| `../PlayerHelper.cpp` / `.h` | `cPlayerDeath` co-op respawn branch (`CoopRespawnApplies`, `UpdateCoopRespawn`). |
| `../Inventory.cpp` / `.h` | `DrawParty()` — inventory party health list. |
| `../GameEnemy.cpp`, `../TriggerHandler.cpp` | Host senses read `GetGhostHealth` (focus health, sight candidates, ghost footsteps). |
| `../MainMenu.cpp` / `MainMenu.h` | Multiplayer menu states, host/join UI, IP typing widget (`#ifdef PENUMBRA_MULTIPLAYER`); v13 username screen (`eMainMenuState_MultiplayerName`, the typing widget's name mode, "Change name"); v18 character picker (`cMainMenuWidget_MultiCharacter` on the Multiplayer, Direct-connect and host lobby screens). |
| `../GameScripts.cpp` / `.h` | Script hooks (`NetOnScriptEvent`), `NetApplyScriptEvent`, `gbNetScriptApplying` / `gbNetScriptPlayerContext` + `cNetScriptPlayerScope` (v14). |
| `../GameEntity.cpp`, `../Player.cpp`, `../Inventory.cpp`, `../GameMessageHandler.cpp`, `../NumericalPanel.cpp`, `../GameLamp.cpp` | `cNetScriptPlayerScope` around the player-driven `RunScriptCommand` sites (v14 Add*Var authority). |
| `../GameSwingDoor.h`, `../GameLamp.h`, `../Inventory.h`, `../MapHandler.h` | `IsLocked()`, `IsLit()`/`GetLitChangeCallback()`, `friend class cNetworkManager` (party items, local timers) for the world snapshot. |
| `../../HPL1Engine/sources/game/ScriptFuncs.cpp` | `gpScriptVarNetCallback` — fired for var writes; Add ops fire AFTER the increment (v14). |
| `../MainMenu.cpp` / `MainMenu.h` | Multiplayer menu states, host/join UI, IP typing widget (`#ifdef PENUMBRA_MULTIPLAYER`). |
| `../CMakeLists.txt` | Globs `multiplayer/*.cpp`, links ENet, defines `PENUMBRA_MULTIPLAYER`; option `PENUMBRA_VOICE` (default ON): finds `Opus::opus`, defines `PENUMBRA_VOICE=1`, links opus + `OpenAL::OpenAL` (v16). |
| `../vcpkg.json` | Declares `enet`, `opus` (+ SDL/OpenAL audio deps). |

## Runtime controls

- **F11** — toggle host on default port (7777).
- **F10** — join `127.0.0.1:<port>`.
- **F9** — LAN/Hamachi discovery scan (~1.5s window; results logged, feed the server browser).
- **V** (hold) — push-to-talk voice chat (v16); heard from your character's head, fading with distance.
- **Menu** — Multiplayer → Host / Server browser (Internet · LAN) / Direct connect / Change name (asks for a username the first time).
- **`multiplayer.cfg`** — `player_name=` (v13, written by the menu), `character=` (v18, the guest's preferred character by file base name, written by the menu's picker), `host=1`, `join=HOST:PORT`, `port=`, `server_name=` (empty = `<player_name>'s game`), `max_players=` (enforced at CONNECT since v15; v17: capped at the number of characters), `server_password=` / `join_password=` (v15, see Security), `ghost_models=a.dae,b.dae` (optional: without it the characters in `multiplayer/models` are auto-discovered, see 'Characters'), `ghost_body_y=` / `ghost_body_ys=` (offsets from the feet, default 0), `coop_respawn=1`, `voice_enabled=1`, `voice_volume=1.0`, `voice_open_mic=0` (v16), `voice_gate_db=-45`, `voice_capture_device=` (open mic threshold, microphone by name), `ghost_interp_ms=100`, `ghost_turn_rate=720`, `ghost_gait_walk|run|crouch_walk|walk_back|strafe_walk|strafe_run=` (m/s), `ghost_anim_trace=0|1`, `ghost_preview=0|1`, `ghost_preview_model=0`, `master_server=HOST:PORT`, `public=0|1` (internet browser, see 'Public servers'). See `multiplayer.cfg.example`.

### Ghost preview (offline animation check)

`ghost_preview=1` spawns a local ghost (`ghost_preview_model` picks the
character-list entry, 0-based) 2 m in front of the player, facing them, feet on the
player's feet height, as soon as a map is up — no session needed. It is fed
synthetic states through the same `ApplyState`/`Update` path as a network
ghost, so interpolation, selection, crossfades and gait scaling are what a
peer would see. Keys while it exists (one hpl.log line per change):

- **F6 / F7** — next / previous clip (cycles every loaded clip; one-shots replay every second).
- **F8** — toggle crouch stance (plays `stand_to_crouch` / `crouch_to_stand`).
- **F2** — treadmill off → walk → run: the ghost walks a 1 m circle at the real `game.cfg` movement speeds, with automatic clip selection and speed scaling.

`ghost_anim_trace=1` additionally logs every clip change and a 5-second
state trace (active states, weight sum, wire vs render speed) per ghost.

## Discovery (server browser backend)

Raw UDP (not ENet) on `kNetDiscoveryPort` **7778** — a fixed side port, NOT
SO_REUSEADDR on the game port (on Windows, unicast to a twice-bound port is
bind-order dependent and could steal ENet traffic; see NetworkPackets.h).
Browser broadcasts a ping to 255.255.255.255, 127.0.0.1, and every
interface's directed broadcast (`ip | ~mask` via GetAdaptersInfo) — the last
one is what makes Hamachi/Radmin/ZeroTier work, since the virtual LAN is its
own interface and the global broadcast usually picks the wrong NIC. Hosts
answer with name / map / players / real game port. Since v15 a host answers
only pings of its own protocol version (`mbVersionMatch` on the browser
side is kept for pongs from older hosts) and rate-limits its pongs per
source address and in total (see Security).

## Public servers (internet server browser)

LAN discovery cannot cross the internet, so the **Internet** tab of the
server browser asks a tiny *master server* instead. There is no default
public master: nothing is listed anywhere unless a host opts in with a
`master_server=` line (the built-in default `master.example.invalid:7779`
is an RFC 2606 name that resolves to nothing — replace it in
`NetworkPackets.h` / `kNetMasterDefaultHost` if you run a community master).

**Run a master** — `tools/master_server.py` is one Python 3 file, stdlib
only. Run it on any VPS, or on your own PC with **udp/7779** forwarded:

```
python3 master_server.py                       # udp/7779, logs to stdout
python3 master_server.py --port 7779 -v        # log every beacon / list
python3 master_server.py --dump --host 1.2.3.4 # print a running master's table
```

It keeps servers keyed by (source IP, game port), expires them 60 s after
the last beacon, answers List requests with the live set (newest first, at
most 100, 10 per datagram), rate-limits List replies to 5/s per source IP,
and silently drops anything malformed. A systemd unit is in the file's
docstring. The master never relays game traffic — it only hands out
addresses.

**Host a public game** — in `multiplayer.cfg`:

```
master_server=1.2.3.4:7779     # your master (implies public=1)
public=1                       # or use the 'Public' checkbox in the host lobby
server_password=secret         # optional; the browser shows [pw]
```

and **forward your game port** (default **udp/7777**, key `port=`) on your
router to the PC that hosts. The host lobby shows the port to forward and
whether the server is currently listed. While hosting the game beacons a
`Register` every 20 s from the discovery socket (game port, players, max,
name, map, password flag, protocol version) and sends `Unregister` when you
stop. The master records the beacon's *source* IP, so the host never needs
to know its own public address.

**Join** — Multiplayer → Server browser → Internet → Refresh. Rows show
name, map, players/max (red when full), a padlock for password servers and
how long ago the host last beaconed ("Seen"); rows from another mod version
are greyed out. A click selects a row; **Join**, Enter or a double-click
joins it (Up/Down select, PgUp/PgDn page past 8 rows, F5 refreshes).
Joining a padlocked row asks for the password first (that sets the join
password for that attempt; `join_password=` in the cfg covers Direct
connect). **Direct connect** (type `ip:port`) still works exactly as before
for friends who share an address over chat, Hamachi or Radmin.

**Wire format** — `NetworkPackets.h` 'MASTER SERVER protocol' (magic
`PNMS`, version 1, little-endian, IPv4 as 4 raw bytes). It is separate from
the game protocol: `kNetProtocolVersion` is *carried* in the packets, never
changed by them. `master_server.py` is the reference implementation.

## Explicit non-goals

Cross-endian packets, dedicated servers, more than a handful of players
(ENet host sized for 31 peers), and any lag compensation beyond the ghost
interpolation delay.

## Suggested next work

1. Head/body yaw split for ghosts (`turn_l`/`turn_r` are loaded but not driven) and a 2-clip directional blend for diagonal movement.
2. Map name handshake so join fails clearly if worlds differ.
3. Extract MainMenu multiplayer UI widgets into this folder if the menu file stays too large.

## Build reminder

Sibling layout: `PenumbraDev/{HPL1Engine,OALWrapper,dependencies,PenumbraOverture-master}`. Configure with vcpkg toolchain so `enet` resolves. Win32 MSVC deps need `Newton.lib` under `dependencies/lib/win32`.
