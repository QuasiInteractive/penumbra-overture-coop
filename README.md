# Penumbra: Overture — Co-op Mod

**Two-player online co-op for Penumbra: Overture**, by [Quasi Interactive](https://quasi-interactive.com).

One of you hosts, the other joins, and you play the campaign together in one
shared world: shared physics, shared items, party level transitions, loot
handovers — you can even hand your friend the pickaxe.
Only way to join each other currently is via Hamachi or RadminVPN. or any type of virtual lan server.

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
- **Proximity voice chat**: hold **V** to talk; friends hear you from where
  your character stands, fading with distance (Opus, ~3 KB/s)
- Borderless fullscreen at native resolution

### Known limits (roadmap)

- Spider/worm set-pieces stay single-target on the host (deliberate — their
  scripted sequences assume vanilla senses); they still position-sync
- Enemies cannot *hear* guests yet, only see them
- Notebook/journal entries are per-player by design

## Voice chat

Push-to-talk: hold **V** while in a session. Everyone else hears you as a
3D sound coming from your character's head — full volume within 2 m,
fading with distance (inverse-distance, clamped at 25 m), so two players
across a mine hall have to shout... or walk over. The party panel shows
`(talking)` after the name of whoever is heard and `[MIC]` on your own line
while your microphone is live. Audio is Opus (16 kHz mono, 20 ms frames,
24 kbps VBR, two frames per packet, unreliable — a lost packet is concealed,
never resent) and travels through the host like everything else. The
microphone is opened the first time you press V in a session (the default
OpenAL capture device — pick it in Windows sound settings); nothing is
opened in single-player.

`multiplayer.cfg` keys (all optional):

| Key | Default | Meaning |
|-----|---------|---------|
| `voice_enabled=` | `1` | `0` disables voice chat entirely on this machine (no microphone, no playback). |
| `voice_volume=` | `1.0` | Playback gain for other players' voices, `0`..`2`. |
| `voice_open_mic=` | `0` | `1` = no key needed: the mic streams whenever its level is above -40 dBFS (with a 0.4 s hold). |

Build side: voice needs the vcpkg `opus` package (listed in
`Overture/vcpkg.json`); CMake option `PENUMBRA_VOICE` (default ON) turns
into `PENUMBRA_VOICE=1` when opus is found and quietly compiles the stubs
otherwise. `-DPENUMBRA_VOICE=OFF` builds without it.

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
