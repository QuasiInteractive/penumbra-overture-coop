#!/usr/bin/env python3
"""
Retarget ghost clip DAEs onto the base model's bind skeleton (in place).

WHY THIS EXISTS
---------------
HPL1's CreateAnimTrack stores each rotation key as
    stored = inv(clip_doc_skin_bind_rot) * full_node_rotation(t)
and the runtime recomposes
    rendered = base_doc_skin_bind_rot * stored .
That is only the authored pose when the CLIP document's skin bind equals the
BASE document's. The 2026-07 model package ships THREE different skin binds
(base+crouch_walk = 28 joints "bind A"; idle/walk/run/jump/crouch_idle =
34 joints "bind B"; both stance transitions = "bind C"). Result in game:
idle's keys nearly equal bind B, so inv(B)*keys ~ identity and the ghost
renders bind A = the skin T-POSE; other bind-B/C clips render similarly
corrupted poses.

Translation keys: the loader stores  key - clip_doc_skin_local_bind_trans
and the runtime adds that onto the base skin local bind. The standing batch
authors hip keys as tiny bobs around its own rest (correct under that
scheme; the jump arc PROVES doc-Y renders as world-up), but the crouch and
transition docs author hips in ground space (0.43..0.91 m absolute), which
the scheme turns into garbage heights (part of the waist-deep-floor bug).

WHAT IT DOES (per clip, per animated joint present in the base skeleton)
------------------------------------------------------------------------
Rotations: skinning-equation transplant. Equal mesh deformation between the
clip's own skeleton and the base skeleton requires
    W_base(j,t) * invBind_base(j) = W_clip(j,t) * invBind_clip(j)
in mesh space. With the same node->mesh basis change C in every doc (the
Mesh_0 wrapper) this reduces, in node space, to
    W_base(j,t) = clipNodeFK(j,t) * K(j),
    K(j) = inv(C) * invBind_clip(j) * inv(invBind_base(j)) * C   (constant),
    C    = bind_base_world_rot(hips) * inv(baseNodeRestFK_rot(hips)).
K is the mesh-frame difference between the two rest skeletons' joint frames
(identity when the clip shares the base bind - crouch_walk stays exact, the
bind-B/C batches get their arm/limb frames corrected). The desired runtime
local is then re-localized (inv(parent W) * W) and encoded so the engine's
inv(clip_skin_bind)*full' stores exactly inv(base_skin_bind)*desired. Both
skin-bind rotations are computed exactly the way the engine computes them:
global skin inverse binds inverted, made local against the nearest skinned
ancestor from the node tree (node rest transform for unskinned joints).

Hips translation, ONLY for ground-space docs (hips node rest y >= 0.2):
rewrite the keys so the engine's stored delta equals
    keys(t) - base_standing_hips (node rest, ~1.026)
i.e. crouch_idle renders ~0.59 below standing, transitions sweep between.
Standing-convention docs (rest y < 0.2) are left untouched - they are the
empirically-correct known-good scheme. Non-hip translations are never
touched.

Idempotent-ish: running twice would double-apply the rotation conjugation,
so it refuses to run if a sibling ".retargeted" marker exists.

Usage:  python retarget_clip_dae.py <models_dir> <base_name> [<base_name>...]
        python retarget_clip_dae.py ../models greenlandman phillip
"""
import math
import os
import re
import sys

CLIP_SUFFIXES = ["idle", "walk", "run", "crouch_idle", "crouch_walk",
                 "jump", "stand_to_crouch", "crouch_to_stand",
                 "walk_back", "strafe_walk_l", "strafe_walk_r",
                 "strafe_run_l", "strafe_run_r", "turn_l", "turn_r"]
GROUND_SPACE_REST_Y = 0.2  # hips rest above this = doc authors hips in ground space

# ---------------------------------------------------------------- matrices
def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]

def mat_t(a):
    return [[a[j][i] for j in range(3)] for i in range(3)]

def rot_axis(axis, deg):
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    if axis == "x":
        return [[1, 0, 0], [0, c, -s], [0, s, c]]
    if axis == "y":
        return [[c, 0, s], [0, 1, 0], [-s, 0, c]]
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]

def rot_zyx(z, y, x):
    """Document-order composition: R = Rz * Ry * Rx (column vectors)."""
    return mat_mul(rot_axis("z", z), mat_mul(rot_axis("y", y), rot_axis("x", x)))

def euler_zyx(m):
    """Decompose m = Rz(a)*Ry(b)*Rx(c); returns degrees (a, b, c)."""
    sb = -m[2][0]
    sb = max(-1.0, min(1.0, sb))
    b = math.asin(sb)
    if abs(sb) < 0.999999:
        a = math.atan2(m[1][0], m[0][0])
        c = math.atan2(m[2][1], m[2][2])
    else:  # gimbal: fold everything into a
        a = math.atan2(-m[0][1], m[1][1])
        c = 0.0
    return math.degrees(a), math.degrees(b), math.degrees(c)

def mat4_invert(m):
    r = [row[:3] for row in m[:3]]
    det = (r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1])
           - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0])
           + r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]))
    ri = [[(r[1][1] * r[2][2] - r[1][2] * r[2][1]) / det,
           (r[0][2] * r[2][1] - r[0][1] * r[2][2]) / det,
           (r[0][1] * r[1][2] - r[0][2] * r[1][1]) / det],
          [(r[1][2] * r[2][0] - r[1][0] * r[2][2]) / det,
           (r[0][0] * r[2][2] - r[0][2] * r[2][0]) / det,
           (r[0][2] * r[1][0] - r[0][0] * r[1][2]) / det],
          [(r[1][0] * r[2][1] - r[1][1] * r[2][0]) / det,
           (r[0][1] * r[2][0] - r[0][0] * r[2][1]) / det,
           (r[0][0] * r[1][1] - r[0][1] * r[1][0]) / det]]
    t = [m[0][3], m[1][3], m[2][3]]
    ti = [-(ri[i][0] * t[0] + ri[i][1] * t[1] + ri[i][2] * t[2]) for i in range(3)]
    return [ri[0] + [ti[0]], ri[1] + [ti[1]], ri[2] + [ti[2]], [0, 0, 0, 1]]

def normalize_rot(m):
    """Gram-Schmidt so accumulated float noise cannot skew the decomposition."""
    def norm(v):
        l = math.sqrt(sum(c * c for c in v))
        return [c / l for c in v]
    x = norm([m[0][0], m[1][0], m[2][0]])
    y = [m[0][1], m[1][1], m[2][1]]
    d = sum(x[i] * y[i] for i in range(3))
    y = norm([y[i] - d * x[i] for i in range(3)])
    z = [x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]]
    return [[x[0], y[0], z[0]], [x[1], y[1], z[1]], [x[2], y[2], z[2]]]

# ---------------------------------------------------------------- dae parsing
def fmt(v):
    s = "%.6g" % v
    return "0" if s == "-0" else s

class Doc:
    def __init__(self, path):
        self.path = path
        self.txt = open(path, encoding="utf-8", errors="strict").read()
        self._parse_nodes()
        self._parse_skin()
        self._compute_binds()

    def _parse_nodes(self):
        """Joint tree: id -> (parent joint id or None, rest rot matrix, rest translate)."""
        vs = self.txt[self.txt.index("<library_visual_scenes"):]
        self.parent = {}
        self.node_rest_rot = {}
        self.node_rest_trans = {}
        stack = []  # (id, is_joint)
        for m in re.finditer(r'<node\b([^>]*)>|</node>', vs):
            if m.group(0) == "</node>":
                if stack:
                    stack.pop()
                continue
            attrs = m.group(1)
            nid = re.search(r'id="([^"]*)"', attrs)
            typ = re.search(r'type="([^"]*)"', attrs)
            nid = nid.group(1) if nid else ""
            is_joint = bool(typ and typ.group(1) == "JOINT")
            if is_joint:
                # nearest JOINT ancestor (engine skips non-JOINT wrappers)
                par = None
                for sid, sj in reversed(stack):
                    if sj:
                        par = sid
                        break
                self.parent[nid] = par
                seg_end = vs.find("<node ", m.end())
                seg_close = vs.find("</node>", m.end())
                if seg_end < 0 or (0 <= seg_close < seg_end):
                    seg_end = seg_close
                seg = vs[m.end():seg_end]
                t = re.search(r'<translate[^>]*>([^<]*)</translate>', seg)
                self.node_rest_trans[nid] = [float(v) for v in t.group(1).split()] if t else [0, 0, 0]
                rots = {ax.lower(): float(ang) for ax, ang in
                        re.findall(r'<rotate sid="rotate(\w)">[^<]*? ([-\d.eE+]+)</rotate>', seg)}
                self.node_rest_rot[nid] = rot_zyx(rots.get("z", 0), rots.get("y", 0), rots.get("x", 0))
            stack.append((nid, is_joint))

    def _parse_skin(self):
        j = re.search(r'<Name_array id="[^"]*skin-joints-array"[^>]*>([^<]*)</Name_array>', self.txt)
        b = re.search(r'<float_array id="[^"]*skin-bind_poses-array"[^>]*>([^<]*)</float_array>', self.txt)
        self.skin_joints = j.group(1).split() if j else []
        vals = [float(v) for v in b.group(1).split()] if b else []
        self.inv_bind = {}
        for i, name in enumerate(self.skin_joints):
            self.inv_bind[name] = [vals[i * 16 + r * 4:i * 16 + r * 4 + 4] for r in range(4)]

    def _compute_binds(self):
        """Per-joint LOCAL bind exactly as the engine sees it: skin-attached
        joints get inv(inverse_bind) localized against the nearest skinned
        ancestor's global bind; unskinned joints keep their node rest."""
        self.world_bind = {}
        for name, ib in self.inv_bind.items():
            self.world_bind[name] = mat4_invert(ib)
        self.local_bind_rot = {}
        self.local_bind_trans = {}
        for name in self.parent:
            if name in self.world_bind:
                par = self.parent[name]
                while par is not None and par not in self.world_bind:
                    par = self.parent.get(par)
                g = self.world_bind[name]
                if par is None:
                    loc = g
                else:
                    loc = [[sum(mat4_invert(self.world_bind[par])[i][k] * g[k][j]
                                for k in range(4)) for j in range(4)] for i in range(4)]
                self.local_bind_rot[name] = normalize_rot([row[:3] for row in loc[:3]])
                self.local_bind_trans[name] = [loc[0][3], loc[1][3], loc[2][3]]
            else:
                self.local_bind_rot[name] = normalize_rot(self.node_rest_rot[name])
                self.local_bind_trans[name] = self.node_rest_trans[name]

    def world_bind_rot(self, joint):
        """Rotation part of the raw skin world bind (mesh space)."""
        return normalize_rot([row[:3] for row in self.world_bind[joint][:3]])

    def anim_arrays(self, joint):
        """(z_span, y_span, x_span, trans_span) — (start, end, values) of each
        float_array's text content, or None per channel."""
        out = []
        for tag in ("rotz", "roty", "rotx", "trans"):
            m = re.search(r'<float_array id="hpl_%s-%s-array"[^>]*>([^<]*)</float_array>'
                          % (re.escape(joint), tag), self.txt)
            out.append((m.start(1), m.end(1), [float(v) for v in m.group(1).split()]) if m else None)
        return out

    def animated_joints(self):
        return sorted(set(re.findall(r'<float_array id="hpl_([^"]+)-rotz-array"', self.txt)))

# ---------------------------------------------------------------- retarget
def retarget(base, clip_path, log):
    clip = Doc(clip_path)
    edits = []  # (start, end, new_text)
    hips = "mixamorigwHips"
    ground_space = clip.node_rest_trans.get(hips, [0, 0, 0])[1] >= GROUND_SPACE_REST_Y
    base_stand_hips = base.node_rest_trans[hips]

    # C: node->mesh basis change, from the (self-consistent) base doc's root.
    C = mat_mul(base.world_bind_rot(hips), mat_t(normalize_rot(base.node_rest_rot[hips])))

    # K(j) = inv(C) * invBind_clip * bind_base * C — constant mesh-frame
    # correction between the two rest skeletons (identity when binds match).
    K = {}
    for j in clip.parent:
        if j in clip.world_bind and j in base.world_bind:
            K[j] = normalize_rot(mat_mul(mat_t(C), mat_mul(mat_t(clip.world_bind_rot(j)),
                                 mat_mul(base.world_bind_rot(j), C))))

    # per-joint key arrays (joints without channels FK with their node rest)
    arrays = {j: clip.anim_arrays(j) for j in clip.animated_joints()}
    frame_count = max((len(a[0][2]) for a in arrays.values() if a[0]), default=0)

    # FK the clip pose and the corrected base pose for every frame.
    desired = {j: [] for j in arrays}  # joint -> [local rot per frame]
    for i in range(frame_count):
        clip_fk, base_w = {}, {}
        for j, par in clip.parent.items():
            a = arrays.get(j)
            if a and a[0]:
                zi = min(i, len(a[0][2]) - 1)
                loc = rot_zyx(a[0][2][zi], a[1][2][min(i, len(a[1][2]) - 1)],
                              a[2][2][min(i, len(a[2][2]) - 1)])
            else:
                loc = clip.node_rest_rot[j]
            clip_fk[j] = loc if par is None else mat_mul(clip_fk[par], loc)
            w = mat_mul(clip_fk[j], K[j]) if j in K else clip_fk[j]
            base_w[j] = w
            if j in desired:
                desired[j].append(normalize_rot(w if par is None
                                                else mat_mul(mat_t(base_w[par]), w)))

    skipped = []
    for joint, a in arrays.items():
        if joint not in base.local_bind_rot or not (a[0] and a[1] and a[2]):
            skipped.append(joint)
            continue
        zs, ys, xs, ts = a
        # encode so the engine's inv(clip_skin_bind)*full' stores
        # inv(base_skin_bind)*desired and the runtime renders desired exactly
        conj = mat_mul(clip.local_bind_rot[joint], mat_t(base.local_bind_rot[joint]))
        nz, ny, nx = [], [], []
        for i in range(len(desired[joint])):
            fullp = mat_mul(conj, desired[joint][i])
            va, vb, vc = euler_zyx(normalize_rot(fullp))
            nz.append(va); ny.append(vb); nx.append(vc)
        edits.append((zs[0], zs[1], " ".join(fmt(v) for v in nz[:len(zs[2])])))
        edits.append((ys[0], ys[1], " ".join(fmt(v) for v in ny[:len(ys[2])])))
        edits.append((xs[0], xs[1], " ".join(fmt(v) for v in nx[:len(xs[2])])))

        if joint == hips and ground_space and ts:
            # stored delta must become keys(t) - base standing hips, so the
            # runtime renders the authored ground-space hip motion. The engine
            # subtracts the clip's skin local bind, so pre-add it back here.
            skin_local = clip.local_bind_trans[hips]
            vals = ts[2]
            nt = []
            for i in range(0, len(vals) - 2, 3):
                for ax in range(3):
                    nt.append(fmt(vals[i + ax] - base_stand_hips[ax] + skin_local[ax]))
            edits.append((ts[0], ts[1], " ".join(nt)))

    # apply spans right-to-left so offsets stay valid
    txt = clip.txt
    for start, end, rep in sorted(edits, reverse=True):
        txt = txt[:start] + rep + txt[end:]
    with open(clip_path, "w", encoding="utf-8", newline="") as f:
        f.write(txt)
    log(f"  {os.path.basename(clip_path)}: rewrote {sum(1 for e in edits)} arrays "
        f"({'ground-space hips rebased' if ground_space else 'hips untouched'})"
        + (f", skipped joints not in base skeleton: {len(skipped)}" if skipped else ""))

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    models_dir = sys.argv[1]
    marker = os.path.join(models_dir, ".retargeted")
    if os.path.exists(marker):
        print("refusing to run: %s exists (already retargeted; delete it only "
              "after restoring pristine clip DAEs)" % marker)
        sys.exit(2)
    for name in sys.argv[2:]:
        base = Doc(os.path.join(models_dir, name + ".dae"))
        print(f"{name}: base skeleton {len(base.skin_joints)} skinned joints")
        for suffix in CLIP_SUFFIXES:
            p = os.path.join(models_dir, f"{name}_{suffix}.dae")
            if os.path.exists(p):
                retarget(base, p, print)
    with open(marker, "w") as f:
        f.write("clip DAEs rewritten by retarget_clip_dae.py; restore originals before re-running\n")

if __name__ == "__main__":
    main()
