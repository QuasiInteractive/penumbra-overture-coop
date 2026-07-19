# Penumbra: Overture — Co-op Mod

**Two-player online co-op for Penumbra: Overture**, by [Quasi Interactive](https://quasi-interactive.com).

One of you hosts, the other joins, and you play the campaign together in one
shared world: shared physics, shared items, party level transitions, loot
handovers — you can even hand your friend the pickaxe.

**[Download the ready-to-play zip from Releases →](../../releases)** — extract it
into your game's `redist` folder and the Multiplayer entry appears in the main
menu. You need to own Penumbra: Overture (Steam).

## Features (v0.6)

- See each other as animated characters (walk/run/crouch/jump, flashlights)
- One-click joining: connect, auto-launch into the host's map, spawn at their side
- Host lobby with live friend counter, "Launch new game" or "Load a save" —
  everyone launches together
- Host-authoritative shared physics: crates, barrels, doors, drawers; grab
  things out of each other's hands, throw things to each other
- Tuned for high ping (guest-side smoothing + held-object prediction)
- One-of-each items and drag-out-of-inventory loot dropping
- Party level transitions: one player takes an exit, everyone follows
- Borderless fullscreen at native resolution

### Not yet synced (roadmap)

- Enemy AI (each player currently has private enemies) — next big milestone:
  host-authoritative shared AI with nearest-target selection
- Script/puzzle state mirroring

## Repository layout

| Path | What |
|------|------|
| `Overture/` | The game code (Frictional Games' GPL release + the co-op mod). All multiplayer code lives in `Overture/multiplayer/`. |
| `HPL1Engine/` | The HPL1 engine (GPL release, with small co-op-supporting changes such as borderless fullscreen). |

## Building (Windows)

Visual Studio (x86) + CMake:

```
cd Overture
cmake -S . -B build_win32 -A Win32 -DPENUMBRA_MULTIPLAYER=ON
cmake --build build_win32 --config Release --target Overture
```

The engine is picked up automatically from the sibling `HPL1Engine/` directory.
Copy the built `overture.exe` (plus the files from the release zip: OpenAL32.dll,
`multiplayer/` models, `multiplayer.cfg`) into the game's `redist` folder.

## License & credits

- Mod by **Quasi Interactive** — <https://quasi-interactive.com>
- Penumbra: Overture and the HPL1 engine are © Frictional Games, released under
  the **GNU GPL** (see `Overture/COPYING` and `HPL1Engine/COPYING`); this mod is
  a modified build of that source, published under the same license.
- Game assets are not included — you need to own the game.
