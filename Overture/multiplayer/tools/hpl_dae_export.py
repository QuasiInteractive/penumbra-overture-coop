# hpl_dae_export.py — Mixamo FBX -> HPL1-native skeletal COLLADA, written
# directly in the exact format of the proven 2026-07 model package:
#   * joints named mixamorigwXxx ('w' for ':'), nested under the visual scene
#     as <translate>+<rotate rotateZ/Y/X> nodes (HPL recomposes T*Rz*Ry*Rx)
#   * one de-indexed Mesh_0_1 geometry (vpos/vnrm/vuv, count == loop count)
#   * skin controller with vcount (HPL's parser REQUIRES vcount), <=4 wpv
#   * clips: library_animations with hpl_<joint> time/trans/rotz/roty/rotx
#     arrays; time = FRAME NUMBERS (the engine converts @30fps)
#   * ONE material, id == <name>, images init_from <name>.tga (HPL derives the
#     .mat file from the diffuse image name) — every triangle uses it, so a
#     multi-material mesh renders with the first material's texture only
# Every clip file embeds the BASE mesh+skin, so the clip's skin bind equals
# the base's by construction — no retarget pass needed.
#
# Clips: the 15 ghost clips now come from bvh_to_hpl_clip.py (Motifect BVH
# pack), so the clips FBX directory is OPTIONAL. Pass '-' (or --no-clips) to
# export the base <name>.dae only; tools/build_character.ps1 does that and then
# runs the BVH converter. With a directory, every *.fbx in it is exported as
# <stem>.dae exactly as before (legacy Mixamo-FBX clip path).
#
# Root-motion policy for FBX clips (pack clips cannot be downloaded "In Place"):
#   * hips XZ linear drift is subtracted from every clip (network position
#     owns locomotion), Y is kept (jumps, crouches)
#   * turn clips additionally get their linear YAW drift removed (network
#     yaw owns facing)
#
# Besides <name>.dae the script writes <name>.tga (the first material's
# diffuse image, power of two, uncompressed 24-bit, the format of the shipped
# malik.tga / phillip.tga) and <name>.mat, then prints a validation report
# ("[hpl] ..." lines): vertex/triangle counts, weights per vertex before the
# 4-weight cap, bone count, joints the BVH retarget needs, texture size, height.
# Exit code 2 when the report has errors (the base file is still written).
#
# Usage (Blender 3.6 .. 4.x):
#   blender --background --python-exit-code 1 --python hpl_dae_export.py -- \
#       <base_fbx> <clips_fbx_dir|-> <out_dir> <name> [scale] [options]
#   blender --background --python hpl_dae_export.py -- \
#       <base_fbx> <out_dir> <name> [scale] --no-clips [options]
# scale default 0.95 (user wants the characters 5% smaller than v1).
# Options:
#   --no-clips          base mesh only (also: '-' as the clips dir)
#   --texture <file>    diffuse image to use instead of the FBX material's
#                       (png/jpg/tga/...; converted to <name>.tga)
#   --no-texture        do not write <name>.tga / <name>.mat
#   --max-tex <px>      largest texture edge (default 2048); non power-of-two
#                       sizes are resampled to the nearest power of two
#   --unit <m>          metres per armature unit (default: auto — Mixamo cm
#                       FBX = 0.01; otherwise the armature object's scale)
#
# Source conventions: Mixamo FBX in Blender ARMATURE space is Y-up, facing +Z,
# the character's left = +X, units cm. The script checks that from the joints
# (Hips->Head = up, RightUpLeg->LeftUpLeg = left) and, if an FBX from another
# tool arrives in another orientation, rotates all absolute data into that
# convention (identity — byte-identical output — for Mixamo files).

import bpy
import math
import os
import re
import struct
import sys

from mathutils import Matrix, Vector

# Measured convention of Mixamo FBX in Blender ARMATURE space: already Y-up,
# facing +Z, units cm, body centered at the origin. So: no axis change, a
# fixed cm->m unit, and a feet-to-origin Y shift applied to ABSOLUTE data
# (mesh verts, rest worlds/binds, root-bone locals + samples) — never to
# child-relative locals.
UNIT = 0.01

PFX = "mixamorigw"
# Triangle budget (see README_characters.md): the engine skins on the CPU,
# every frame, 3 vertices per triangle (the mesh is de-indexed and
# SplitVertices never welds), for every visible ghost (up to 3 in a
# 4-player game).
TRI_RECOMMENDED = 12000
TRI_MAX = 20000
MAX_WEIGHTS = 4           # cSubMesh::CompileBonePairs keeps 4 per vertex
MAX_BONES = 256           # cSubMesh::mpVertexBones is unsigned char (0..255)
DEFAULT_MAX_TEX = 2048

# Every joint name of Mixamo's auto-rigger skeleton (without the prefix).
_SIDE = ["Shoulder", "Arm", "ForeArm", "Hand", "UpLeg", "Leg", "Foot", "ToeBase",
         "Toe_End", "Eye"]
for _f in ("Thumb", "Index", "Middle", "Ring", "Pinky"):
    _SIDE += ["Hand%s%d" % (_f, i) for i in range(1, 5)]
MIXAMO_JOINTS = set(["Hips", "Spine", "Spine1", "Spine2", "Neck", "Head", "HeadTop_End"]
                    + ["Left" + n for n in _SIDE] + ["Right" + n for n in _SIDE])
# Joints the BVH retarget cannot do without (the body part would stay frozen
# in its rest pose); fingers are optional (malik has no thumbs).
CORE_JOINTS = ["Hips", "Spine", "Spine1", "Spine2", "Neck", "Head"] + [
    s + n for s in ("Left", "Right")
    for n in ("Shoulder", "Arm", "ForeArm", "Hand", "UpLeg", "Leg", "Foot", "ToeBase")]


def converter_map():
    """Target joint names bvh_to_hpl_clip.py drives (its MAP keys)."""
    try:
        here = os.path.dirname(os.path.abspath(__file__))
        if here not in sys.path:
            sys.path.insert(0, here)
        import bvh_to_hpl_clip
        return sorted(bvh_to_hpl_clip.MAP)
    except Exception as ex:  # keep exporting even without the converter
        print("[hpl] note: bvh_to_hpl_clip.py not importable (%s); using the built-in joint list" % ex)
        base = ["Hips", "Spine", "Spine1", "Spine2", "Neck", "Head"]
        for s in ("Left", "Right"):
            base += [s + n for n in ("Shoulder", "Arm", "ForeArm", "Hand", "HandThumb1",
                                     "HandThumb2", "HandThumb3", "HandIndex1", "HandIndex2",
                                     "HandIndex3", "UpLeg", "Leg", "Foot", "ToeBase")]
        return sorted(PFX + n for n in base)


def fnum(v):
    s = "%.6g" % v
    return "0" if s == "-0" else s


def clean_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_fbx(path):
    before = set(bpy.data.objects)
    # automatic_bone_orientation stays False (default): the rest frames must
    # be the FBX's own, they are the skin binds.
    bpy.ops.import_scene.fbx(filepath=path, ignore_leaf_bones=False)
    return [o for o in bpy.data.objects if o not in before]


def find_armature(objs):
    for o in objs:
        if o.type == "ARMATURE":
            return o
    return None


def joint_name(bone_name):
    """Blender bone name -> DAE joint id. Mixamo names in any of their
    variants ('mixamorig:Hips', 'mixamorig1:Hips', 'mixamorig_Hips', bare
    'Hips') all become 'mixamorigwHips', the spelling the converter's MAP and
    the shipped models use; anything else is only made XML-id safe."""
    m = re.match(r"^mixamorig\d*[:_]?(.+)$", bone_name)
    tail = m.group(1) if m else bone_name
    if tail in MIXAMO_JOINTS:
        return PFX + tail
    return bone_name.replace(":", "w").replace(".", "_").replace(" ", "_")


def convert_matrix(m, scale, yoff=0.0):
    """Armature-space 4x4 -> (rot3x3, translation in meters). yoff (source
    units) shifts absolute transforms so the feet sit at Y=0."""
    r = m.to_3x3()
    t = (m.to_translation() - Vector((0.0, yoff, 0.0))) * scale
    return r, t


def decompose_zyx(r):
    """R = Rz(z) @ Ry(y) @ Rx(x); returns degrees."""
    sy = -r[2][0]
    sy = max(-1.0, min(1.0, sy))
    y = math.asin(sy)
    if abs(sy) < 0.999999:
        x = math.atan2(r[2][1], r[2][2])
        z = math.atan2(r[1][0], r[0][0])
    else:  # gimbal: fold everything into x
        x = math.atan2(-r[1][2], r[1][1])
        z = 0.0
    return math.degrees(z), math.degrees(y), math.degrees(x)


def closest_angle(a, ref):
    while a - ref > 180.0:
        a -= 360.0
    while a - ref < -180.0:
        a += 360.0
    return a


def loop_normal_getter(me):
    """Per-corner normal accessor for Blender 3.6 .. 4.x. Up to 4.0 the split
    normals must be computed first (calc_normals_split); 4.1 removed that
    call and exposes Mesh.corner_normals (always valid)."""
    if hasattr(me, "calc_normals_split"):
        try:
            me.calc_normals_split()
        except (AttributeError, RuntimeError):
            pass
    if hasattr(me, "corner_normals"):
        cn = me.corner_normals
        return lambda li: cn[li].vector
    return lambda li: me.loops[li].normal


def detect_axes(arm_obj):
    """Armature-space rotation that brings the rig into the Mixamo convention
    (Y-up, facing +Z, left = +X). None when it already is (the normal case:
    Mixamo FBX), so Mixamo output stays byte-identical."""
    bones = arm_obj.data.bones
    by_j = {joint_name(b.name): b for b in bones}

    def pos(*names):
        for n in names:
            b = by_j.get(PFX + n)
            if b is not None:
                return b.head_local.copy()
        return None

    lo = pos("Hips")
    hi = pos("Head", "Neck", "Spine2")
    left = pos("LeftUpLeg", "LeftArm", "LeftShoulder")
    right = pos("RightUpLeg", "RightArm", "RightShoulder")
    if lo is None or hi is None or left is None or right is None:
        return None, "axes not checked (no Mixamo Hips/Head/Left*/Right* joints); assumed Y-up, facing +Z"
    up = hi - lo
    lr = left - right
    if up.length < 1e-9 or lr.length < 1e-9:
        return None, "axes not checked (degenerate joint positions)"
    up.normalize()
    lr = lr - up * lr.dot(up)
    if lr.length < 1e-9:
        return None, "axes not checked (left/right joints on the up axis)"
    lr.normalize()
    if up.y > 0.97 and lr.x > 0.97:
        return None, "axes OK: Y-up, facing +Z, left = +X (Mixamo convention)"
    if up.z > 0.97 and lr.x > 0.97:
        # Blender's own convention (Z-up, front view = facing -Y): what an
        # FBX written by Blender's exporter re-imports as
        C = Matrix(((1, 0, 0), (0, 0, 1), (0, -1, 0))).to_4x4()
        return C, "armature space is Blender Z-up facing -Y; rotated into Y-up, facing +Z"
    fwd = lr.cross(up)
    C = Matrix(((lr.x, lr.y, lr.z), (up.x, up.y, up.z), (fwd.x, fwd.y, fwd.z))).to_4x4()

    def name(v):
        a = max(range(3), key=lambda i: abs(v[i]))
        return ("-" if v[a] < 0 else "+") + "XYZ"[a]
    return C, ("armature space was %s-up with left = %s; rotated into Y-up, facing +Z"
               % (name(up), name(lr)))


class Rig(object):
    """Everything the writer needs, in Y_UP meters, feet at the origin."""

    def __init__(self, arm_obj, mesh_obj, scale, axes=None):
        self.scale = scale
        self.arm = arm_obj
        self.C = axes  # None (identity) or 4x4 rotation into Y-up/+Z
        C = self.C if self.C is not None else Matrix.Identity(4)
        # feet-to-origin: lowest mesh vertex (armature space, source units)
        self.yoff = 0.0
        if mesh_obj is not None:
            to_arm = arm_obj.matrix_world.inverted() @ mesh_obj.matrix_world
            if self.C is not None:
                to_arm = C @ to_arm
            self.yoff = min((to_arm @ v.co).y for v in mesh_obj.data.vertices)
        # bones in parent-before-child order
        self.bones = []
        def walk(b):
            self.bones.append(b.name)
            for c in b.children:
                walk(c)
        for b in arm_obj.data.bones:
            if b.parent is None:
                walk(b)
        self.jname = {b: joint_name(b) for b in self.bones}
        self.parent = {b: (arm_obj.data.bones[b].parent.name
                           if arm_obj.data.bones[b].parent else None)
                       for b in self.bones}

        # rest transforms (armature space) and locals
        self.rest_world = {}
        self.rest_local_rt = {}
        for b in self.bones:
            mw = arm_obj.data.bones[b].matrix_local
            if self.C is not None:
                mw = C @ mw
            self.rest_world[b] = mw
            if self.parent[b]:
                pm = arm_obj.data.bones[self.parent[b]].matrix_local
                if self.C is not None:
                    pm = C @ pm
                self.rest_local_rt[b] = convert_matrix(pm.inverted() @ mw, scale)
            else:  # root local IS absolute: gets the feet shift
                self.rest_local_rt[b] = convert_matrix(mw, scale, self.yoff)

        # ---- mesh (de-indexed, in armature space) ----
        self.tris = []  # flat loop data
        # validation stats (per unique vertex, before the 4-weight cap)
        self.stats = dict(unique_verts=0, max_raw_wpv=0, capped_verts=0,
                          max_dropped_weight=0.0, unweighted_verts=0, has_uv=False,
                          uv_layers=0, material_tris={})
        if mesh_obj is not None:
            depsgraph = bpy.context.evaluated_depsgraph_get()
            arm_obj.data.pose_position = "REST"
            bpy.context.view_layer.update()
            eval_obj = mesh_obj.evaluated_get(depsgraph)
            me = eval_obj.to_mesh()
            me.calc_loop_triangles()
            loop_normal = loop_normal_getter(me)
            to_arm = arm_obj.matrix_world.inverted() @ mesh_obj.matrix_world
            if self.C is not None:
                to_arm = C @ to_arm
            nrm_m = to_arm.to_3x3().inverted().transposed()
            uvl = me.uv_layers.active.data if me.uv_layers.active else None
            self.stats["has_uv"] = uvl is not None
            self.stats["uv_layers"] = len(me.uv_layers)

            gidx_to_bone = {g.index: g.name for g in mesh_obj.vertex_groups}
            bone_index = {b: i for i, b in enumerate(self.bones)}

            # weights once per unique vertex (sorted, capped, normalized)
            vweights = []
            st = self.stats
            st["unique_verts"] = len(me.vertices)
            for v in me.vertices:
                ws = []
                for g in v.groups:
                    bn = gidx_to_bone.get(g.group)
                    if bn in bone_index and g.weight > 1e-4:
                        ws.append((bone_index[bn], g.weight))
                ws.sort(key=lambda t: -t[1])
                raw_tot = sum(w for _, w in ws)
                st["max_raw_wpv"] = max(st["max_raw_wpv"], len(ws))
                if len(ws) > MAX_WEIGHTS:
                    st["capped_verts"] += 1
                    dropped = sum(w for _, w in ws[MAX_WEIGHTS:]) / (raw_tot or 1.0)
                    st["max_dropped_weight"] = max(st["max_dropped_weight"], dropped)
                if not ws:
                    st["unweighted_verts"] += 1
                ws = ws[:MAX_WEIGHTS]
                tot = sum(w for _, w in ws) or 1.0
                vweights.append([(j, w / tot) for j, w in ws])

            pos, nrm, uv, weights = [], [], [], []
            for lt in me.loop_triangles:
                mi = lt.material_index
                st["material_tris"][mi] = st["material_tris"].get(mi, 0) + 1
                for li in lt.loops:
                    loop = me.loops[li]
                    v = me.vertices[loop.vertex_index]
                    p = (to_arm @ v.co - Vector((0.0, self.yoff, 0.0))) * scale
                    n = nrm_m @ loop_normal(li)
                    n.normalize()
                    pos.append(p)
                    nrm.append(n)
                    # copy: .uv is a view into the evaluated mesh, which
                    # to_mesh_clear() frees before write_dae reads it
                    uv.append(tuple(uvl[li].uv) if uvl else (0.0, 0.0))
                    weights.append(vweights[loop.vertex_index])
            self.pos, self.nrm, self.uv, self.weights = pos, nrm, uv, weights
            eval_obj.to_mesh_clear()
            arm_obj.data.pose_position = "POSE"
        else:
            self.pos = self.nrm = self.uv = self.weights = None

    def inv_bind(self, b):
        r, t = convert_matrix(self.rest_world[b], self.scale, self.yoff)
        m = r.to_4x4()
        m.translation = t
        return m.inverted()


def sample_clip(rig, carm, action, is_turn):
    """Sample on the CLIP'S OWN armature: its rest is grounded (feet at the
    origin) and matches the action's space, so the root needs NO feet shift —
    while every child bone's local equals the shared rig's. Mixing the base
    armature in here put the hips ~0.8 m too high (different rest origins).
    -> (frame_count, {bone: ([trans xyz]*, [z], [y], [x])}) in Y_UP meters."""
    arm = carm
    f0, f1 = action.frame_range
    f0, f1 = int(round(f0)), int(round(f1))
    count = f1 - f0 + 1
    scn = bpy.context.scene
    scn.frame_start, scn.frame_end = f0, f1

    raw = {b: [] for b in rig.bones}  # per-frame local matrices (source units)
    hips = rig.bones[0]
    hips_world = []
    for f in range(f0, f1 + 1):
        scn.frame_set(f)
        pose = arm.pose
        for b in rig.bones:
            pb = pose.bones.get(b)
            if pb is None:  # bone absent from this clip: hold rig rest
                raw[b].append(rig.rest_world[b].copy() if rig.parent[b] is None
                              else (rig.rest_world[rig.parent[b]].inverted()
                                    @ rig.rest_world[b]))
                continue
            pm = (pose.bones[rig.parent[b]].matrix
                  if rig.parent[b] and pose.bones.get(rig.parent[b]) else Matrix.Identity(4))
            m = pm.inverted() @ pb.matrix
            if rig.parent[b] is None and rig.C is not None:
                m = rig.C @ m  # root local is absolute: same axis fix as the base
            raw[b].append(m)
        hw = pose.bones[hips].matrix.copy()
        hips_world.append(rig.C @ hw if rig.C is not None else hw)

    # ---- in-place fixes on the root (armature space, blender units) ----
    t_start = hips_world[0].to_translation()
    t_end = hips_world[-1].to_translation()
    drift = t_end - t_start
    # armature space is Y-up: ground plane is XZ; keep Y (jumps, crouches)
    do_xy = count > 1 and (Vector((drift.x, drift.z)).length * rig.scale > 0.03)
    yaw_total = 0.0
    if is_turn and count > 1:
        # yaw about armature Y between first and last frame
        e0 = hips_world[0].to_euler("YZX")
        e1 = hips_world[-1].to_euler("YZX")
        yaw_total = e1.y - e0.y
        while yaw_total > math.pi:
            yaw_total -= 2 * math.pi
        while yaw_total < -math.pi:
            yaw_total += 2 * math.pi
    if do_xy or abs(yaw_total) > 0.02:
        for i in range(count):
            k = i / float(count - 1)
            m = raw[hips][i]
            if abs(yaw_total) > 0.02:
                # rotate the root pose back around its START position
                pivot = Matrix.Translation(t_start)
                unrot = pivot @ Matrix.Rotation(-yaw_total * k, 4, "Y") @ pivot.inverted()
                m = unrot @ m
            if do_xy:
                m = m.copy()
                m.translation = m.translation - Vector((drift.x * k, 0.0, drift.z * k))
            raw[hips][i] = m

    # ---- convert + decompose with per-joint angle continuity ----
    out = {}
    for b in rig.bones:
        tr, rz, ry, rx = [], [], [], []
        pz = py = px = None
        for m in raw[b]:
            # clip space is already grounded: scale only, no feet shift
            r, t = convert_matrix(m, rig.scale)
            tr.extend((t.x, t.y, t.z))
            z, y, x = decompose_zyx(r)
            if pz is not None:
                z, y, x = closest_angle(z, pz), closest_angle(y, py), closest_angle(x, px)
            pz, py, px = z, y, x
            rz.append(z)
            ry.append(y)
            rx.append(x)
        out[b] = (tr, rz, ry, rx)
    return count, out


# ------------------------------------------------------------------ writer

def w_joint_nodes(rig, out, depth, bone):
    ind = "  " * depth
    j = rig.jname[bone]
    r, t = rig.rest_local_rt[bone]
    z, y, x = decompose_zyx(r)
    out.append('%s<node id="%s" sid="%s" name="%s" type="JOINT">' % (ind, j, j, j))
    out.append('%s  <translate sid="translate">%s %s %s</translate>'
               '<rotate sid="rotateZ">0 0 1 %s</rotate>'
               '<rotate sid="rotateY">0 1 0 %s</rotate>'
               '<rotate sid="rotateX">1 0 0 %s</rotate>'
               % (ind, fnum(t.x), fnum(t.y), fnum(t.z), fnum(z), fnum(y), fnum(x)))
    for b2 in rig.bones:
        if rig.parent[b2] == bone:
            w_joint_nodes(rig, out, depth + 1, b2)
    out.append('%s</node>' % ind)


def write_dae(rig, name, path, anim=None):
    """anim = (frame_count, per-bone arrays) or None for the base file."""
    L = []
    L.append("<?xml version='1.0' encoding='utf-8'?>")
    L.append('<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">')
    L.append('  <asset>')
    L.append('    <contributor><author>hpl_dae_export</author>'
             '<authoring_tool>Blender %s</authoring_tool></contributor>' %
             bpy.app.version_string.split()[0])
    L.append('    <unit name="meter" meter="1" />')
    L.append('    <up_axis>Y_UP</up_axis>')
    L.append('  </asset>')

    # images / effects / materials — HPL derives <name>.mat from the diffuse
    # image file name, so init_from MUST be <name>.tga
    L.append('  <library_images>')
    for kind in ("diffuse", "emission", "normal"):
        L.append('    <image id="%s-%s-image"><init_from>%s.tga</init_from></image>'
                 % (name, kind, name))
    L.append('  </library_images>')
    L.append('  <library_effects>')
    L.append('    <effect id="%s-fx" name="%s"><profile_COMMON>' % (name, name))
    for kind in ("diffuse", "emission"):
        L.append('      <newparam sid="%s-%s-surface"><surface type="2D">'
                 '<init_from>%s-%s-image</init_from></surface></newparam>' % (name, kind, name, kind))
        L.append('      <newparam sid="%s-%s-sampler"><sampler2D>'
                 '<source>%s-%s-surface</source></sampler2D></newparam>' % (name, kind, name, kind))
    L.append('      <technique sid="common"><lambert>')
    L.append('        <diffuse><texture texture="%s-diffuse-sampler" texcoord="CHANNEL0" /></diffuse>' % name)
    L.append('      </lambert></technique>')
    L.append('    </profile_COMMON></effect>')
    L.append('  </library_effects>')
    L.append('  <library_materials>')
    L.append('    <material id="%s" name="%s"><instance_effect url="#%s-fx" /></material>'
             % (name, name, name))
    L.append('  </library_materials>')

    # geometry — fully de-indexed
    n = len(rig.pos)
    L.append('  <library_geometries>')
    L.append('    <geometry id="Mesh_0_1" name="Mesh_0"><mesh>')
    L.append('      <source id="Mesh_0_1-vpos"><float_array id="Mesh_0_1-vpos-array" count="%d">%s</float_array>'
             '<technique_common><accessor source="#Mesh_0_1-vpos-array" count="%d" stride="3">'
             '<param name="X" type="float" /><param name="Y" type="float" /><param name="Z" type="float" />'
             '</accessor></technique_common></source>'
             % (n * 3, " ".join(fnum(c) for p in rig.pos for c in (p.x, p.y, p.z)), n))
    L.append('      <source id="Mesh_0_1-vnrm"><float_array id="Mesh_0_1-vnrm-array" count="%d">%s</float_array>'
             '<technique_common><accessor source="#Mesh_0_1-vnrm-array" count="%d" stride="3">'
             '<param name="X" type="float" /><param name="Y" type="float" /><param name="Z" type="float" />'
             '</accessor></technique_common></source>'
             % (n * 3, " ".join(fnum(c) for p in rig.nrm for c in (p.x, p.y, p.z)), n))
    L.append('      <source id="Mesh_0_1-vuv"><float_array id="Mesh_0_1-vuv-array" count="%d">%s</float_array>'
             '<technique_common><accessor source="#Mesh_0_1-vuv-array" count="%d" stride="2">'
             '<param name="S" type="float" /><param name="T" type="float" /></accessor></technique_common></source>'
             % (n * 2, " ".join(fnum(c) for t in rig.uv for c in (t[0], t[1])), n))
    L.append('      <vertices id="Mesh_0_1-vertices"><input semantic="POSITION" source="#Mesh_0_1-vpos" /></vertices>')
    L.append('      <triangles count="%d" material="%s">'
             '<input semantic="VERTEX" source="#Mesh_0_1-vertices" offset="0" />'
             '<input semantic="NORMAL" source="#Mesh_0_1-vnrm" offset="1" />'
             '<input semantic="TEXCOORD" source="#Mesh_0_1-vuv" offset="2" />'
             '<p>%s</p></triangles>'
             % (n // 3, name, " ".join("%d %d %d" % (i, i, i) for i in range(n))))
    L.append('    </mesh></geometry>')
    L.append('  </library_geometries>')

    # skin controller (vcount REQUIRED by the HPL parser)
    flat_w = []
    vcount = []
    vidx = []
    for ws in rig.weights:
        vcount.append(len(ws))
        for j, w in ws:
            vidx.append("%d %d" % (j, len(flat_w)))
            flat_w.append(w)
    L.append('  <library_controllers>')
    L.append('    <controller id="Mesh_0_1-skin" name="skinCluster0"><skin source="#Mesh_0_1">')
    L.append('      <bind_shape_matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</bind_shape_matrix>')
    L.append('      <source id="Mesh_0_1-skin-joints"><Name_array id="Mesh_0_1-skin-joints-array" count="%d">%s</Name_array>'
             '<technique_common><accessor source="#Mesh_0_1-skin-joints-array" count="%d" stride="1">'
             '<param name="JOINT" type="Name" /></accessor></technique_common></source>'
             % (len(rig.bones), " ".join(rig.jname[b] for b in rig.bones), len(rig.bones)))
    binds = []
    for b in rig.bones:
        m = rig.inv_bind(b)
        binds.extend(fnum(m[r][c]) for r in range(4) for c in range(4))
    L.append('      <source id="Mesh_0_1-skin-bind_poses"><float_array id="Mesh_0_1-skin-bind_poses-array" count="%d">%s</float_array>'
             '<technique_common><accessor count="%d" offset="0" source="#Mesh_0_1-skin-bind_poses-array" stride="16">'
             '<param name="TRANSFORM" type="float4x4" /></accessor></technique_common></source>'
             % (len(binds), " ".join(binds), len(rig.bones)))
    L.append('      <source id="Mesh_0_1-skin-weights"><float_array id="Mesh_0_1-skin-weights-array" count="%d">%s</float_array>'
             '<technique_common><accessor count="%d" offset="0" source="#Mesh_0_1-skin-weights-array" stride="1">'
             '<param name="WEIGHT" type="float" /></accessor></technique_common></source>'
             % (len(flat_w), " ".join(fnum(w) for w in flat_w), len(flat_w)))
    L.append('      <joints><input semantic="JOINT" source="#Mesh_0_1-skin-joints" />'
             '<input semantic="INV_BIND_MATRIX" source="#Mesh_0_1-skin-bind_poses" /></joints>')
    L.append('      <vertex_weights count="%d">'
             '<input semantic="JOINT" source="#Mesh_0_1-skin-joints" offset="0" />'
             '<input semantic="WEIGHT" source="#Mesh_0_1-skin-weights" offset="1" />'
             '<vcount>%s</vcount><v>%s</v></vertex_weights>'
             % (len(rig.weights), " ".join(str(c) for c in vcount), " ".join(vidx)))
    L.append('    </skin></controller>')
    L.append('  </library_controllers>')

    # visual scene: joint tree, then the mesh node (transform copied verbatim
    # from the proven package — HPL's skinned path ignores it)
    L.append('  <library_visual_scenes>')
    L.append('    <visual_scene id="RootNode" name="RootNode">')
    for b in rig.bones:
        if rig.parent[b] is None:
            w_joint_nodes(rig, L, 3, b)
    root_joint = rig.jname[rig.bones[0]]
    L.append('      <node id="Mesh_0" name="Mesh_0" type="NODE">')
    L.append('        <translate sid="translate">0 0 0</translate>'
             '<rotate sid="rotateZ">0 0 1 0</rotate><rotate sid="rotateY">0 1 0 -0</rotate>'
             '<rotate sid="rotateX">1 0 0 -90</rotate><scale sid="scale">100 100 100</scale>'
             '<instance_controller url="#Mesh_0_1-skin">')
    L.append('          <skeleton>#%s</skeleton>' % root_joint)
    L.append('          <bind_material><technique_common>'
             '<instance_material symbol="defaultMaterial" target="#%s">'
             '<bind_vertex_input semantic="CHANNEL0" input_semantic="TEXCOORD" input_set="0" />'
             '</instance_material></technique_common></bind_material>')
    L.append('        </instance_controller>')
    L.append('      </node>')
    L.append('    </visual_scene>')
    L.append('  </library_visual_scenes>')

    if anim is not None:
        count, data = anim
        times = " ".join(str(i) for i in range(count))
        L.append('  <library_animations>')
        for b in rig.bones:
            j = rig.jname[b]
            tr, rz, ry, rx = data[b]
            L.append('    <animation id="hpl_%s">' % j)
            L.append('      <source id="hpl_%s-time"><float_array id="hpl_%s-time-array" count="%d">%s</float_array>'
                     '<technique_common><accessor source="#hpl_%s-time-array" count="%d" stride="1" />'
                     '</technique_common></source>' % (j, j, count, times, j, count))
            L.append('      <source id="hpl_%s-trans"><float_array id="hpl_%s-trans-array" count="%d">%s</float_array>'
                     '<technique_common><accessor source="#hpl_%s-trans-array" count="%d" stride="3" />'
                     '</technique_common></source>'
                     % (j, j, count * 3, " ".join(fnum(v) for v in tr), j, count))
            for ax, arr in (("rotz", rz), ("roty", ry), ("rotx", rx)):
                L.append('      <source id="hpl_%s-%s"><float_array id="hpl_%s-%s-array" count="%d">%s</float_array>'
                         '<technique_common><accessor source="#hpl_%s-%s-array" count="%d" stride="1" />'
                         '</technique_common></source>'
                         % (j, ax, j, ax, count, " ".join(fnum(v) for v in arr), j, ax, count))
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

    L.append('  <scene><instance_visual_scene url="#RootNode" /></scene>')
    L.append('</COLLADA>')

    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write("\n".join(L))
    print("WROTE %s (%d joints, %d loop-verts%s)" %
          (os.path.basename(path), len(rig.bones), len(rig.pos),
           ", %d frames" % anim[0] if anim else ""))


# ------------------------------------------------------------------ texture

def is_pow2(n):
    return n > 0 and (n & (n - 1)) == 0


def pow2_near(n, cap):
    """Nearest power of two (in log space), at most cap."""
    p = 1 << max(0, int(round(math.log(max(1, n), 2))))
    return min(p, cap)


def _image_upstream(socket, depth=0):
    """First Image Texture node feeding a socket (through mix/color nodes)."""
    if socket is None or depth > 8:
        return None
    for link in socket.links:
        node = link.from_node
        if node.type == "TEX_IMAGE" and node.image is not None:
            return node.image
        for inp in node.inputs:
            img = _image_upstream(inp, depth + 1)
            if img is not None:
                return img
    return None


def material_image(mat):
    """The material's diffuse (base color) image, else any image it uses."""
    if mat is None or not getattr(mat, "use_nodes", True) or mat.node_tree is None:
        return None
    nodes = mat.node_tree.nodes
    for n in nodes:
        if n.type == "BSDF_PRINCIPLED":
            img = _image_upstream(n.inputs.get("Base Color"))
            if img is not None:
                return img
    for n in nodes:
        if n.type in ("BSDF_DIFFUSE", "EMISSION"):
            img = _image_upstream(n.inputs.get("Color"))
            if img is not None:
                return img
    for n in nodes:
        if n.type == "TEX_IMAGE" and n.image is not None:
            return n.image
    return None


def write_tga(img, path, max_tex):
    """Write img as an uncompressed 24-bit, bottom-left-origin TGA (the format
    of the shipped malik.tga / phillip.tga), resampled to power-of-two edges
    of at most max_tex. -> dict(src_w, src_h, w, h, resized, alpha_dropped)."""
    import numpy as np  # bundled with Blender
    w, h = img.size[0], img.size[1]
    if w == 0 or h == 0:
        raise RuntimeError("image '%s' has no pixel data (file '%s' missing or unreadable?)"
                           % (img.name, img.filepath))
    tw, th = pow2_near(w, max_tex), pow2_near(h, max_tex)
    src = img
    if (tw, th) != (w, h):
        src = img.copy()
        src.scale(tw, th)
    buf = np.empty(tw * th * 4, dtype=np.float32)
    src.pixels.foreach_get(buf)
    px = buf.reshape(th, tw, 4)
    alpha_dropped = bool(img.channels == 4 and float(px[:, :, 3].min()) < 0.99)
    rgb = px[:, :, :3]
    if img.is_float:  # float buffers hold scene-linear values -> sRGB
        rgb = np.clip(rgb, 0.0, 1.0)
        rgb = np.where(rgb <= 0.0031308, rgb * 12.92, 1.055 * np.power(rgb, 1.0 / 2.4) - 0.055)
    data = (np.clip(rgb, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)[:, :, ::-1]  # RGB -> BGR
    # Blender rows run bottom to top == TGA descriptor 0 (bottom-left origin)
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, tw, th, 24, 0)
    with open(path, "wb") as f:
        f.write(header)
        f.write(np.ascontiguousarray(data).tobytes())
    if src is not img:
        bpy.data.images.remove(src)
    return dict(src_w=w, src_h=h, w=tw, h=th, resized=(tw, th) != (w, h),
                alpha_dropped=alpha_dropped)


MAT_TEMPLATE = ("<Material>\n"
                "\t<Main Type=\"Diffuse\" DepthTest=\"True\" UseAlpha=\"False\" />\n"
                "\t<TextureUnits>\n"
                "\t\t<Diffuse File=\"%s.tga\" Mipmaps=\"True\" Type=\"2D\" Wrap=\"Repeat\" />\n"
                "\t</TextureUnits>\n"
                "</Material>\n")


def write_mat(out_dir, name):
    """<name>.mat exactly like the shipped malik.mat / phillip.mat."""
    path = os.path.join(out_dir, name + ".mat")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(MAT_TEMPLATE % name)
    return path


# ------------------------------------------------------------------ report

class Report(object):
    def __init__(self):
        self.info, self.warn, self.err = [], [], []

    def i(self, msg):
        self.info.append(msg)

    def w(self, msg):
        self.warn.append(msg)

    def e(self, msg):
        self.err.append(msg)

    def dump(self):
        print("[hpl] ================= character validation =================")
        for m in self.info:
            print("[hpl] %s" % m)
        for m in self.warn:
            print("[hpl] WARNING: %s" % m)
        for m in self.err:
            print("[hpl] ERROR: %s" % m)
        if self.err:
            print("[hpl] RESULT: FAILED (%d error(s), %d warning(s))" % (len(self.err), len(self.warn)))
        elif self.warn:
            print("[hpl] RESULT: OK with %d warning(s)" % len(self.warn))
        else:
            print("[hpl] RESULT: OK")
        print("[hpl] ==========================================================")


def validate(rig, rep, mesh, base_path):
    st = rig.stats
    tris = len(rig.pos) // 3
    rep.i("mesh: %d triangles, %d unique vertices -> %d engine vertices (de-indexed, 3 per triangle)"
          % (tris, st["unique_verts"], len(rig.pos)))
    if tris > TRI_MAX:
        rep.w("%d triangles is above the %d maximum: every visible ghost is skinned on the CPU "
              "each frame (README_characters.md 'Budgets'); decimate the mesh" % (tris, TRI_MAX))
    elif tris > TRI_RECOMMENDED:
        rep.w("%d triangles is above the recommended %d (fine for one character, "
              "4 of them cost CPU every frame)" % (tris, TRI_RECOMMENDED))

    # weights
    rep.i("weights: max %d per vertex before the %d-weight cap; %d vertices capped%s"
          % (st["max_raw_wpv"], MAX_WEIGHTS, st["capped_verts"],
             (" (largest dropped share %.1f%%, renormalized)" % (100.0 * st["max_dropped_weight"]))
             if st["capped_verts"] else ""))
    if st["capped_verts"] and st["max_dropped_weight"] > 0.15:
        rep.w("some vertices lose more than 15%% of their skin weight to the %d-weight cap; "
              "in Blender run Weights > Limit Total (4) + Normalize All and re-check the deformation"
              % MAX_WEIGHTS)
    if st["unweighted_verts"]:
        rep.w("%d vertices have NO bone weight: they stay frozen at the rest pose while the body "
              "moves (weight-paint them, or parent that part to a bone before exporting)"
              % st["unweighted_verts"])

    # bones / names
    jn = [rig.jname[b] for b in rig.bones]
    rep.i("skeleton: %d bones (skin joints), root '%s'" % (len(jn), jn[0] if jn else "-"))
    if len(jn) > MAX_BONES:
        rep.e("%d bones: the engine stores bone indices as unsigned char (max %d)" % (len(jn), MAX_BONES))
    dup = sorted(set(j for j in jn if jn.count(j) > 1))
    if dup:
        rep.e("bone names collide after conversion to DAE ids: %s" % " ".join(dup))
    roots = [rig.jname[b] for b in rig.bones if rig.parent[b] is None]
    if PFX + "Hips" not in jn:
        rep.e("no Hips joint (mixamorig:Hips): bvh_to_hpl_clip.py cannot animate this rig; "
              "rig it with Mixamo or name the bones exactly like README_characters.md lists")
    elif PFX + "Hips" not in roots:
        hb = [b for b in rig.bones if rig.jname[b] == PFX + "Hips"][0]
        rep.e("mixamorig:Hips is not a root bone (parent '%s'): remove the extra root bone "
              "(bvh_to_hpl_clip.py needs Hips at the top)" % rig.parent[hb])
    if len(roots) > 1:
        rep.w("%d root bones (%s): only one (Hips) is expected" % (len(roots), " ".join(roots)))
    foreign = [j for j in jn if not j.startswith(PFX)]
    if foreign:
        rep.w("%d non-Mixamo joint name(s) (not driven by the BVH clips, they follow their parent): %s%s"
              % (len(foreign), " ".join(foreign[:12]), " ..." if len(foreign) > 12 else ""))
    cmap = converter_map()
    missing = [j for j in cmap if j not in jn]
    core_missing = [j for j in missing if j[len(PFX):] in CORE_JOINTS]
    opt_missing = [j for j in missing if j not in core_missing]
    rep.i("BVH retarget joints: %d/%d of bvh_to_hpl_clip MAP present"
          % (len(cmap) - len(missing), len(cmap)))
    if opt_missing:
        rep.i("  optional MAP joints absent (fine, e.g. malik has no thumbs): %s"
              % " ".join(j[len(PFX):] for j in opt_missing))
    if core_missing:
        rep.w("core MAP joints missing, these body parts will not animate: %s"
              % " ".join(j[len(PFX):] for j in core_missing))
    has_leg = any(PFX + s + "Leg" in jn and PFX + s + "Foot" in jn for s in ("Left", "Right"))
    if not has_leg:
        rep.e("no Left/RightLeg + Foot joints: bvh_to_hpl_clip.py --scale auto needs them")
    undriven = [j[len(PFX):] for j in jn if j.startswith(PFX) and j not in cmap
                and not j.endswith("_End") and not j.endswith("4")]
    if undriven:
        rep.i("  Mixamo joints not driven by the BVH (they follow their parent): %s" % " ".join(undriven))

    # pose: arm angle below horizontal (T-pose ~0, malik's A-pose 39)
    for side in ("Left",):
        a = [b for b in rig.bones if rig.jname[b] == PFX + side + "Arm"]
        f = [b for b in rig.bones if rig.jname[b] == PFX + side + "ForeArm"]
        if a and f:
            d = rig.rest_world[f[0]].to_translation() - rig.rest_world[a[0]].to_translation()
            if d.length > 1e-9:
                ang = math.degrees(math.asin(max(-1.0, min(1.0, -d.y / d.length))))
                ang = ang if abs(ang) >= 0.5 else 0.0
                rep.i("rest pose: upper arm %.0f deg below horizontal (T-pose 0, malik's A-pose ~39)" % ang)
                if ang > 60:
                    rep.w("arms hang more than 60 deg in the rest pose: use a T- or A-pose")

    # materials / uv
    used = sorted(st["material_tris"])
    mats = list(mesh.data.materials) if mesh is not None else []
    if len(used) > 1:
        names = ["%s (%d tris)" % (mats[i].name if i < len(mats) and mats[i] else "slot %d" % i,
                                   st["material_tris"][i]) for i in used]
        rep.w("%d materials are used: %s. The engine gets ONE material/texture (<name>.tga) "
              "for every triangle, so the other parts show the wrong texture - bake to one atlas"
              % (len(used), ", ".join(names)))
    if not st["has_uv"]:
        rep.w("the mesh has no UV map: the texture cannot map")
    elif st["uv_layers"] > 1:
        rep.i("%d UV maps; the active one is exported" % st["uv_layers"])

    # size
    ys = [p.y for p in rig.pos]
    span = max(ys) - min(ys)
    rep.i("height: %.2f m (feet at y 0, scale %g m per armature unit)" % (span, rig.scale))
    if span < 1.3 or span > 2.2:
        rep.w("height %.2f m is outside 1.3..2.2 m: check the scale argument / --unit" % span)
    if os.path.exists(base_path):
        mb = os.path.getsize(base_path) / 1048576.0
        rep.i("files: %s %.1f MB; each of the 15 clip files embeds it (~%.0f MB for the whole set)"
              % (os.path.basename(base_path), mb, 16 * mb + 15 * 0.2))


def weight_bone_parented(meshes, arm):
    """A mesh parented to a BONE (Mixamo eyes / hair / props) carries no
    vertex groups; give it a full weight on that bone so it follows after
    the join instead of freezing at the rest pose."""
    for m in meshes:
        if m.parent is arm and m.parent_type == "BONE" and m.parent_bone:
            vg = m.vertex_groups.get(m.parent_bone) or m.vertex_groups.new(name=m.parent_bone)
            vg.add(list(range(len(m.data.vertices))), 1.0, "REPLACE")
            print("[hpl] note: '%s' is parented to bone '%s'; weighted 100%% to it" % (m.name, m.parent_bone))


def parse_args(argv):
    flags = dict(no_clips=False, texture=None, no_texture=False, max_tex=DEFAULT_MAX_TEX, unit=None)
    pos = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--no-clips":
            flags["no_clips"] = True
        elif a == "--no-texture":
            flags["no_texture"] = True
        elif a in ("--texture", "--max-tex", "--unit"):
            if i + 1 >= len(argv):
                raise SystemExit("%s needs a value" % a)
            v = argv[i + 1]
            i += 1
            if a == "--texture":
                flags["texture"] = v
            elif a == "--max-tex":
                flags["max_tex"] = int(v)
            else:
                flags["unit"] = float(v)
        else:
            pos.append(a)
        i += 1

    def isfloat(s):
        try:
            float(s)
            return True
        except ValueError:
            return False

    usage = ("usage: blender --background --python hpl_dae_export.py -- "
             "<base_fbx> <clips_fbx_dir|-> <out_dir> <name> [scale] [--no-clips] "
             "[--texture f] [--no-texture] [--max-tex px] [--unit m]")
    # short form: <fbx> <out> <name> [scale] --no-clips
    if flags["no_clips"] and (len(pos) == 3 or (len(pos) == 4 and isfloat(pos[3]))) \
            and pos[1] != "-":
        base_fbx, clips_dir, out_dir, name = pos[0], None, pos[1], pos[2]
        rest = pos[3:]
    elif len(pos) in (4, 5):
        base_fbx, clips_dir, out_dir, name = pos[0], pos[1], pos[2], pos[3]
        rest = pos[4:]
    else:
        raise SystemExit(usage)
    if clips_dir in ("-", "") or flags["no_clips"]:
        clips_dir = None
    user_scale = float(rest[0]) if rest else 0.95
    if not os.path.isfile(base_fbx):
        raise SystemExit("base FBX not found: %s" % base_fbx)
    if clips_dir is not None and not os.path.isdir(clips_dir):
        raise SystemExit("clips FBX dir not found: %s (pass '-' or --no-clips for the base only)"
                         % clips_dir)
    if not name or "_" in name or name != name.lower() or not re.match(r"^[a-z0-9]+$", name):
        print("[hpl] note: model names are best lowercase letters/digits without '_' "
              "('%s'): the runtime finds clips as <name>_<slot>.dae" % name)
    return base_fbx, clips_dir, out_dir, name, user_scale, flags


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    base_fbx, clips_dir, out_dir, name, user_scale, flags = parse_args(argv)
    rep = Report()

    clean_scene()
    objs = import_fbx(base_fbx)
    arm = find_armature(objs)
    meshes = [o for o in objs if o.type == "MESH"]
    if arm is None:
        raise SystemExit("%s has no armature: rig the character first (Mixamo auto-rigger)" % base_fbx)
    if not meshes:
        raise SystemExit("%s has no mesh" % base_fbx)
    rep.i("source: %s (Blender %s), %d mesh object(s) joined into one"
          % (os.path.basename(base_fbx), bpy.app.version_string.split()[0], len(meshes)))
    weight_bone_parented(meshes, arm)
    if len(meshes) > 1:  # single geometry like the proven package
        # the skinned mesh (Armature modifier) stays the active object
        meshes.sort(key=lambda o: 0 if any(md.type == "ARMATURE" for md in o.modifiers) else 1)
        bpy.ops.object.select_all(action="DESELECT")
        for m in meshes:
            m.select_set(True)
        bpy.context.view_layer.objects.active = meshes[0]
        bpy.ops.object.join()
        meshes = [meshes[0]]
    mesh = meshes[0]

    # units: Mixamo FBX = cm armature space (object scale 0.01) -> exactly UNIT
    if flags["unit"] is not None:
        unit = flags["unit"]
        rep.i("unit: %g m per armature unit (--unit)" % unit)
    else:
        ws = arm.matrix_world.to_scale()
        wsc = (abs(ws.x) + abs(ws.y) + abs(ws.z)) / 3.0
        if abs(wsc - UNIT) <= 0.01 * UNIT:
            unit = UNIT
        else:
            unit = wsc
            # a metre-scaled object over cm data (or the reverse) still ends
            # up the wrong size: fall back to cm when that gives a human
            hb = [b.head_local.length for b in arm.data.bones] or [0.0]
            ext = 2.0 * max(hb)
            if not (0.5 <= ext * unit <= 5.0) and 0.5 <= ext * UNIT <= 5.0:
                unit = UNIT
        rep.i("unit: %g m per armature unit (armature object scale %.4g)" % (unit, wsc))

    axes, axes_note = detect_axes(arm)
    rep.i(axes_note)
    if axes is not None and "Blender Z-up" not in axes_note:
        rep.w("the rig was not in the Mixamo orientation; it was rotated (check the facing "
              "in anim_viewer.html: the face must look toward +Z)")

    scale = unit * user_scale
    rig = Rig(arm, mesh, scale, axes)
    span = (max(p.y for p in rig.pos) - min(p.y for p in rig.pos))
    print("HEIGHT %.2f m (feet shift %.1f source units, scale %g)"
          % (span, rig.yoff, scale))
    os.makedirs(out_dir, exist_ok=True)
    base_path = os.path.join(out_dir, name + ".dae")
    write_dae(rig, name, base_path)

    # texture + .mat
    if flags["no_texture"]:
        rep.i("texture: skipped (--no-texture); %s.tga + %s.mat must come from elsewhere" % (name, name))
    else:
        img = None
        if flags["texture"]:
            img = bpy.data.images.load(os.path.abspath(flags["texture"]))
            src_note = flags["texture"]
        else:
            first = None
            for i in sorted(rig.stats["material_tris"]) or [0]:
                mats = mesh.data.materials
                if i < len(mats) and mats[i] is not None:
                    first = mats[i]
                    break
            img = material_image(first)
            src_note = "material '%s'" % (first.name if first else "-")
            others = set()
            for m in mesh.data.materials:
                oi = material_image(m)
                if oi is not None and oi != img:
                    others.add(oi.name)
            if others:
                rep.w("other textures ignored (one texture per character): %s" % " ".join(sorted(others)))
        if img is None:
            rep.w("no diffuse texture found in the FBX: pass --texture <file> (build_character.ps1 "
                  "-Texture) or put a power-of-two %s.tga next to %s.dae" % (name, name))
        else:
            tga = os.path.join(out_dir, name + ".tga")
            try:
                ti = write_tga(img, tga, flags["max_tex"])
                rep.i("texture: %s from %s: source %dx%d%s -> %s.tga %dx%d 24-bit"
                      % (img.name, src_note, ti["src_w"], ti["src_h"],
                         "" if is_pow2(ti["src_w"]) and is_pow2(ti["src_h"]) else " (not power of two)",
                         name, ti["w"], ti["h"]))
                if ti["resized"]:
                    rep.w("texture resampled %dx%d -> %dx%d (power of two, max %d): paint at a "
                          "power-of-two size to avoid the resample"
                          % (ti["src_w"], ti["src_h"], ti["w"], ti["h"], flags["max_tex"]))
                if ti["alpha_dropped"]:
                    rep.w("the texture's alpha channel is dropped (the .mat is opaque, UseAlpha=False)")
                write_mat(out_dir, name)
                rep.i("material: %s.mat -> %s.tga" % (name, name))
            except Exception as ex:
                rep.w("texture not written: %s" % ex)

    if clips_dir is not None:
        for f in sorted(os.listdir(clips_dir)):
            if not f.lower().endswith(".fbx") or os.path.splitext(f)[0].lower() == name:
                continue
            clip_objs = import_fbx(os.path.join(clips_dir, f))
            carm = find_armature(clip_objs)
            act = carm.animation_data.action if carm and carm.animation_data else None
            if act is None:
                print("SKIP %s: no action" % f)
            else:
                stem = os.path.splitext(f)[0]
                is_turn = stem.endswith("turn_l") or stem.endswith("turn_r")
                anim = sample_clip(rig, carm, act, is_turn)
                write_dae(rig, name, os.path.join(out_dir, stem + ".dae"), anim)
            for o in clip_objs:
                bpy.data.objects.remove(o, do_unlink=True)
    else:
        rep.i("clips: none from FBX (base only); run bvh_to_hpl_clip.py --base %s --name %s"
              % (base_path, name))

    validate(rig, rep, mesh, base_path)
    rep.dump()
    if rep.err:
        sys.exit(2)


if __name__ == "__main__":
    main()
