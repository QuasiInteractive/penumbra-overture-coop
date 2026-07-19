# hpl_dae_export.py — Mixamo FBX -> HPL1-native skeletal COLLADA, written
# directly in the exact format of the proven 2026-07 model package:
#   * joints named mixamorigwXxx ('w' for ':'), nested under the visual scene
#     as <translate>+<rotate rotateZ/Y/X> nodes (HPL recomposes T*Rz*Ry*Rx)
#   * one de-indexed Mesh_0_1 geometry (vpos/vnrm/vuv, count == loop count)
#   * skin controller with vcount (HPL's parser REQUIRES vcount), <=4 wpv
#   * clips: library_animations with hpl_<joint> time/trans/rotz/roty/rotx
#     arrays; time = FRAME NUMBERS (the engine converts @30fps)
#   * material id == <name>, images init_from <name>.tga (HPL derives the
#     .mat file from the diffuse image name)
# Every clip file embeds the BASE mesh+skin, so the clip's skin bind equals
# the base's by construction — no retarget pass needed.
#
# Root-motion policy (pack clips cannot be downloaded "In Place"):
#   * hips XZ linear drift is subtracted from every clip (network position
#     owns locomotion), Y is kept (jumps, crouches)
#   * turn clips additionally get their linear YAW drift removed (network
#     yaw owns facing)
#
# Usage:
#   blender --background --python hpl_dae_export.py -- <base_fbx> <clips_fbx_dir> <out_dir> <name> [scale]
# scale default 0.95 (user wants the characters 5% smaller than v1).

import bpy
import math
import os
import sys

from mathutils import Matrix, Vector

# Measured convention of Mixamo FBX in Blender ARMATURE space: already Y-up,
# facing +Z, units cm, body centered at the origin. So: no axis change, a
# fixed cm->m unit, and a feet-to-origin Y shift applied to ABSOLUTE data
# (mesh verts, rest worlds/binds, root-bone locals + samples) — never to
# child-relative locals.
UNIT = 0.01


def fnum(v):
    s = "%.6g" % v
    return "0" if s == "-0" else s


def clean_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_fbx(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path, ignore_leaf_bones=False)
    return [o for o in bpy.data.objects if o not in before]


def find_armature(objs):
    for o in objs:
        if o.type == "ARMATURE":
            return o
    return None


def joint_name(bone_name):
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


class Rig(object):
    """Everything the writer needs, in Y_UP meters, feet at the origin."""

    def __init__(self, arm_obj, mesh_obj, scale):
        self.scale = scale
        self.arm = arm_obj
        # feet-to-origin: lowest mesh vertex (armature space, source units)
        self.yoff = 0.0
        if mesh_obj is not None:
            to_arm = arm_obj.matrix_world.inverted() @ mesh_obj.matrix_world
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
            self.rest_world[b] = mw
            if self.parent[b]:
                pm = arm_obj.data.bones[self.parent[b]].matrix_local
                self.rest_local_rt[b] = convert_matrix(pm.inverted() @ mw, scale)
            else:  # root local IS absolute: gets the feet shift
                self.rest_local_rt[b] = convert_matrix(mw, scale, self.yoff)

        # ---- mesh (de-indexed, in armature space) ----
        self.tris = []  # flat loop data
        if mesh_obj is not None:
            depsgraph = bpy.context.evaluated_depsgraph_get()
            arm_obj.data.pose_position = "REST"
            bpy.context.view_layer.update()
            eval_obj = mesh_obj.evaluated_get(depsgraph)
            me = eval_obj.to_mesh()
            me.calc_loop_triangles()
            try:
                me.calc_normals_split()
            except AttributeError:
                pass  # Blender 4.1+: split normals always available
            to_arm = arm_obj.matrix_world.inverted() @ mesh_obj.matrix_world
            nrm_m = to_arm.to_3x3().inverted().transposed()
            uvl = me.uv_layers.active.data if me.uv_layers.active else None

            gidx_to_bone = {g.index: g.name for g in mesh_obj.vertex_groups}
            bone_index = {b: i for i, b in enumerate(self.bones)}

            pos, nrm, uv, weights = [], [], [], []
            for lt in me.loop_triangles:
                for li in lt.loops:
                    loop = me.loops[li]
                    v = me.vertices[loop.vertex_index]
                    p = (to_arm @ v.co - Vector((0.0, self.yoff, 0.0))) * scale
                    n = nrm_m @ loop.normal
                    n.normalize()
                    pos.append(p)
                    nrm.append(n)
                    uv.append(uvl[li].uv if uvl else (0.0, 0.0))
                    ws = []
                    for g in v.groups:
                        bn = gidx_to_bone.get(g.group)
                        if bn in bone_index and g.weight > 1e-4:
                            ws.append((bone_index[bn], g.weight))
                    ws.sort(key=lambda t: -t[1])
                    ws = ws[:4]
                    tot = sum(w for _, w in ws) or 1.0
                    weights.append([(j, w / tot) for j, w in ws])
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
            raw[b].append(pm.inverted() @ pb.matrix)
        hips_world.append(pose.bones[hips].matrix.copy())

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


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    base_fbx, clips_dir, out_dir, name = argv[0], argv[1], argv[2], argv[3]
    user_scale = float(argv[4]) if len(argv) > 4 else 0.95

    clean_scene()
    objs = import_fbx(base_fbx)
    arm = find_armature(objs)
    meshes = [o for o in objs if o.type == "MESH"]
    if len(meshes) > 1:  # single geometry like the proven package
        bpy.ops.object.select_all(action="DESELECT")
        for m in meshes:
            m.select_set(True)
        bpy.context.view_layer.objects.active = meshes[0]
        bpy.ops.object.join()
        meshes = [meshes[0]]
    mesh = meshes[0]

    scale = UNIT * user_scale
    rig = Rig(arm, mesh, scale)
    span = (max(p.y for p in rig.pos) - min(p.y for p in rig.pos))
    print("HEIGHT %.2f m (feet shift %.1f source units, scale %g)"
          % (span, rig.yoff, scale))
    os.makedirs(out_dir, exist_ok=True)
    write_dae(rig, name, os.path.join(out_dir, name + ".dae"))

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


main()
