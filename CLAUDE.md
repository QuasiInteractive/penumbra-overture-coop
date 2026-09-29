# Penumbra: Overture co-op — working notes

Two-player (and more) online co-op mod for Penumbra: Overture on the HPL1 engine.
All multiplayer code is in `Overture/multiplayer/`; game glue is spread through
`Overture/*.cpp` behind `#ifdef PENUMBRA_MULTIPLAYER`. Read
`Overture/multiplayer/README.md` first — it documents the protocol, every
subsystem and the `multiplayer.cfg` keys.

## Local layout (Windows)
- Repo: `C:\PenumbraDev\gitpull` (branch `claude/serene-ptolemy-3bzkij`)
- OALWrapper: `C:\PenumbraDev\OALWrapper`, deps bundle: `C:\PenumbraDev\dependencies`
- vcpkg: `C:\vcpkg`
- Game: `D:\SteamLibrary\steamapps\common\Penumbra Overture\redist` (host) and
  `...\redist_guest` (second windowed instance for local two-player tests)

## Build
```
cd C:\PenumbraDev\gitpull\Overture
cmake -S . -B build_win32 -A Win32 -DPENUMBRA_MULTIPLAYER=ON "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake -DOAL_ROOT=C:\PenumbraDev\OALWrapper -DPENUMBRA_DEPENDENCIES_ROOT=C:\PenumbraDev\dependencies
cmake --build build_win32 --config Release --target Overture
```
PowerShell needs the policy flag quoted. Re-run configure after vcpkg.json changes.

## Install for testing
Copy `build_win32\Release\overture.exe` (+ any new vcpkg DLL such as `opus.dll`)
into both redist folders; copy `Overture\multiplayer\models\*.dae` and `*.json`
to `<redist>\multiplayer\models\` and delete `*.collcach` there (the engine
prefers stale caches). Each redist has its own `multiplayer.cfg`
(`host=1` vs `join=127.0.0.1:7777`, `player_name=`, `force_windowed=1`,
`window_x/window_y`). Logs: `<redist>\hpl.log`, multiplayer lines start with
` multiplayer:`.

## Rules of thumb
- Protocol changes: bump `kNetProtocolVersion` in `NetworkPackets.h`, add a
  history line, update the `static_assert` sizes, and add new packet types to
  `IsAllowedFrom` + `ValidateEventPacket` in `NetworkManager.cpp` (the v15
  security gate drops anything it does not know).
- Keep single-player byte-identical: gate new behaviour on a live session.
- Both builds must compile: real (`PENUMBRA_MULTIPLAYER`) and the stub branch
  in `NetworkManager.cpp`.
- Animations: regenerate clips with `Overture/multiplayer/tools/bvh_to_hpl_clip.py`
  (see `tools/README_animations.md`); inspect with `tools/anim_viewer.html`.
- `Overture/multiplayer/tools/syntax_check.sh` is a Linux-only syntax check with
  stub headers; on Windows the real MSVC build replaces it.
