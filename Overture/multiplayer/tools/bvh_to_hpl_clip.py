#!/usr/bin/env python3
"""
bvh_to_hpl_clip.py - Motifect BVH locomotion pack -> HPL1 ghost clip DAEs.

Writes, for one base model <name>.dae (a Mixamo rig exported by
hpl_dae_export.py), the 15 clip files <name>_<slot>.dae that
cGhostPlayer::LoadAnimations loads (Overture/multiplayer/GhostPlayer.cpp:385-420),
plus a sidecar <name>_clips.json with per-slot metadata.

Pure Python 3 (no numpy). Reuses the DAE parser / matrix helpers of
retarget_clip_dae.py (same directory).

OUTPUT FORMAT (byte-compatible with the proven hpl_dae_export.py clips)
------------------------------------------------------------------------
Each clip file is the WHOLE base document (images, effects, material,
geometry, skin controller, visual scene = base skeleton rest) with a
<library_animations> block inserted before <scene>. Per joint j, in the
base document's node order:
    <animation id="hpl_j">
      sources hpl_j-time (frame numbers 0..N-1), hpl_j-trans (N*3 metres),
              hpl_j-rotz / -roty / -rotx (N degrees)
      4 samplers + channels  j/translate, j/rotateZ.ANGLE, j/rotateY.ANGLE,
                             j/rotateX.ANGLE
    </animation>
Because the clip embeds the base skin, the clip's skin bind == the base's
by construction, which is what HPL's CreateAnimTrack needs (it stores
inv(clip_bind_rot)*full_rot and the runtime re-applies base_bind_rot*stored;
see retarget_clip_dae.py's docstring and MeshLoaderColladaHelpers.cpp:445-513).
The keys written here are FULL local node transforms (translate in metres,
rotateZ/Y/X degrees composing R = Rz*Ry*Rx in document order, i.e. HPL's
T*Rz*Ry*Rx); the loader applies the storage transform itself - it is NOT
pre-applied here.

Times are frame numbers: HPL converts at a hard-coded 30 fps when the
document has no MAYA <extra> start/end block (MeshLoaderColladaHelpers.cpp:462),
and the Motifect pack is 30 fps, so one BVH frame == one key.

RETARGET MATH (world-rotation delta + rest-direction alignment)
----------------------------------------------------------------
Both rigs are Y-up, face +Z, left = +X (see README_animations.md for the
measurements). The BVH rest pose (all channels 0) is a strict T-pose with
identity world rotations, so the source "delta from rest" of joint s at time
t is simply its FK world rotation W_src(s,t).

For a target (Mixamo) joint j mapped to source joint s = MAP[j]:

    W_tgt(j,t) = W_src(s,t) * A(j) * W_tgt_rest(j)

W_tgt_rest(j) is the base's world BIND rotation rot3(inv(IB_j)) - the frame
the engine actually skins with (not the node's rest rotateZ/Y/X, although
for these files the two agree). A(j) is a constant per-joint correction: the
two rests are both T-poses but not the same T-pose (Mixamo arms droop 10
degrees on phillip and 39 on malik's A-pose, spine leans back 6, ...).
Without A(j) the source T-pose would map onto the target's own rest and
those differences would be baked into every frame (arms 10/39 degrees too
low). A(j) is the minimal rotation taking the target's rest bone direction
(joint -> ALIGN_CHILD[j], from the bind positions) onto the source's rest
bone direction, so that   W_tgt(j,t) * localBoneDir = W_src(s,t) * srcBoneDir
- the target bone points exactly where the source bone points, every frame.
A(j) is only applied to the torso, arms and legs (ALIGN_JOINTS). It is
deliberately NOT applied to the feet, toes, head and fingers: their target
rest directions are defined by leaf markers (HeadTop_End, Toe_End, finger
tips) or by a differently placed ankle joint (phillip's ankle sits 14 cm up,
the BVH actor's 7 cm), so aligning those directions would lift the toes off
the floor / tilt the head, while the plain delta keeps "source rest = target
rest = flat on the floor" for the feet.

The result is re-localised against the parent's retargeted world rotation,
    L(j,t) = inv(W_tgt(parent(j),t)) * W_tgt(j,t)
and decomposed to Z/Y/X Euler in the document convention (euler_zyx from
retarget_clip_dae.py), with per-joint angle continuity across frames (the
two equivalent Euler solutions are both tried, the one nearest the previous
frame wins) so a viewer never sees 359 -> 0 flips. The engine itself is
indifferent (it re-composes a quaternion per key).

Target joints with no source (HeadTop_End, Toe_End, Thumb4, Index4 leaves;
BVH Neck2 and the finger metacarpals are folded automatically because the
world rotation of the mapped child already contains them) keep their bind
local rotation, i.e. they inherit their parent's delta.

HIPS TRANSLATION (the only real translation track)
--------------------------------------------------
    hips_tgt.y  = base_bind_hips.y  + (bvh_y  - STAND_HIPS_CM) * k
    hips_tgt.xz = base_bind_hips.xz + (bvh_xz_dedrifted - bvh_xz_dedrifted[first]) * k
k (metres per cm) = --scale: 'auto' = (base thigh+shin) / (bvh thigh+shin),
the LEG ratio (phillip 0.00878, malik 0.00981), not the raw 0.01 or the
hips-height ratio: a knee bend that lowers the BVH hips by 51 cm lowers a
12%-shorter-legged phillip by 0.45 m, which is what his legs produce when
bent by the same angles - so the feet stay on the floor in crouches.
A standing BVH frame lands exactly on the base rest (stored delta ~0).

Floor lock (default on, --no-floor-lock for the raw formula above): the two
bodies are not proportional (phillip's ankle sits 14 cm above the floor, the
BVH actor's 7 cm; his thigh+shin is 12% shorter), so a formula that only
scales the hips height leaves the feet floating in a kneel (crouch_idle
+7.6 cm) or under the floor at a tip-toe take-off (jump -8.6 cm). Per frame
the target's lowest contact joint (feet, toe bases, toe tips, knees) is
therefore placed where the source's lowest contact joint is:
    lowest_tgt(t) = floor_tgt + clearance_src(t) * k
floor_tgt = the base rest's lowest contact joint (phillip: toe base 0.018 m),
clearance_src = the source's lowest contact joint height (floor = y 0 in the
pack). The hips Y is shifted by the difference; the shift is logged per clip
and stays within ~2 cm on standing clips (-8 cm in the kneeling crouch_idle,
where the knees are the contact). Per-slot `airborne` mode:
  "keep"   (default) clearance kept: the hips rise with the source's feet
  "ground" positive clearance dropped: the feet never leave the entity floor
  "flat"   (jump) frames whose source contact is off the ground are not
           locked and the hips are capped at the standing height: the ghost
           body is placed at the remote camera's Y (GhostPlayer.cpp:809-814),
           which already rises with the real jump, so the clip contributes
           only the leg swing/tuck and the take-off/landing dips - no double
           height, and no 50 cm hips plunge that a ground lock would author
           when the legs tuck mid-air.
Every other joint's translate is the constant bind local translation
(stored delta exactly 0), exactly like the hpl_dae_export.py clips.

Root motion policy (per slot flags; see README_animations.md):
  * linear XZ drift between the first and last exported source frame is
    removed on every clip (the network position owns locomotion); the
    removed drift / duration is the clip's gait speed (JSON gait_speed_mps)
  * Y is kept (crouch depth, walk/run bob), floor-locked as described above;
    airborne="flat" (jump) caps the hips at the standing height while the
    source is off the ground; clamp_y_to_stand caps them always (unused)
  * linear yaw drift is removed on turn clips (the network yaw owns facing):
    every source joint is rotated about the hips' start position
  * loops: the exported keys are source frames [s, e) followed by an exact
    copy of frame s, so N = e-s+1 keys and the loop length is (e-s)/30 s.
    HPL wraps a looping state inclusive of L (cMath::Wrap), showing the last
    key for the instant t == L and then key 0 - an exact duplicate makes that
    seamless (the shipped phillip_walk.dae uses the same convention: 32 keys,
    key 31 == key 0). 'loop:auto' searches the best [s, e) in a window.
  * reverse=True plays the source frames backwards (stand_to_crouch is the
    reverse of crouch_rise_up).
  * stretch=F (default 1) time-stretches the selection: the output has
    round(N*F) keys (N = e-s selected frames) sampled at fractional source
    frames, every BVH channel lerped between its two neighbouring frames
    (rotation channels made continuous first, translations plain). A loop
    period stays e-s source frames, a non-loop / pingpong selection spans
    s..e-1 inclusive, so key 0 and the last forward key are real frames.
  * pingpong=True appends the reversed forward sequence minus both
    endpoints (2M-2 keys), so the loop closes exactly with zero seam and
    zero net drift; the gait is 0. crouch_idle is a stretched pingpong
    hold of crouch_rise_up frames 10-13 (see SLOTS).

Usage:
  python3 bvh_to_hpl_clip.py --base <models>/<name>.dae --bvh-dir <pack>/BVH \
      --out <models> --name <name> [--scale auto|<m_per_cm>] [--only slot,slot] \
      [--stand-hips-cm 99.8] [--dry-run]
"""
import argparse
import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from retarget_clip_dae import (Doc, mat_mul, mat_t, rot_axis, rot_zyx, euler_zyx,  # noqa: E402
                               normalize_rot, fmt)

FPS = 30.0
PFX = "mixamorigw"
HIPS = PFX + "Hips"
# BVH standing hips height (idle_neutral / jump / turn / crouch_rise_up end all
# stand at 99.5-100.0 cm; see README). A BVH frame at this height renders the
# base rest hips height.
STAND_HIPS_CM = 99.8
# a source contact joint at or below this height counts as "on the ground"
GROUND_EPS_CM = 2.0

# ------------------------------------------------------------------ slot table
# slot -> dict(bvh, frames, loop, reverse, remove_yaw, airborne, clamp_y_to_stand,
#              stretch, pingpong, notes)
# frames = [start, end)  (BVH frame numbers, 30 fps), or "loop:auto:MIN:MAX"
# (search the best seam for a loop of MIN..MAX frames). stretch = time-stretch
# factor (fractional-frame resampling), pingpong = play back to the start.
SLOTS = [
    ("idle", dict(bvh="idle_neutral", frames=(125, 217), loop=True,
                  notes="only pack idle without body yaw drift; seam 0.13 cm")),
    ("walk", dict(bvh="walk_forward", frames=(79, 113), loop=True,
                  notes="one gait cycle (T=35); use (37,106) for two cycles")),
    ("run", dict(bvh="run_jog", frames=(137, 159), loop=True,
                 notes="fast half of run_jog (~2.15 m/s BVH); first half is a 1.05 m/s jog")),
    ("walk_back", dict(bvh="walk_backward", frames=(99, 138), loop=True)),
    ("strafe_walk_l", dict(bvh="walk_strafe_left", frames=(78, 179), loop=True,
                           notes="two cycles: the speed pulses inside one cycle")),
    ("strafe_walk_r", dict(bvh="walk_strafe_right", frames=(116, 153), loop=True)),
    ("strafe_run_l", dict(bvh="run_strafe_left", frames=(117, 139), loop=True,
                          notes="frames 0-60 of the source are a standing start")),
    ("strafe_run_r", dict(bvh="run_strafe_right", frames=(133, 157), loop=True,
                          notes="frames 0-35 of the source are a standing start")),
    # crouch_idle: the pack's crouch_idle.bvh is a deep kneel (hips 0.42 m on
    # phillip, knees on the floor) while crouch_walk stands at 0.55-0.69 m and
    # both stance transitions start/end on crouch_rise_up frame 10 (hips
    # ~0.50 m), so a crouching player who stopped would drop to the knees.
    # Instead hold crouch_rise_up frames 10-13 (a slow 6.5 cm BVH rise from
    # the transitions' shared frame), time-stretched 11.25x and played back
    # and forth: 45 keys up + 43 down + loop duplicate = 89 keys, 2.93 s.
    ("crouch_idle", dict(bvh="crouch_rise_up", frames=(10, 14), loop=True, stretch=11.25,
                         pingpong=True,
                         notes="stretched pingpong hold of crouch_rise_up f10-13: key 0 == "
                               "stand_to_crouch's last / crouch_to_stand's first key; "
                               "alternative: crouch_idle.bvh loop:auto:75:105 (deep kneel, hips 0.42 m)")),
    # ("crouch_idle", dict(bvh="crouch_idle", frames="loop:auto:75:105", loop=True,
    #                      notes="deep kneel (hips 0.42 m); pops against crouch_walk and the transitions")),
    ("crouch_walk", dict(bvh="crouch_walk_forward", frames=(82, 136), loop=True)),
    ("jump", dict(bvh="jump_standing", frames=(44, 84), loop=False, airborne="flat",
                  notes="airborne 47-59 + landing; the runtime starts the clip on the "
                        "airborne edge so the anticipation frames 26-46 are skipped")),
    ("stand_to_crouch", dict(bvh="crouch_rise_up", frames=(10, 42), loop=False, reverse=True,
                             notes="reverse of crouch_to_stand; frames 0-9 are a heel-sit squat")),
    ("crouch_to_stand", dict(bvh="crouch_rise_up", frames=(10, 42), loop=False)),
    ("turn_l", dict(bvh="turn_left_90", frames=(0, 40), loop=False, remove_yaw=True,
                    notes="yaw ramps 0->93 deg over 0-40 then holds")),
    ("turn_r", dict(bvh="turn_right_90", frames=(0, 60), loop=False, remove_yaw=True,
                    notes="slower ramp than turn_l (-95 deg by f60)")),
]

# ------------------------------------------------------------ joint mapping
# target (Mixamo, ':' -> 'w') -> source (Motifect BVH)
MAP = {
    "Hips": "Hips", "Spine": "Spine1", "Spine1": "Spine2", "Spine2": "Chest",
    "Neck": "Neck1", "Head": "Head",
    "LeftShoulder": "LeftShoulder", "LeftArm": "LeftArm",
    "LeftForeArm": "LeftForeArm", "LeftHand": "LeftHand",
    "LeftHandThumb1": "LeftHandThumb1", "LeftHandThumb2": "LeftHandThumb2",
    "LeftHandThumb3": "LeftHandThumb3",
    # BVH index has an extra metacarpal joint (Index1); Mixamo Index1 is the knuckle
    "LeftHandIndex1": "LeftHandIndex2", "LeftHandIndex2": "LeftHandIndex3",
    "LeftHandIndex3": "LeftHandIndex4",
    "LeftUpLeg": "LeftLeg", "LeftLeg": "LeftShin", "LeftFoot": "LeftFoot",
    "LeftToeBase": "LeftToeBase",
}
for _k in list(MAP):
    if _k.startswith("Left"):
        MAP["Right" + _k[4:]] = "Right" + MAP[_k][4:]
MAP = {PFX + k: v for k, v in MAP.items()}

# Joints whose rest bone direction is aligned onto the source's (A(j) != I).
# Value: the target child that defines the bone direction. The source child
# is the source joint mapped from that target child (Neck -> Head spans the
# BVH Neck2; LeftHand -> LeftHandIndex1 maps to BVH LeftHandIndex2).
ALIGN_CHILD = {
    "Hips": "Spine", "Spine": "Spine1", "Spine1": "Spine2", "Spine2": "Neck",
    "Neck": "Head",
    "LeftShoulder": "LeftArm", "LeftArm": "LeftForeArm", "LeftForeArm": "LeftHand",
    "LeftHand": "LeftHandIndex1",
    "LeftUpLeg": "LeftLeg", "LeftLeg": "LeftFoot",
}
for _k in list(ALIGN_CHILD):
    if _k.startswith("Left"):
        ALIGN_CHILD["Right" + _k[4:]] = "Right" + ALIGN_CHILD[_k][4:]
ALIGN_CHILD = {PFX + k: PFX + v for k, v in ALIGN_CHILD.items()}

# ground-contact joints for the floor lock and the self-checks
CONTACT_TGT = [PFX + n for n in ("LeftFoot", "RightFoot", "LeftToeBase", "RightToeBase",
                                 "LeftToe_End", "RightToe_End", "LeftLeg", "RightLeg")]
CONTACT_SRC = ["LeftFoot", "RightFoot", "LeftToeBase", "RightToeBase",
               "LeftToeEnd", "RightToeEnd", "LeftShin", "RightShin"]
CHECK_FEET = ["LeftFoot", "RightFoot", "LeftToeBase", "RightToeBase",
              "LeftToe_End", "RightToe_End"]

# ----------------------------------------------------------------- vec math
I3 = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]


def mv(m, v):
    return [m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] for i in range(3)]


def vadd(a, b):
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]


def vsub(a, b):
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]


def vscale(a, s):
    return [a[0] * s, a[1] * s, a[2] * s]


def vlen(a):
    return math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])


def vnorm(a):
    l = vlen(a)
    if l < 1e-12:
        raise ValueError("zero-length vector")
    return [a[0] / l, a[1] / l, a[2] / l]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def rot_from_to(a, b):
    """Minimal rotation matrix taking unit vector a onto unit vector b."""
    ax = cross(a, b)
    s = vlen(ax)
    c = max(-1.0, min(1.0, a[0] * b[0] + a[1] * b[1] + a[2] * b[2]))
    if s < 1e-9:
        if c > 0:
            return [row[:] for row in I3]
        # antiparallel: rotate 180 deg about any axis perpendicular to a
        p = [1.0, 0.0, 0.0] if abs(a[0]) < 0.9 else [0.0, 1.0, 0.0]
        ax = vnorm(cross(a, p))
        s, c = 0.0, -1.0
    else:
        ax = [v / s for v in ax]
    x, y, z = ax
    C = 1.0 - c
    return [[c + x * x * C, x * y * C - z * s, x * z * C + y * s],
            [y * x * C + z * s, c + y * y * C, y * z * C - x * s],
            [z * x * C - y * s, z * y * C + x * s, c + z * z * C]]


def rot_angle_deg(m):
    tr = max(-1.0, min(3.0, m[0][0] + m[1][1] + m[2][2]))
    return math.degrees(math.acos(max(-1.0, min(1.0, (tr - 1.0) / 2.0))))


def yaw_of(m):
    """Heading of a joint about +Y: angle of its local +Z axis in the XZ plane."""
    return math.degrees(math.atan2(m[0][2], m[2][2]))


def closest_angle(a, ref):
    while a - ref > 180.0:
        a -= 360.0
    while a - ref < -180.0:
        a += 360.0
    return a


def euler_zyx_continuous(m, prev):
    """euler_zyx(m) with continuity: both equivalent (z,y,x) solutions are
    tried, wrapped to within 180 deg of prev, the nearest wins."""
    z, y, x = euler_zyx(m)
    if prev is None:
        return z, y, x
    cands = [(z, y, x), (z + 180.0, 180.0 - y, x + 180.0)]
    best, best_d = None, None
    for cz, cy, cx in cands:
        cz, cy, cx = (closest_angle(cz, prev[0]), closest_angle(cy, prev[1]),
                      closest_angle(cx, prev[2]))
        d = abs(cz - prev[0]) + abs(cy - prev[1]) + abs(cx - prev[2])
        if best is None or d < best_d:
            best, best_d = (cz, cy, cx), d
    return best


# ------------------------------------------------------------------ BVH
class BVH(object):
    """Minimal BVH reader: hierarchy, channels, frames, FK."""

    def __init__(self, path):
        self.path = path
        self.joints, self.parent, self.offset, self.channels, self.endsite = [], {}, {}, {}, {}
        txt = open(path, encoding="utf-8", errors="replace").read()
        head, motion = txt.split("MOTION", 1)
        toks = head.replace("{", " { ").replace("}", " } ").split()
        stack, cur, i = [], None, 0
        while i < len(toks):
            t = toks[i]
            if t in ("ROOT", "JOINT"):
                name = toks[i + 1]
                self.joints.append(name)
                self.parent[name] = stack[-1] if stack else None
                cur = name
                i += 2
            elif t == "End":
                cur = stack[-1] + "_End"
                self.parent[cur] = stack[-1]
                self.endsite[cur] = True
                i += 2
            elif t == "OFFSET":
                self.offset[cur] = [float(toks[i + 1]), float(toks[i + 2]), float(toks[i + 3])]
                i += 4
            elif t == "CHANNELS":
                n = int(toks[i + 1])
                self.channels[cur] = toks[i + 2:i + 2 + n]
                i += 2 + n
            elif t == "{":
                stack.append(cur)
                i += 1
            elif t == "}":
                stack.pop()
                cur = stack[-1] if stack else None
                i += 1
            else:
                i += 1
        lines = [l for l in motion.strip().split("\n") if l.strip()]
        self.nframes = int(lines[0].split()[1])
        self.dt = float(lines[1].split()[2])
        self.frames = [[float(x) for x in l.split()] for l in lines[2:2 + self.nframes]]
        if len(self.frames) != self.nframes:
            raise ValueError("%s: Frames: %d but %d motion lines" % (path, self.nframes, len(self.frames)))
        self.chidx, k = {}, 0
        for j in self.joints:
            self.chidx[j] = k
            k += len(self.channels[j])
        for f in self.frames:
            if len(f) != k:
                raise ValueError("%s: frame has %d values, expected %d" % (path, len(f), k))
        # per motion column: is it a rotation channel (for row() interpolation)
        self.rot_cols = [name.endswith("rotation") for j in self.joints for name in self.channels[j]]

    def row(self, t):
        """Motion row at source frame t. An integral t is the stored frame; a
        fractional t (stretch resampling) lerps every channel between the two
        neighbouring frames, rotation channels the shorter way round."""
        if float(t).is_integer():
            return self.frames[int(t)]
        i0 = int(math.floor(t))
        a = t - i0
        r0, r1 = self.frames[i0], self.frames[i0 + 1]
        return [v0 + a * ((closest_angle(v1, v0) if rot else v1) - v0)
                for v0, v1, rot in zip(r0, r1, self.rot_cols)]

    def local(self, j, frame):
        ch, base = self.channels[j], self.chidx[j]
        t, R = list(self.offset[j]), I3
        for name, v in zip(ch, frame[base:base + len(ch)]):
            if name == "Xposition":
                t[0] += v
            elif name == "Yposition":
                t[1] += v
            elif name == "Zposition":
                t[2] += v
            elif name == "Zrotation":
                R = mat_mul(R, rot_axis("z", v))  # channel order == composition order
            elif name == "Yrotation":
                R = mat_mul(R, rot_axis("y", v))
            elif name == "Xrotation":
                R = mat_mul(R, rot_axis("x", v))
        return R, t

    def fk(self, fi=None):
        """World rotation and position (cm) per joint; fi None = rest pose,
        fractional fi = interpolated frame (see row())."""
        W, P = {}, {}
        row = None if fi is None else self.row(fi)
        for j in self.joints:
            if fi is None:
                R, t = I3, list(self.offset[j])
            else:
                R, t = self.local(j, row)
            p = self.parent[j]
            if p is None:
                W[j], P[j] = R, t
            else:
                W[j] = mat_mul(W[p], R)
                P[j] = vadd(P[p], mv(W[p], t))
        for e in self.endsite:
            p = self.parent[e]
            P[e] = vadd(P[p], mv(W[p], self.offset[e]))
            W[e] = W[p]
        return W, P


# --------------------------------------------------------------- target rig
class Target(object):
    """Base skeleton as the engine sees it: bind locals from the skin inverse
    binds (retarget_clip_dae.Doc), world bind rotations/positions, order."""

    def __init__(self, base_path):
        self.doc = Doc(base_path)
        self.joints = list(self.doc.parent)          # document (= skin) order
        self.parent = self.doc.parent
        self.local_rot = self.doc.local_bind_rot
        self.local_trans = self.doc.local_bind_trans
        # world rest via FK of the bind locals (== rot3(inv(IB)) for skinned joints)
        self.world_rot, self.world_pos = {}, {}
        for j in self.joints:
            p = self.parent[j]
            if p is None:
                self.world_rot[j] = self.local_rot[j]
                self.world_pos[j] = list(self.local_trans[j])
            else:
                self.world_rot[j] = normalize_rot(mat_mul(self.world_rot[p], self.local_rot[j]))
                self.world_pos[j] = vadd(self.world_pos[p], mv(self.world_rot[p], self.local_trans[j]))
        # cross-check FK world against the raw skin bind (both must agree)
        worst = 0.0
        for j in self.joints:
            if j in self.doc.world_bind:
                g = self.doc.world_bind[j]
                worst = max(worst, vlen(vsub([g[0][3], g[1][3], g[2][3]], self.world_pos[j])))
        if worst > 1e-3:
            raise SystemExit("base %s: FK of bind locals disagrees with skin world bind by %.4f m"
                             % (base_path, worst))
        if HIPS not in self.joints:
            raise SystemExit("base %s has no %s joint" % (base_path, HIPS))
        if self.parent[HIPS] is not None:
            raise SystemExit("base %s: %s is not a root joint" % (base_path, HIPS))

    def floor(self):
        """Lowest contact joint of the rest pose (== the mesh floor, feet at y 0)."""
        return min(self.world_pos[j][1] for j in CONTACT_TGT if j in self.world_pos)

    def fk(self, rots, trans):
        """FK of full local (rot matrix, translation) per joint -> world pos/rot."""
        W, P = {}, {}
        for j in self.joints:
            p = self.parent[j]
            if p is None:
                W[j], P[j] = rots[j], list(trans[j])
            else:
                W[j] = mat_mul(W[p], rots[j])
                P[j] = vadd(P[p], mv(W[p], trans[j]))
        return W, P

    def leg_length(self):
        vals = []
        for side in ("Left", "Right"):
            a, b = PFX + side + "Leg", PFX + side + "Foot"
            if a in self.joints and b in self.joints:
                vals.append(vlen(self.local_trans[a]) + vlen(self.local_trans[b]))
        if not vals:
            raise SystemExit("base has no Left/RightLeg + Foot joints for --scale auto")
        return sum(vals) / len(vals)


def bvh_leg_length(bvh):
    vals = []
    for side in ("Left", "Right"):
        a, b = side + "Shin", side + "Foot"
        if a in bvh.offset and b in bvh.offset:
            vals.append(vlen(bvh.offset[a]) + vlen(bvh.offset[b]))
    return sum(vals) / len(vals)


# ------------------------------------------------------------- retargeting
class Retargeter(object):
    def __init__(self, tgt, bvh_rest, log):
        self.tgt = tgt
        self.log = log
        _, self.src_rest_pos = bvh_rest.fk(None)
        self.src_joints = set(bvh_rest.joints)
        # active mapping: target joint present in base AND source joint present
        self.map = {tj: sj for tj, sj in MAP.items() if tj in tgt.joints and sj in self.src_joints}
        missing_tgt = sorted(k for k in MAP if k not in tgt.joints)
        missing_src = sorted(v for k, v in MAP.items() if k in tgt.joints and v not in self.src_joints)
        if missing_src:
            raise SystemExit("BVH lacks source joints needed by the mapping: %s" % missing_src)
        unmapped = [j for j in tgt.joints if j not in self.map]
        log("  mapping: %d/%d base joints driven by the BVH; keeping bind local for: %s"
            % (len(self.map), len(tgt.joints), " ".join(j[len(PFX):] for j in unmapped)))
        if missing_tgt:
            log("  (mapping entries absent from this base, ignored: %s)"
                % " ".join(j[len(PFX):] for j in missing_tgt))
        # A(j): rest-direction alignment
        self.A = {}
        rows = []
        for tj, tchild in ALIGN_CHILD.items():
            if tj not in self.map or tchild not in tgt.joints or tchild not in MAP:
                continue
            schild = MAP[tchild]
            sj = self.map[tj]
            if schild not in self.src_rest_pos:
                continue
            d_t = vnorm(vsub(tgt.world_pos[tchild], tgt.world_pos[tj]))
            d_s = vnorm(vsub(self.src_rest_pos[schild], self.src_rest_pos[sj]))
            A = rot_from_to(d_t, d_s)
            self.A[tj] = A
            rows.append((tj[len(PFX):], rot_angle_deg(A)))
        log("  rest alignment A(j) [deg]: " + ", ".join("%s %.1f" % r for r in rows))

    def frame(self, W_src, hips_tgt, prev_euler):
        """One frame. W_src: source world rotations (already yaw-corrected).
        Returns (rots{j: 3x3 local}, trans{j}, euler{j: (z,y,x)})."""
        tgt = self.tgt
        W_t, rots, trans, euler = {}, {}, {}, {}
        for j in tgt.joints:
            p = tgt.parent[j]
            if j in self.map:
                w = mat_mul(W_src[self.map[j]], tgt.world_rot[j] if j not in self.A
                            else mat_mul(self.A[j], tgt.world_rot[j]))
            else:
                w = mat_mul(W_t[p], tgt.local_rot[j]) if p is not None else tgt.local_rot[j]
            w = normalize_rot(w)
            W_t[j] = w
            loc = w if p is None else normalize_rot(mat_mul(mat_t(W_t[p]), w))
            rots[j] = loc
            trans[j] = hips_tgt if j == HIPS else list(tgt.local_trans[j])
            euler[j] = euler_zyx_continuous(loc, prev_euler.get(j) if prev_euler else None)
        return rots, trans, euler


# ------------------------------------------------------------------ writer
def write_clip(base_txt, out_path, order, count, data):
    """data: joint -> (trans flat list, rz, ry, rx). Inserts library_animations
    before <scene> in the base document text; layout == hpl_dae_export.py."""
    times = " ".join(str(i) for i in range(count))
    L = ['  <library_animations>']
    for j in order:
        tr, rz, ry, rx = data[j]
        L.append('    <animation id="hpl_%s">' % j)
        L.append('      <source id="hpl_%s-time"><float_array id="hpl_%s-time-array" count="%d">%s</float_array>'
                 '<technique_common><accessor source="#hpl_%s-time-array" count="%d" stride="1" />'
                 '</technique_common></source>' % (j, j, count, times, j, count))
        L.append('      <source id="hpl_%s-trans"><float_array id="hpl_%s-trans-array" count="%d">%s</float_array>'
                 '<technique_common><accessor source="#hpl_%s-trans-array" count="%d" stride="3" />'
                 '</technique_common></source>'
                 % (j, j, count * 3, " ".join(fmt(v) for v in tr), j, count))
        for ax, arr in (("rotz", rz), ("roty", ry), ("rotx", rx)):
            L.append('      <source id="hpl_%s-%s"><float_array id="hpl_%s-%s-array" count="%d">%s</float_array>'
                     '<technique_common><accessor source="#hpl_%s-%s-array" count="%d" stride="1" />'
                     '</technique_common></source>'
                     % (j, ax, j, ax, count, " ".join(fmt(v) for v in arr), j, ax, count))
        L.append('      <sampler id="hpl_%s-trans-sampler"><input semantic="INPUT" source="#hpl_%s-time" />'
                 '<input semantic="OUTPUT" source="#hpl_%s-trans" /></sampler>'
                 '<channel source="#hpl_%s-trans-sampler" target="%s/translate" />' % (j, j, j, j, j))
        for ax, tgt in (("rotz", "rotateZ"), ("roty", "rotateY"), ("rotx", "rotateX")):
            L.append('      <sampler id="hpl_%s-%s-sampler"><input semantic="INPUT" source="#hpl_%s-time" />'
                     '<input semantic="OUTPUT" source="#hpl_%s-%s" /></sampler>'
                     '<channel source="#hpl_%s-%s-sampler" target="%s/%s.ANGLE" />'
                     % (j, ax, j, j, ax, j, ax, j, tgt))
        L.append('    </animation>')
    L.append('  </library_animations>')
    m = re.search(r'\n[ \t]*<scene>', base_txt)
    if not m:
        raise SystemExit("base document has no <scene> element")
    txt = base_txt[:m.start()] + "\n" + "\n".join(L) + base_txt[m.start():]
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        f.write(txt)


def strip_animations(txt):
    """Base document text without any library_animations (in case a clip is
    passed as --base by mistake)."""
    return re.sub(r'\n[ \t]*<library_animations>.*?</library_animations>', '', txt, flags=re.S)


# ---------------------------------------------------------------- loop search
def auto_loop(bvh, lo, hi, log):
    """Best [s, e) with lo <= e-s <= hi: minimal seam between frames e and s
    (joint positions relative to the hips, plus hips velocity)."""
    rel = []
    for fi in range(bvh.nframes):
        W, P = bvh.fk(fi)
        h = P["Hips"]
        rel.append(([vsub(P[j], h) for j in bvh.joints], h))
    best = None
    for s in range(0, bvh.nframes - lo):
        for e in range(s + lo, min(s + hi, bvh.nframes - 1) + 1):
            ps, pe = rel[s][0], rel[e][0]
            d = math.sqrt(sum(vlen(vsub(a, b)) ** 2 for a, b in zip(ps, pe)) / len(ps))
            vs = vsub(rel[s + 1][1], rel[s][1])
            ve = vsub(rel[e][1], rel[e - 1][1])
            score = d + 0.05 * vlen(vsub(vs, ve)) * FPS
            if best is None or score < best[0]:
                best = (score, s, e, d)
    log("  loop:auto -> [%d,%d) seam %.2f cm (score %.2f)" % (best[1], best[2], best[3], best[0]))
    return best[1], best[2]


# ------------------------------------------------------------------ per slot
def build_slot(slot, cfg, tgt, rt, bvh_dir, k, stand_cm, floor_lock, log):
    bvh = BVH(os.path.join(bvh_dir, cfg["bvh"] + ".bvh"))
    if abs(bvh.dt * FPS - 1.0) > 0.01:
        raise SystemExit("%s: frame time %.5f is not 30 fps; HPL assumes 30 fps key numbering"
                         % (bvh.path, bvh.dt))
    loop = cfg.get("loop", False)
    frames = cfg["frames"]
    if isinstance(frames, str) and frames.startswith("loop:auto"):
        parts = frames.split(":")
        lo, hi = int(parts[2]), int(parts[3])
        s, e = auto_loop(bvh, lo, hi, log)
    else:
        s, e = frames
    stretch = float(cfg.get("stretch", 1.0))
    pingpong = bool(cfg.get("pingpong", False))
    # last SOURCE frame that takes part: a plain loop needs frame e for its
    # seam, a pingpong loop closes on itself
    last = e if (loop and not pingpong) else e - 1
    if s < 0 or last >= bvh.nframes or e <= s:
        raise SystemExit("%s: frame range [%d,%d) outside 0..%d" % (slot, s, e, bvh.nframes - 1))
    if stretch <= 0.0:
        raise SystemExit("%s: stretch must be > 0" % slot)

    # ---- exported source frame positions (forward pass) ----
    # stretch != 1 resamples at fractional source frames (BVH.row lerps the
    # channels): a loop period is e-s frames (frame e == frame s), a non-loop
    # or pingpong selection spans s..e-1 inclusive.
    n_src = e - s
    if stretch == 1.0:
        fwd = list(range(s, e))
    else:
        m = int(round(n_src * stretch))
        if m < 2:
            raise SystemExit("%s: stretch %g leaves %d key(s)" % (slot, stretch, m))
        if loop and not pingpong:
            fwd = [s + n_src * i / float(m) for i in range(m)]
        else:
            fwd = [s + (n_src - 1) * i / float(m - 1) for i in range(m)]
    positions = fwd + ([e] if loop and not pingpong else [])   # ascending, s..last

    # ---- source FK over the positions ----
    src = {}
    for fi in positions:
        src[fi] = bvh.fk(fi)
    hips0 = list(src[positions[0]][1]["Hips"])
    hipsL = list(src[positions[-1]][1]["Hips"])
    span = float(last - s) if last > s else 1.0

    # linear yaw drift (turn clips only)
    yaw_total = 0.0
    if cfg.get("remove_yaw"):
        yaw_prev, acc = yaw_of(src[positions[0]][0]["Hips"]), 0.0
        for fi in positions[1:]:
            y = yaw_of(src[fi][0]["Hips"])
            acc += closest_angle(y - yaw_prev, 0.0)
            yaw_prev = y
        yaw_total = acc

    def unyawed(fi):
        """Source world rots/positions of frame fi with the linear yaw drift
        removed: the whole pose is rotated about the hips' START position."""
        W, P = src[fi]
        if abs(yaw_total) > 1e-6:
            R = rot_axis("y", -yaw_total * (fi - s) / span)
            W = {j: mat_mul(R, w) for j, w in W.items()}
            P = {j: vadd(hips0, mv(R, vsub(p, hips0))) for j, p in P.items()}
        return W, P

    # linear XZ drift (network position owns locomotion), measured on the
    # yaw-corrected path so that the last frame's hips land exactly on the
    # first frame's (measuring it on the raw path and rotating afterwards
    # left the turn clips' hips ~18 cm off at the end).
    hipsL_c = unyawed(positions[-1])[1]["Hips"]
    drift = [hipsL_c[0] - hips0[0], 0.0, hipsL_c[2] - hips0[2]]
    drift_len = vlen(drift)

    def corrected(fi):
        """Source world rots/positions of frame fi with yaw AND drift removed."""
        W, P = unyawed(fi)
        if drift_len > 1e-6:
            d = vscale(drift, (fi - s) / span)
            P = {j: vsub(p, d) for j, p in P.items()}
        return W, P

    # ---- exported key sequence ----
    seq = fwd[::-1] if cfg.get("reverse") else list(fwd)
    if pingpong:
        seq = seq + seq[-2:0:-1]      # back to the start, both endpoints excluded
    if loop:
        seq = seq + [seq[0]]          # exact duplicate of the first key (see docstring)
    count = len(seq)

    hips_rest = tgt.local_trans[HIPS]
    clamp = cfg.get("clamp_y_to_stand", False)
    airborne = cfg.get("airborne", "keep")
    tgt_floor = tgt.floor()

    def pose(fi, prev_euler):
        """Retargeted pose of source frame fi -> (rots, trans, euler, Wt, Pt, info)."""
        W, P = corrected(fi)
        hb = P["Hips"]
        hy = hips_rest[1] + (hb[1] - stand_cm) * k
        hips_t = [hips_rest[0] + (hb[0] - hips0[0]) * k, hy,
                  hips_rest[2] + (hb[2] - hips0[2]) * k]
        rots, trans, euler = rt.frame(W, hips_t, prev_euler)
        Wt, Pt = tgt.fk(rots, trans)
        src_low = min(P[j][1] for j in CONTACT_SRC if j in P)
        shift = 0.0
        on_ground = src_low <= GROUND_EPS_CM
        if floor_lock and (airborne != "flat" or on_ground):
            clearance = src_low if airborne == "keep" else min(src_low, 0.0)
            tgt_low = min(Pt[j][1] for j in CONTACT_TGT if j in Pt)
            shift = (tgt_floor + clearance * k) - tgt_low
            hips_t[1] += shift
        if (clamp or airborne == "flat") and hips_t[1] > hips_rest[1]:
            shift += hips_rest[1] - hips_t[1]
            hips_t[1] = hips_rest[1]
        if shift != 0.0:
            trans[HIPS] = hips_t
            Wt, Pt = tgt.fk(rots, trans)
        return rots, trans, euler, Wt, Pt, dict(src_low=src_low, shift=shift)

    data = {j: ([], [], [], []) for j in tgt.joints}
    prev = None
    worlds = []                       # (Wt, Pt) per key for the checks
    hips_ys, shifts = [], []
    src_foot_min, foot_min, foot_min_joint = float("inf"), float("inf"), ""
    for fi in seq:
        rots, trans, euler, Wt, Pt, info = pose(fi, prev)
        prev = euler
        for j in tgt.joints:
            tr, rz, ry, rx = data[j]
            tr.extend(trans[j])
            z, y, x = euler[j]
            rz.append(z)
            ry.append(y)
            rx.append(x)
        worlds.append((Wt, Pt))
        hips_ys.append(Pt[HIPS][1])
        shifts.append(info["shift"])
        src_foot_min = min(src_foot_min, info["src_low"])
        for cj in CHECK_FEET:
            j = PFX + cj
            if j in Pt and Pt[j][1] < foot_min:
                foot_min, foot_min_joint = Pt[j][1], cj

    # NaN / range guard
    for j, arrs in data.items():
        for arr in arrs:
            for v in arr:
                if v != v or abs(v) > 1e6:
                    raise SystemExit("%s: NaN/huge value in joint %s" % (slot, j))

    seam = None
    if loop:
        # true seam: the source frame that would follow the last exported key
        # (frame e, dedrifted; on a pingpong loop key 0's own frame, so 0)
        # vs key 0, on the target
        _, _, _, We, Pe, _ = pose(seq[0] if pingpong else e, None)
        Ws, Ps = worlds[0]
        n = len(tgt.joints)
        pos_rms = math.sqrt(sum(vlen(vsub(Pe[j], Ps[j])) ** 2 for j in tgt.joints) / n)
        rot_max = max(rot_angle_deg(mat_mul(mat_t(Ws[j]), We[j])) for j in tgt.joints)
        seam = dict(pos_rms_m=pos_rms, hips_pos_m=vlen(vsub(Pe[HIPS], Ps[HIPS])), rot_max_deg=rot_max)

    length_s = (count - 1) / FPS
    # a pingpong loop returns to its start: no net displacement, not a gait
    gait_bvh = drift_len / (span / FPS) if (loop and not pingpong) else 0.0
    gait_mps = gait_bvh * k
    meta = dict(
        source=cfg["bvh"] + ".bvh", bvh_frames=[s, e], reverse=bool(cfg.get("reverse", False)),
        stretch=stretch, pingpong=pingpong,
        frame_count=count, fps=FPS, length_s=round(length_s, 5), loop=loop,
        gait_speed_bvh_cms=round(gait_bvh, 2), gait_speed_mps=round(gait_mps, 4),
        hips_height_m=dict(min=round(min(hips_ys), 4), max=round(max(hips_ys), 4),
                           mean=round(sum(hips_ys) / len(hips_ys), 4), first=round(hips_ys[0], 4),
                           last=round(hips_ys[-1], 4)),
        hips_height_bvh_cm=dict(first=round(hips0[1], 2), last=round(hipsL[1], 2)),
        root_motion=dict(xz_drift_removed_cm=[round(drift[0], 2), round(drift[2], 2)],
                         yaw_removed_deg=round(yaw_total, 2), y_clamped_to_stand=clamp,
                         airborne=airborne),
        floor_lock=dict(enabled=floor_lock, target_floor_m=round(tgt_floor, 4),
                        hips_shift_m=dict(min=round(min(shifts), 4), max=round(max(shifts), 4))),
        foot_min_y_m=round(foot_min, 4), foot_min_joint=foot_min_joint,
        bvh_foot_min_y_cm=round(src_foot_min, 2),
        seam=None if seam is None else {kk: round(v, 4) for kk, v in seam.items()},
        notes=cfg.get("notes", ""),
    )
    log("  %-16s %-22s [%3d,%3d)%s N=%3d L=%.3fs hips %.3f..%.3f m (lock %+.1f..%+.1f cm)  "
        "foot min %+.3f m (%s; bvh %+.1f cm)  drift %.1f cm -> gait %.2f m/s  yaw %.1f deg%s"
        % (slot, cfg["bvh"], s, e,
           (" rev" if cfg.get("reverse") else "") + (" x%g" % stretch if stretch != 1.0 else "")
           + (" pingpong" if pingpong else ""), count, length_s,
           min(hips_ys), max(hips_ys), min(shifts) * 100, max(shifts) * 100,
           foot_min, foot_min_joint, src_foot_min, drift_len, gait_mps, yaw_total,
           "  seam pos %.1f mm / rot %.1f deg" % (seam["pos_rms_m"] * 1000, seam["rot_max_deg"])
           if seam else ""))
    return count, data, meta, worlds


# ----------------------------------------------------------------- validate
def validate(base, out_path, log):
    clip = Doc(out_path)
    ok = True
    if list(clip.parent) != list(base.parent):
        log("  FAIL %s: joint order differs from base" % out_path)
        ok = False
    if clip.skin_joints != base.skin_joints:
        log("  FAIL %s: skin joint list differs from base" % out_path)
        ok = False
    for j in base.skin_joints:
        a, b = clip.inv_bind[j], base.inv_bind[j]
        if any(abs(a[r][c] - b[r][c]) > 1e-9 for r in range(4) for c in range(4)):
            log("  FAIL %s: inverse bind of %s differs" % (out_path, j))
            ok = False
            break
    anim = clip.animated_joints()
    if set(anim) != set(base.parent):
        log("  FAIL %s: animated joints %d != base joints %d" % (out_path, len(anim), len(base.parent)))
        ok = False
    n = None
    for j in anim:
        arrs = clip.anim_arrays(j)
        lens = [len(a[2]) for a in arrs[:3]] + [len(arrs[3][2]) // 3]
        if n is None:
            n = lens[0]
        if any(l != n for l in lens):
            log("  FAIL %s: %s key counts %s" % (out_path, j, lens))
            ok = False
            break
    t = re.search(r'<float_array id="hpl_%s-time-array" count="(\d+)">([^<]*)<' % HIPS, clip.txt)
    if not t or [int(v) for v in t.group(2).split()] != list(range(int(t.group(1)))):
        log("  FAIL %s: time array is not 0..N-1" % out_path)
        ok = False
    return ok, n


# --------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", required=True, help="<models>/<name>.dae written by hpl_dae_export.py")
    ap.add_argument("--bvh-dir", required=True, help="Motifect pack BVH directory")
    ap.add_argument("--out", required=True, help="output models directory")
    ap.add_argument("--name", required=True, help="clip file prefix (<name>_<slot>.dae)")
    ap.add_argument("--scale", default="auto",
                    help="metres per BVH cm for the hips translation: 'auto' = leg-length ratio")
    ap.add_argument("--stand-hips-cm", type=float, default=STAND_HIPS_CM,
                    help="BVH standing hips height mapped onto the base rest hips (default %.1f)" % STAND_HIPS_CM)
    ap.add_argument("--only", default="", help="comma-separated slot subset")
    ap.add_argument("--no-floor-lock", action="store_true",
                    help="raw hips formula only; do not re-seat the lowest contact joint per frame")
    ap.add_argument("--dry-run", action="store_true", help="compute and check, write nothing")
    args = ap.parse_args()

    def log(msg):
        print(msg)
        sys.stdout.flush()

    tgt = Target(args.base)
    base_txt = strip_animations(tgt.doc.txt)
    log("base %s: %d joints, hips rest %s m" % (args.base, len(tgt.joints),
                                                "(%s)" % " ".join("%.4f" % v for v in tgt.local_trans[HIPS])))
    node_vs_bind = max(rot_angle_deg(mat_mul(mat_t(tgt.local_rot[j]), normalize_rot(tgt.doc.node_rest_rot[j])))
                       for j in tgt.joints)
    log("  node rest vs skin bind local rotation: max %.3f deg (both conventions agree)" % node_vs_bind
        if node_vs_bind < 0.5 else
        "  WARNING node rest differs from skin bind local rotation by up to %.1f deg; binds are used" % node_vs_bind)

    slots = [(s, c) for s, c in SLOTS if not args.only or s in args.only.split(",")]
    if not slots:
        raise SystemExit("--only matched no slot; slots: %s" % " ".join(s for s, _ in SLOTS))
    # the source rest pose (k, A(j)) always comes from the full table's first
    # slot: the pack's files differ in their offsets by ~1e-6 cm, enough to
    # change last digits, and a --only subset must write the same bytes as a
    # full run
    bvh_rest = BVH(os.path.join(args.bvh_dir, SLOTS[0][1]["bvh"] + ".bvh"))

    if args.scale == "auto":
        k = tgt.leg_length() / bvh_leg_length(bvh_rest)
        log("  scale auto: base thigh+shin %.4f m / bvh %.2f cm -> k = %.5f m/cm"
            % (tgt.leg_length(), bvh_leg_length(bvh_rest), k))
    else:
        k = float(args.scale)
        log("  scale: k = %.5f m/cm" % k)

    rt = Retargeter(tgt, bvh_rest, log)

    sidecar = dict(model=args.name, base=os.path.basename(args.base),
                   generator="bvh_to_hpl_clip.py", fps=FPS, scale_m_per_cm=round(k, 6),
                   stand_hips_cm=args.stand_hips_cm,
                   base_hips_rest_m=[round(v, 5) for v in tgt.local_trans[HIPS]],
                   floor_lock=not args.no_floor_lock, target_floor_m=round(tgt.floor(), 5),
                   joint_order=tgt.joints, clips={})
    all_ok = True
    probe = {}
    for slot, cfg in slots:
        count, data, meta, worlds = build_slot(slot, cfg, tgt, rt, args.bvh_dir, k,
                                               args.stand_hips_cm, not args.no_floor_lock, log)
        out_path = os.path.join(args.out, "%s_%s.dae" % (args.name, slot))
        if not args.dry_run:
            write_clip(base_txt, out_path, tgt.joints, count, data)
            ok, n = validate(tgt.doc, out_path, log)
            all_ok = all_ok and ok and n == count
            meta["file"] = os.path.basename(out_path)
            meta["validated"] = bool(ok and n == count)
        sidecar["clips"][slot] = meta
        if slot in ("walk", "idle"):
            _, P0 = worlds[0]
            probe[slot] = {c: [round(v, 4) for v in P0[PFX + c]] for c in ("LeftFoot", "Head", "Hips")}

    for slot, pr in probe.items():
        log("  frame0 %s: LeftFoot %s  Head %s  Hips %s" % (slot, pr["LeftFoot"], pr["Head"], pr["Hips"]))
    if not args.dry_run:
        side_path = os.path.join(args.out, "%s_clips.json" % args.name)
        if args.only and os.path.exists(side_path):
            # subset run: keep the other slots' entries of the existing
            # sidecar, in SLOTS order
            try:
                with open(side_path, encoding="utf-8") as f:
                    old = json.load(f).get("clips", {})
            except ValueError as ex:
                raise SystemExit("%s is not valid JSON (%s); regenerate without --only" % (side_path, ex))
            sidecar["clips"] = {slot: sidecar["clips"].get(slot, old.get(slot))
                                for slot, _ in SLOTS if slot in sidecar["clips"] or slot in old}
        with open(side_path, "w", encoding="utf-8") as f:
            json.dump(sidecar, f, indent=1)
        log("  wrote %s (%s)" % (side_path, "all clips validated" if all_ok else "VALIDATION FAILURES"))
        if not all_ok:
            sys.exit(1)


if __name__ == "__main__":
    main()
