# Penumbra: Overture — Co-op Mod

**Two-player online co-op for Penumbra: Overture**, by [Quasi Interactive](https://quasi-interactive.com).

One of you hosts, the other joins, and you play the campaign together in one
shared world: shared physics, shared items, party level transitions, loot
handovers — you can even hand your friend the pickaxe.
Only way to current join each other is via Hamachi or RadminVPN. or any type of virtual lan server.

**[Download the ready-to-play zip from Releases →](../../releases)** — extract it
into your game's `redist` folder and the Multiplayer entry appears in the main
menu. You need to own Penumbra: Overture (Steam).

## Features (v0.10)

- See each other as animated characters (walk/run/crouch/jump, flashlights)
- One-click joining: connect, auto-launch into the host's map, spawn at their side
- Host lobby with live friend counter, "Launch new game" or "Load a save" —
  everyone launches together
- Host-authoritative shared physics at 30 Hz: crates, barrels, doors, drawers;
  grab things out of each other's hands, throw things to each other
- **Shared enemies**: the host runs the one true AI, everyone sees the same
  wolves — and they hunt whoever is closest, host or guest. Guest hits land
  for real; deaths replicate.
- **Script & puzzle sync**: script variables, door locks, entity triggers and
  item consumption replicate — gated doors open for the whole party
- **Party inventory**: multi-item gates (torch + glowstick...) pass when the
  party *collectively* holds the items, however the pickups were split
- Breakable objects (pickaxed doors, boards) break for everyone
- One-of-each items and drag-out-of-inventory loot sharing
- Party level transitions: one player takes an exit, everyone follows
- Hardened for real internet: version-gated handshake, out-of-order packet
  rejection, animation smoothing at jitter
- Borderless fullscreen at native resolution

### Known limits (roadmap)

- Spider/worm set-pieces stay single-target on the host (deliberate — their
  scripted sequences assume vanilla senses); they still position-sync
- Enemies cannot *hear* guests yet, only see them
- Notebook/journal entries are per-player by design

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
