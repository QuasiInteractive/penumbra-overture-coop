# Ghost animation clips (Motifect BVH pack -> HPL1 clip DAEs)

The 15 clip files per ghost model in `Overture/multiplayer/models/`
(`<name>_<slot>.dae`, loaded by `cGhostPlayer::LoadAnimations`) are generated
from the Motifect BVH locomotion pack by `bvh_to_hpl_clip.py` in this folder.
Each run also writes `<name>_clips.json`, the per-slot sidecar the runtime
reads for gait speeds (`gait_speed_mps`).

Files in this folder:

| file | role |
|---|---|
| `bvh_to_hpl_clip.py` | the converter (pure Python 3, no numpy); imports `retarget_clip_dae.py` for its DAE parser and matrix helpers |
| `retarget_clip_dae.py` | older in-place retargeter for the 2026-07 clip package; only its `Doc` parser / `rot_zyx` / `euler_zyx` helpers are used now |
| `hpl_dae_export.py` | Blender script that produced the base `<name>.dae` (mesh + skin + JOINT tree); its clip layout is what the converter reproduces byte-for-byte in structure |
| `anim_viewer.html` | offline viewer that renders exactly what `cMeshEntity` computes (drop base + clips + BVH + sidecar on it) |

## Regenerate

The BVH pack is not in the repository; point `--bvh-dir` at its `BVH/`
folder (45 files, 30 fps, 77 joints).

```sh
cd <repo>
python3 Overture/multiplayer/tools/bvh_to_hpl_clip.py \
    --base Overture/multiplayer/models/phillip.dae \
    --bvh-dir <motifect_pack>/BVH \
    --out Overture/multiplayer/models --name phillip
python3 Overture/multiplayer/tools/bvh_to_hpl_clip.py \
    --base Overture/multiplayer/models/malik.dae \
    --bvh-dir <motifect_pack>/BVH \
    --out Overture/multiplayer/models --name malik
```

Each run takes about 6 s, overwrites the 15 `<name>_<slot>.dae` files and
`<name>_clips.json`, and exits non-zero if its self-validation fails. The
output is deterministic (a second run into another directory is byte-identical),
so a regenerate is safe to diff. `--only walk,run` limits the slots (the
other slots' files are not touched and their sidecar entries are kept; the
subset writes the same bytes as a full run), `--dry-run` computes and logs
without writing. After regenerating, delete the
engine's `.collcach` cache files for the clips (the loader prefers a cache
newer than the `.dae`).

## Slot table

BVH frames are 30 fps, `[start, end)`; a looping clip exports frames
`start..end-1` followed by an exact copy of frame `start` (so `N = end-start+1`
keys, loop length `(end-start)/30 s`, seamless under HPL's inclusive wrap).
`stretch` (time-stretch factor) resamples the selection at fractional source
frames, every BVH channel lerped between its two neighbours (rotations made
continuous first), giving `round(N*stretch)` forward keys; `pingpong` then
appends those keys reversed minus both endpoints, so the loop returns to key 0
exactly (seam 0, no net drift, gait 0).
Gait = hips XZ drift removed over the range / duration, scaled to the model
(see "Hips translation"); 0 = not a locomotion clip. The runtime plays a
locomotion clip at `measured_speed / gait`.

| slot | pack clip | frames | keys | loop | phillip gait m/s | malik gait m/s | notes |
|---|---|---|---|---|---|---|---|
| idle | idle_neutral | [125,217) | 93 | yes | 0 | 0 | only pack idle without body yaw drift |
| walk | walk_forward | [79,113) | 35 | yes | 1.28 | 1.45 | one gait cycle (T = 35 f) |
| run | run_jog | [137,159) | 23 | yes | 1.92 | 2.18 | fast half of run_jog; first half is a 1.05 m/s jog |
| walk_back | walk_backward | [99,138) | 40 | yes | 0.72 | 0.81 | |
| strafe_walk_l | walk_strafe_left | [78,179) | 102 | yes | 0.54 | 0.61 | two cycles (speed pulses inside one) |
| strafe_walk_r | walk_strafe_right | [116,153) | 38 | yes | 0.77 | 0.88 | |
| strafe_run_l | run_strafe_left | [117,139) | 23 | yes | 2.10 | 2.39 | frames 0-60 of the source are a standing start |
| strafe_run_r | run_strafe_right | [133,157) | 25 | yes | 1.84 | 2.09 | frames 0-35 are a standing start |
| crouch_idle | crouch_rise_up | [10,14) x11.25, pingpong | 89 | yes | 0 | 0 | hold of the transitions' shared frame 10 (hips 0.50 m) breathing up 6 cm to f13 and back: 45 keys up + 43 down, 2.93 s. Replaces the pack's crouch_idle.bvh (`loop:auto:75:105` -> [192,283), 92 keys: a deep kneel, hips 0.42 m, knees on the floor, head 0.59 m), kept as a commented alternative in `SLOTS` |
| crouch_walk | crouch_walk_forward | [82,136) | 55 | yes | 0.76 | 0.87 | hips 0.55-0.69 m |
| jump | jump_standing | [44,84) | 40 | no | 0 | 0 | airborne 47-59 + landing; `airborne="flat"` |
| stand_to_crouch | crouch_rise_up, reversed | [10,42) | 32 | no | 0 | 0 | frames 0-9 of the source are a heel-sit squat |
| crouch_to_stand | crouch_rise_up | [10,42) | 32 | no | 0 | 0 | same length as stand_to_crouch (runtime lerps the Y offset over it) |
| turn_l | turn_left_90 | [0,40) | 40 | no | 0 | 0 | linear yaw drift (+92.7 deg) removed |
| turn_r | turn_right_90 | [0,60) | 60 | no | 0 | 0 | linear yaw drift (-93.9 deg) removed |

Resulting hips heights (phillip / malik, m): standing clips 0.91-1.03 /
0.93-1.10, crouch_idle 0.50-0.55 / 0.49-0.56, crouch_walk 0.55-0.69 /
0.56-0.72, jump dips to 0.71 / 0.75 on landing. Lowest foot joint on ground clips stays within
about +/-2 cm of the base rest floor (toe base at 0.018 / 0.015 m); the
exceptions are walk_back (-1.8 / -2.6 cm, the actor's foot sinks 4 cm in the
source) and the jump take-off/landing frames (down to -8.6 / -9.1 cm, see
"Jump" below).

The table lives in `SLOTS` at the top of the converter: `bvh`, `frames`
(a tuple or `"loop:auto:MIN:MAX"`), `loop`, `reverse`, `remove_yaw`,
`airborne`, `clamp_y_to_stand`, `stretch`, `pingpong`, `notes`.

## Output format (what the engine needs)

A clip file is the whole base document (images, effects, material, geometry,
skin controller, JOINT tree) with one `<library_animations>` block inserted
before `<scene>`. The base part is byte-identical to `<name>.dae`, so the
clip's skin bind equals the base's by construction: `CreateAnimTrack` stores
`inv(clip_bind_rot) * key_rot` and the runtime renders
`base_bind_rot * stored`, which only reproduces the authored pose when the two
binds match. Per joint, in document order:

```
<animation id="hpl_<joint>">
  sources hpl_<joint>-time  (frame numbers 0..N-1; HPL converts at 30 fps
                             because the file has no MAYA <extra> block)
          hpl_<joint>-trans (N*3 metres), -rotz/-roty/-rotx (N degrees)
  4 samplers, channels <joint>/translate, <joint>/rotateZ.ANGLE, rotateY, rotateX
```

Keys are the complete node-local transform in parent-joint space:
`L = T(trans) * Rz * Ry * Rx` (column vectors, degrees), i.e. exactly what the
loader composes from the node's `<translate>` + `<rotate>` elements. Every
joint gets all four channels every frame, so the scene's rest angles never
enter playback. Non-hips joints carry their constant bind-local translation
(stored delta 0); only `mixamorigwHips` translates.

## Retarget math

Both rigs are Y-up, face +Z, left = +X. The BVH rest pose (all channels 0)
is a strict T-pose with identity world rotations, so the source's "delta from
rest" of joint `s` at time `t` is simply its FK world rotation `W_src(s,t)`.
For a target (Mixamo) joint `j` mapped to source joint `s = MAP[j]`:

```
W_tgt(j,t) = W_src(s,t) * A(j) * W_tgt_rest(j)
```

`W_tgt_rest(j)` is the base's world bind rotation `rot3(inv(IB_j))`, the frame
the engine skins with (it agrees with the node rest angles to 0.001 deg in
these files, but the binds are used). `A(j)` is a constant per-joint
correction: the two rests are both T-poses but not the same one (phillip's
arms droop 10 deg and his feet pitch 19 deg differently; malik is an A-pose,
arms 39 deg down). Without `A(j)` the source T-pose would map onto the
target's own rest and those differences would be baked into every frame.
`A(j)` is the minimal rotation taking the target's rest bone direction
(joint -> `ALIGN_CHILD[j]`, from the bind positions) onto the source's rest
bone direction, so the target bone points exactly where the source bone
points, every frame. It is applied to the torso, neck, shoulders, arms, hands
and legs only. It is deliberately not applied to the feet, toes, head and
fingers: their target directions are defined by leaf markers or by a
differently placed ankle (phillip's ankle sits 14 cm above the floor, the
BVH actor's 7 cm), so aligning them would tilt the head and lift the toes; the
plain delta keeps "source rest = target rest = flat on the floor" for the
feet. Twist about the bone axis is not corrected (both rigs use Y-along-bone,
the residual is the Mixamo rest twist).

The result is re-localised against the parent's retargeted world rotation,
`L(j,t) = inv(W_tgt(parent(j),t)) * W_tgt(j,t)`, decomposed to Z/Y/X Euler
(`euler_zyx`), and made continuous across frames (both equivalent Euler
solutions are tried, the one nearest the previous frame wins, so no channel
jumps by 360). The engine itself is indifferent, it re-composes a quaternion
per key. Target joints without a source (HeadTop_End, Toe_End, Thumb4,
Index4 leaves; none carry skin weights) keep their bind local, i.e. they
follow their parent. BVH joints without a target (Neck2, the index
metacarpals, middle/ring/pinky fingers, eyes, jaw) fold automatically because
the mapped child's world rotation already contains them.

Joint mapping (`MAP`): Hips, Spine1->Spine, Spine2->Spine1, Chest->Spine2,
Neck1->Neck, Head, Left/RightShoulder, Arm, ForeArm, Hand, Thumb1-3 (phillip
only), Index2/3/4 -> Index1/2/3 (the BVH index has an extra metacarpal),
Leg->UpLeg, Shin->Leg, Foot, ToeBase. The mapping is filtered to the joints the
base actually has (malik has no thumbs), so the same table serves both models.

## Hips translation, floor lock, root motion

```
hips.y  = base_bind_hips.y  + (bvh_y  - STAND_HIPS_CM) * k
hips.xz = base_bind_hips.xz + (bvh_xz_dedrifted - bvh_xz_dedrifted[first]) * k
```

`k` (metres per BVH cm, `--scale`, default `auto`) is the leg-length ratio
(base thigh+shin / BVH thigh+shin: phillip 0.00894, malik 0.01018), not the
raw 0.01 or the hips-height ratio: a knee bend that lowers the BVH hips by
51 cm lowers a shorter-legged phillip by what his legs produce when bent by
the same angles, so the feet stay on the floor in crouches. A BVH frame at the
standing height (`STAND_HIPS_CM = 99.8`) lands exactly on the base rest hips.

Floor lock (default on, `--no-floor-lock` for the raw formula): the two bodies
are not proportional (ankle height, foot length), so per frame the target's
lowest contact joint (feet, toe bases, toe tips, knees) is placed where the
source's lowest contact joint is: `lowest_tgt = floor_tgt + clearance_src * k`,
`floor_tgt` = the base rest's lowest contact joint, `clearance_src` = the
source's lowest contact height (floor = y 0 in the pack). The hips Y is shifted
by the difference; the shift stays within about 2 cm on standing clips (it
was -8 cm on the former kneeling crouch_idle, where the knees were the
contact). Per-slot `airborne`
mode: `keep` (default) keeps positive clearance, so the hips rise with the
source's feet; `ground` drops positive clearance, the feet never leave the
floor; `flat` (jump) does not lock frames whose source contact is off the
ground and caps the hips at the standing height.

Jump: the ghost body is placed at the remote camera's Y plus a stance offset
(`GhostPlayer.cpp`, `ghost_body_y` in `multiplayer.cfg`), and the remote
camera already rises during a real jump, so the clip must not add the +33 cm
hips arc a second time. With `airborne="flat"` the clip contributes only the
leg swing/tuck and the take-off/landing dips. Consequence in clip space: on the
3-4 take-off and landing frames where the source's legs are extended but its
feet are already 2-15 cm off the ground, the lowest toe goes up to 8.6 cm
below the floor; in game the entity is rising/falling with the camera at those
instants, which is closer to reality than lifting the hips above standing.

Root motion: the linear hips XZ drift between the first and last exported
source frame is removed on every clip (the network position owns
locomotion; `drift / duration` is the sidecar's gait speed). Y is kept
(crouch depth, walk/run bob). Turn clips additionally get their linear yaw
drift removed (every source joint is rotated about the hips' start position;
the network yaw owns facing), and their XZ drift is measured on the
yaw-corrected path so the hips end exactly where they started.

## Tuning knobs

- `SLOTS` (top of the converter): source clip, frame range, loop, reverse,
  `remove_yaw`, `airborne`, `clamp_y_to_stand`, `stretch`, `pingpong`.
  `frames="loop:auto:MIN:MAX"` searches the best seam (joint positions
  relative to the hips plus hips velocity) for a loop of MIN..MAX frames.
  `stretch=F` slows the selection F times (fractional-frame resampling of the
  BVH channels), `pingpong=True` plays it back to its first frame (an exact
  loop out of any short monotonic motion, e.g. a hold with a slow bob).
- `MAP` / `ALIGN_CHILD`: joint mapping and which bones get the rest-direction
  alignment `A(j)`.
- `CONTACT_TGT` / `CONTACT_SRC` / `GROUND_EPS_CM` (2 cm): floor-lock joint sets
  and the "on the ground" threshold for `airborne="flat"`.
- `--scale auto|<m_per_cm>`, `--stand-hips-cm` (99.8), `--no-floor-lock`,
  `--only`, `--dry-run`.
- Sidecar `gait_speed_mps` is what the runtime uses; `ghost_gait_<clip>` in
  `multiplayer.cfg` overrides it per clip without regenerating.

## Checking results

1. Read the converter log: per slot it prints the key count, hips height
   range, floor-lock shift range, lowest foot joint height, removed drift /
   resulting gait, removed yaw, and the loop seam (source frame `end` vs
   `start` on the target, position rms and max joint rotation). It then
   re-parses every written file and fails on joint order / skin / inverse-bind
   / key-count / time-array mismatches against the base. Sanity expectations:
   feet within a few cm of the floor on ground clips, standing hips near the
   base rest (0.967 phillip, 1.034 malik), seam under ~5 cm rms for gait
   loops, `all clips validated`.
2. `anim_viewer.html` (same folder, works from `file://`): drop `<name>.dae`,
   the `<name>_*.dae` clips, `<name>.tga`, `<name>_clips.json` and the source
   `.bvh` files onto it. It plays clips with the engine's own semantics (bind
   from inverse binds, `bindR * inv(bindR) * Rz*Ry*Rx` keys, frame/30 times,
   inclusive loop wrap, slerp between keys), reports clip-vs-base skeleton
   compatibility, loop seam (first vs last key), and slerp snap statistics,
   and can draw the BVH stick figure next to the retargeted mesh and A/B
   compare two clips. The "ghost camera markers" show where the remote
   player's camera sits above the mesh origin (feet at y = 0): the head of the
   posed mesh should sit at the standing marker, and at the crouched marker in
   crouch clips; set `ghost_body_y` / `ghost_body_y_crouch` from those.
3. In game, the ghost preview mode (`ghost_preview=1` in `multiplayer.cfg`)
   spawns a local preview ghost in front of the player and exercises the real
   clip selector without a second machine; F6/F7 cycle the 15 clips
   (`cGhostPlayer::GetClipName` order, forced through `DebugPlayClip`), F8
   toggles the crouch stance (transitions + Y offset), F2 a treadmill walk to
   check gait scaling and foot sliding. Check feet on the floor in idle / walk
   / crouch_idle, the head at eye height, no popping at loop seams, and that
   walk and run do not slide at the remote's real speeds.

## Known caveats

- crouch_idle is a stretched pingpong hold of crouch_rise_up frames 10-13:
  it starts on the exact pose both stance transitions start/end on (frame 10,
  hips 0.50 m) and breathes 6 cm up over 1.47 s and back. That sits between
  the transition endpoint and crouch_walk (hips 0.55-0.69 m), so the crouch
  idle <-> walk switch is a ~10 cm hips change under the crossfade instead of
  the former 20 cm drop onto the pack's deep kneel (crouch_idle.bvh: hips
  0.42 m, torso pitched ~76 deg, head at 0.59 m; kept as a commented
  alternative in `SLOTS`). The resampling is linear, so the turnaround at
  frame 13 flips the hips velocity (about 4 cm/s): a soft kink, not a pop.
- Gait loop seams (run 5 cm rms / 22 deg, strafe_run_l 7 cm) are inherent in
  the source cycles; the runtime crossfades over them.
- The feet's 19 deg (phillip) / 15 deg (malik) rest-pitch difference from the
  BVH foot is intentional (no `A(j)` on feet), so a source frame with a flat
  foot renders the target's own flat foot.
