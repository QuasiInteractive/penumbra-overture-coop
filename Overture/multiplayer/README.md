# Penumbra Overture — Co-op Multiplayer

All dedicated multiplayer source lives in this folder. Work here first; game glue is listed below.

## Files in this folder

| File | Role |
|------|------|
| `NetworkPackets.h` | Packed ENet payloads (player state, bodies, enemies, script/item events, join/leave) + discovery ping/pong, protocol magic/version (`kNetProtocolVersion`), `kNetSendPeriodSeconds`, `kNetDiscoveryPort`. |
| `NetworkManager.h` / `.cpp` | Listen-server host, client join, 30 Hz state relay, LAN/Hamachi discovery (raw UDP broadcast), `multiplayer.cfg`, F9/F10/F11, per-frame ghost updates, the ghost preview mode. |
| `BodySync.h` / `.cpp` | Shared physics: host-authoritative body replication. Name-hash identity, map-load census (host/guest verify), host state batches, guest apply. |
| `GhostPlayer.h` / `.cpp` | Remote peer visuals (skinned mesh + marker light + flashlight). Interpolation buffer on the sender's clock, clip selection from the wire velocity/flags, weight-preserving crossfades, gait-scaled playback. Not a real `cPlayer`. |
| `multiplayer.cfg.example` | Copy next to `overture.exe` as `multiplayer.cfg`. |
| `models/` | Ghost `.dae` meshes + `<name>_<clip>.dae` clips + `<name>_clips.json` (gait table); runtime resource dir is `multiplayer/models` (cwd = exe folder). |

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
`kNetProtocolVersion` (currently **12**), and the connect handshake refuses
mismatched builds.

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
  and draws a 60x6 bar (red..green) + `P<id>` (`P<id> dead`), hidden behind
  the camera or beyond 25 m, fading from 15 m; not drawn over the inventory,
  notebook, panel or death menu.
- **Inventory** — `cInventory::DrawParty()` lists every connected player
  with health and a bar in the free column right of the slot grid.

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

## Game glue (outside this folder — edit carefully)

| Location | What |
|----------|------|
| `../Init.cpp` / `Init.h` | Owns `mpNetworkManager`; `Startup()` after input exists; `Update()` each frame; config port load/save. |
| `../Player.cpp` / `Player.h` | `DrawPartyHud()` — world-anchored party health bars in `OnDraw`. |
| `../PlayerHelper.cpp` / `.h` | `cPlayerDeath` co-op respawn branch (`CoopRespawnApplies`, `UpdateCoopRespawn`). |
| `../Inventory.cpp` / `.h` | `DrawParty()` — inventory party health list. |
| `../GameEnemy.cpp`, `../TriggerHandler.cpp` | Host senses read `GetGhostHealth` (focus health, sight candidates, ghost footsteps). |
| `../MainMenu.cpp` / `MainMenu.h` | Multiplayer menu states, host/join UI, IP typing widget (`#ifdef PENUMBRA_MULTIPLAYER`). |
| `../CMakeLists.txt` | Globs `multiplayer/*.cpp`, links ENet, defines `PENUMBRA_MULTIPLAYER`. |
| `../vcpkg.json` | Declares `enet` (+ SDL/OpenAL audio deps). |

## Runtime controls

- **F11** — toggle host on default port (7777).
- **F10** — join `127.0.0.1:<port>`.
- **F9** — LAN/Hamachi discovery scan (~1.5s window; results logged, feed the server browser).
- **Menu** — Multiplayer → Host / Join.
- **`multiplayer.cfg`** — `host=1`, `join=HOST:PORT`, `port=`, `server_name=`, `max_players=`, `ghost_models=a.dae,b.dae`, `ghost_body_y=` / `ghost_body_ys=` (offsets from the feet, default 0), `coop_respawn=1`, `ghost_interp_ms=100`, `ghost_turn_rate=720`, `ghost_gait_walk|run|crouch_walk|walk_back|strafe_walk|strafe_run=` (m/s), `ghost_anim_trace=0|1`, `ghost_preview=0|1`, `ghost_preview_model=0`. See `multiplayer.cfg.example`.

### Ghost preview (offline animation check)

`ghost_preview=1` spawns a local ghost (`ghost_preview_model` picks the
`ghost_models` entry) 2 m in front of the player, facing them, feet on the
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
answer with name / map / players / real game port; version-mismatched servers
are listed with `mbVersionMatch=false` so the UI can grey them out.

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
