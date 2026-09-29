# Penumbra: Overture co-op — project handoff and changelog

Read this at the start of a new chat about the co-op mod. It explains where
everything is, how to build and test it, what has been done so far (in
order, with the reasons), what is still open, and where to look when
something breaks. `CLAUDE.md` holds the short working rules;
`Overture/multiplayer/README.md` is the full technical reference (protocol,
every subsystem, every `multiplayer.cfg` key).

Last updated: 2026-09-29, protocol **v19**, head commit `670ccd2`.

---

## 1. Where everything is

| What | Where |
|---|---|
| GitHub repo | `quasiinteractive/penumbra-overture-coop` |
| Working branch | `claude/serene-ptolemy-3bzkij` (all work is here; no PR yet) |
| Local clone (Windows) | `C:\PenumbraDev\gitpull` |
| Game install, host copy | `D:\SteamLibrary\steamapps\common\Penumbra Overture\redist` |
| Game install, guest copy (second windowed instance for local tests) | `D:\SteamLibrary\steamapps\common\Penumbra Overture\redist_guest` |
| Release package for friends | `C:\Users\Deadl\Downloads\po models\new version of penumbra coop` |
| OALWrapper | `C:\PenumbraDev\OALWrapper` |
| Engine dependency bundle | `C:\PenumbraDev\dependencies` |
| vcpkg | `C:\vcpkg` |
| Build output | `C:\PenumbraDev\gitpull\Overture\build_win32\Release\overture.exe` |
| Game log (both copies) | `<redist>\hpl.log` — multiplayer lines start with ` multiplayer:`, voice lines with ` voice:` |

Inside the repo:

| Path | What it is |
|---|---|
| `Overture/multiplayer/` | All multiplayer code: `NetworkManager.*` (sessions, packets, lobby, characters, security), `NetworkPackets.h` (wire format + protocol version), `BodySync.*` (physics sync), `GhostPlayer.*` (the other players' animated bodies), `VoiceChat.*` (proximity voice) |
| `Overture/multiplayer/README.md` | Full technical reference |
| `Overture/multiplayer/multiplayer.cfg.example` | Every config key with comments |
| `Overture/multiplayer/models/` | The four characters: `phillip`, `fisherman`, `red`, `malik` — each a base `.dae` + `.mat` + `.tga`, 15 animation clips (`<name>_<clip>.dae`) and `<name>_clips.json` (walk/run speeds). ~77 MB |
| `Overture/multiplayer/tools/` | `hpl_dae_export.py` (FBX/DAE -> HPL mesh), `bvh_to_hpl_clip.py` (animation clips), `build_character.ps1` (one command: FBX -> ready character), `anim_viewer.html` (look at clips in a browser), `master_server.py` (internet server list), `syntax_check.sh` (Linux-only compile check) |
| `Overture/multiplayer/tools/README_characters.md` | How to add or rebuild a character |
| `Overture/multiplayer/tools/README_animations.md` | How to make or change animation clips |
| `Overture/*.cpp` | Game code; co-op changes sit behind `#ifdef PENUMBRA_MULTIPLAYER` (menu in `MainMenu.cpp`, party panel in `Player.cpp`, items in `Inventory.cpp`, enemies in `GameEnemy*.cpp`, respawn in `PlayerHelper.cpp`) |
| `HPL1Engine/` | The engine (one change: smoother quaternion blending in `sources/math/Math.cpp`) |

---

## 2. Get, build, install, package

**Get the latest:**
```
cd C:\PenumbraDev\gitpull
git pull origin claude/serene-ptolemy-3bzkij
```

**Build** (PowerShell; the policy flag must be quoted):
```
cd C:\PenumbraDev\gitpull\Overture
cmake -S . -B build_win32 -A Win32 -DPENUMBRA_MULTIPLAYER=ON "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake -DOAL_ROOT=C:\PenumbraDev\OALWrapper -DPENUMBRA_DEPENDENCIES_ROOT=C:\PenumbraDev\dependencies
cmake --build build_win32 --config Release --target Overture
```
Only the second command is needed after the first configure (re-run the
first after `vcpkg.json` changes).

**Install for a local two-player test:**
1. Copy `build_win32\Release\overture.exe` (and any new DLL from the build
   folder, e.g. `opus.dll`) into both `redist` and `redist_guest`.
2. Copy `Overture\multiplayer\models\*.dae`, `*.mat`, `*.tga`, `*.json` into
   `<redist>\multiplayer\models\` for both copies, and **delete any
   `*.collcach` files there** (the engine prefers stale caches).
3. Each copy has its own `multiplayer.cfg`:
   - host: `host=1`, `player_name=...`, `force_windowed=1`, `window_x=`/`window_y=`
   - guest: `join=127.0.0.1:7777`, `player_name=...`, `force_windowed=1`, other window position
   - Do **not** keep a `ghost_models=` line (not needed; characters are auto-discovered).

**Release package for friends** (`C:\Users\Deadl\Downloads\po models\new version of penumbra coop`):
same layout as a redist folder — `overture.exe` + the build's DLLs at the
root, `multiplayer\models\*`, `multiplayer.cfg.example`, `master_server.py`,
and an `INSTALL.txt`. The friend copies it over their own
`Penumbra Overture\redist`. Everyone must run the same build (the protocol
version must match or the connection is refused).

---

## 3. How to play / test

- **Menu → Multiplayer**: Host, Server browser (Internet / LAN tabs), Direct
  connect (`ip:port`), Change name. The first time you get asked for a username.
- **Host is always Philip.** Guests get The Fisherman, Red or Malik (one of
  each, no duplicates, max 4 players) and can change with the
  `Character: < X >` picker or the character buttons under it.
- **Keys:** hold **V** = push-to-talk; **X** = holster a weapon; F11 = quick
  host on port 7777; F10 = quick join 127.0.0.1; F9 = LAN scan.
- **Ports:** game UDP **7777** (forward it on the host's router for internet
  play, or use Hamachi/Radmin). Master server UDP **7779**.
- **Internet server list** needs someone to run `master_server.py` (Python,
  no installs) on a PC/VPS with udp/7779 open, and every player's
  `multiplayer.cfg` to have `master_server=HOST:7779`; the host also sets
  `public=1`. Without it, use LAN or Direct connect.
- **Passwords:** host `server_password=`, guest types it in the browser
  (or `join_password=` for Direct connect).

---

## 4. Changelog (oldest first)

### Before this work (July 2026)
- v0.6 and v0.10: the original co-op mod — two players, host-authoritative
  physics, shared enemies, script/puzzle sync, breakable sync, basic
  internet hardening.

### Animations and ghosts (the other players' bodies)
- **Linux syntax-check harness** so code can be checked without Windows.
- **New animation pipeline:** Motifect mocap locomotion clips converted to
  HPL with `bvh_to_hpl_clip.py`; `anim_viewer.html` to preview `.dae` clips
  in a browser.
- **Engine fix:** near-identical quaternion keys are blended smoothly (no
  more twitching).
- **Ghost animation rewrite:** per-frame interpolation keyed to the
  sender's clock, crossfades between clips, clip speed matched to how fast
  the player actually moves, `ghost_preview=1` to watch a ghost offline.
- **Feedback fixes:** less "fruity" hip sway (`hip_sway` damping), running
  uses `run_sprint` (arms pump instead of hugging the chest), crouch idle
  is a breathing hold instead of a deep kneel, turn clips don't drift.

### Enemies
- **Every enemy hunts the nearest player** (it used to only chase the
  host); guests make footstep noise so enemies hear them.

### Build fixes (Windows)
- OALWrapper headers via `-DOAL_ROOT`, OpenAL headers from vcpkg,
  `CMAKE_POLICY_VERSION_MINIMUM=3.5` for CMake 4.x.
- `window_x` / `window_y` / `force_windowed` for two windowed copies side by side.

### Party features
- **v12 health:** your friends' health on the wire, health bars over their
  heads, co-op respawn (a dead player comes back while a friend lives).
- **v13 usernames:** asked the first time you open Multiplayer; party panel
  top-left (`Name (Character) (host)`); join/leave messages.
- **v14 world snapshot:** a player who joins late (or reconnects) gets the
  current world — doors, levers, items, script variables, enemies; works
  after the host loads a save.

### Security and the lobby
- **v15 security:** challenge/password handshake, packets only accepted
  from the right side (host vs guest), size/range validation, strikes and
  kicks for bad packets, rate limits.
- **Internet server browser** + `master_server.py`; LAN browser.
- **v16 proximity voice chat:** Opus over the network, push-to-talk on V,
  heard from the speaker's head and fading with distance.
- **Review fixes:** camera angle wrapping (was kicking guests), open
  servers no longer refuse guests with a leftover password.

### Four-player lobbies and characters
- **Character auto-discovery** in `multiplayer/models`; >2-player fixes.
- **Character pipeline:** `build_character.ps1` turns a rigged FBX
  (Meshy rig) into a ready character (decimated to ~12k triangles,
  textures, materials, all 15 clips).
- **v17 one of each:** the host hands every player a different character;
  the player cap = number of characters (4).
- **New models:** new Philip (red parka) and Malik, plus The Fisherman and
  Red (originally "Stefan", renamed).
- **v18 character picker:** guests choose their character (menu line
  `Character: < X >`, saved as `character=` in `multiplayer.cfg`).

### After the first real internet test (2026-09-29)
Reported: barrel "break dancing", objects spinning / going through walls /
floating, no voice, hammer stuck in hand, host shown as Malik, browser
looks bad. All fixed (not yet re-tested in game):
- **v19 characters by name** (`d934ff7`): the host was shown as Malik
  because each machine's character list was in a different order (e.g. an
  old `ghost_models=malik.dae,phillip.dae`). Characters now travel by name,
  every list is ordered Philip, Fisherman, Red, Malik, and Philip is never
  offered to a guest.
- **Stuck hammer** (`84259ab`): dropping the equipped weapon left the
  player in "weapon mode" with nothing to switch it off, so they couldn't
  grab anything. Dropping or losing a held item now holsters it, plus a
  per-frame safety check. (`Inventory.cpp`, `PlayerState_Weapon*.cpp`)
- **Server browser redesign** (`4a385c2`): framed panel, tabs, columns,
  padlock for passwords, full servers in red, status line, pages past 8
  servers, Join / Direct connect / Back buttons, nicer password screen.
  A click selects a row; Join / Enter / double-click joins. Picker has
  `<` `>` arrows and character buttons (host / taken / free). Also fixed a
  possible crash from a `%` in a server name.
- **Voice** (`414249e`): nothing ever played — an invalid OpenAL setting
  made every incoming voice stream shut itself down. Fixed; microphone
  opening now falls back through several devices; new `hpl.log` lines at
  every step; your party-panel line shows `[NO MIC]` / `[MIC SILENT]` /
  `[VOICE OFF]`. New keys `voice_gate_db=-45`, `voice_capture_device=`.
- **Physics sync** (`670ccd2`): guests teleported objects to the host's
  position every frame, pushing them into walls and each other (the
  break dancing and spinning), through walls, and freezing them mid-air.
  Objects are now pushed with velocity so walls stop them; they only jump
  when far off or stuck; resting objects settle exactly; ragdoll bones are
  no longer synced; objects the host snatches keep their real weight and
  gravity.

---

## 5. Open items / next steps

1. **Re-test everything from section 4's last part** with two machines
   (all v19). Watch: objects near walls, thrown barrels, doors; voice
   (check ` voice:` lines in `hpl.log` on both sides); dropping the hammer;
   character picker; server browser.
2. **Package DLL check:** the friend's package may lack the ogg/vorbis
   DLLs. Run `dumpbin /dependents overture.exe` (VS Developer prompt) in
   the package folder and make sure every listed DLL is there.
3. **Ping column** in the browser currently shows "Seen" (seconds since the
   host was last heard) — a real ping would need a small NetworkManager
   addition.
4. **Physics design gap:** a guest's own body can push objects locally
   that the host doesn't know about; they get corrected back, but a
   cleaner fix is to forward those pushes to the host.
5. **Tuning to watch** in `BodySync.cpp`: `kDriveTime` 0.1,
   `kDisturbGrace` 0.5, `kKeyframesPerTick` 2.
6. Maybe a hint "Press X to holster" when a player tries to grab while
   holding a weapon.

---

## 6. When something breaks

- Ask for **both** `hpl.log` files (host and guest). Useful lines:
  ` multiplayer:` (joins, characters, security drops, physics census),
  ` voice:` (mic device, first packet sent/received, playback started,
  underruns).
- "Refused / version mismatch" = the two builds differ; rebuild and recopy
  `overture.exe` on both.
- Character wrong on one screen = that machine is missing a character's
  files in `multiplayer\models` (the log says which).

## 7. Rules for changing the code

See `CLAUDE.md`. In short: any wire-format change bumps
`kNetProtocolVersion` in `NetworkPackets.h` (with a history line and the
`static_assert` sizes) and new packet types must be added to
`IsAllowedFrom` + `ValidateEventPacket` in `NetworkManager.cpp`; keep
single-player identical (gate on a live session); both the multiplayer and
the stub build must compile.
