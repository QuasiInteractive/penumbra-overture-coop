#!/usr/bin/env python3
"""
make_static_dae.py — convert an assimp/Blender-exported skinned COLLADA file
into a static mesh the HPL1 engine loads reliably.

Why: the ghost DAEs ship <vertex_weights> WITHOUT the <vcount> element, so
HPL1's controller parser skips the skin entirely (MeshLoaderColladaHelpers.cpp
"Couldn't find vertex_weights vcount element"), then writes a .collcach with
an empty PairVec that crashes LoadControllerVec on the next load. The JOINT
nodes alone still make the engine build a skeleton, and a skeleton with no
compiled weights crashes cSubMeshEntity::UpdateGraphics per frame.

Phase-1 ghosts never animate (the mod only sets a world matrix), so the fix
is to strip the skin and let the mesh load as a plain static prop:
  1. drop <library_controllers>
  2. drop the joint-node hierarchy (the "Armature" sibling of the mesh node)
  3. turn <instance_controller> into <instance_geometry> (bind_material kept)
The mesh node's matrix (x100 Mixamo scale + axis rotation) is kept — HPL1
bakes the node world transform into the vertex buffer for static meshes.

HPL1 also resolves materials its own way (MeshLoaderCollada.cpp): the
<triangles material="..."> attribute must equal the <material id="...">, and
the .mat file name is derived from the DIFFUSE image's file name
(CreateMaterial does SetFileExt(image, "mat")). assimp writes
material="defaultMaterial" and images named Image_0.jpg, which resolves to
nothing -> NULL submesh material -> crash in cRenderList::Add. So:
  4. point <triangles material=...> at the real material id
  5. rewrite the diffuse image's <init_from> to <material-id>.tga so the
     engine loads <material-id>.mat (the HPL texture/material pair on disk)

Usage:  python make_static_dae.py in.dae out.dae
"""
import re
import sys
import xml.etree.ElementTree as ET


def make_static(src_path, dst_path):
    with open(src_path, encoding="utf-8") as f:
        dae = f.read()

    # 1) Remove the controller library.
    dae, n = re.subn(r"<library_controllers>.*?</library_controllers>\s*", "", dae, flags=re.S)
    if n != 1:
        raise SystemExit("%s: expected 1 library_controllers, found %d" % (src_path, n))

    # Geometry id — the target for instance_geometry.
    m = re.search(r'<geometry id="([^"]+)"', dae)
    if not m:
        raise SystemExit("%s: no <geometry id> found" % src_path)
    geom_id = m.group(1)

    # 2) Remove the joint hierarchy: the node subtree that precedes the mesh
    #    node inside <visual_scene>. The mesh node is the one holding
    #    instance_controller; joints are its PRECEDING SIBLING subtree.
    mesh_node = re.search(r'<node [^>]*>(?:(?!<node ).)*?<instance_controller', dae, re.S)
    if not mesh_node:
        raise SystemExit("%s: no node with instance_controller found" % src_path)
    scene = re.search(r'(<visual_scene[^>]*>\s*)(.*?)(</visual_scene>)', dae, re.S)
    if not scene:
        raise SystemExit("%s: no visual_scene found" % src_path)
    head = dae[: scene.start(2)]
    body = dae[scene.start(2): scene.end(2)]
    tail = dae[scene.end(2):]
    keep_from = body.rfind("<node ", 0, body.find("<instance_controller"))
    if keep_from < 0:
        raise SystemExit("%s: could not isolate the mesh node" % src_path)
    body = body[keep_from:]
    dae = head + body + tail

    # 3) instance_controller -> instance_geometry (skeleton element dropped,
    #    bind_material kept verbatim).
    dae, n = re.subn(
        r'<instance_controller url="#[^"]*">\s*(?:<skeleton>[^<]*</skeleton>\s*)?',
        '<instance_geometry url="#%s">' % geom_id, dae, flags=re.S)
    if n != 1:
        raise SystemExit("%s: expected 1 instance_controller open tag, found %d" % (src_path, n))
    dae, n = re.subn(r"</instance_controller>", "</instance_geometry>", dae)
    if n != 1:
        raise SystemExit("%s: expected 1 instance_controller close tag, found %d" % (src_path, n))

    # 4) The engine matches <triangles material="..."> against <material id>.
    m = re.search(r'<material id="([^"]+)"', dae)
    if not m:
        raise SystemExit("%s: no <material id> found" % src_path)
    mat_id = m.group(1)
    dae, n = re.subn(r'(<triangles\b[^>]*\bmaterial=")[^"]*(")',
                     r"\g<1>%s\g<2>" % mat_id, dae)
    if n < 1:
        raise SystemExit("%s: no <triangles material=...> found" % src_path)

    # 5) The .mat file name comes from the diffuse image's file name: find the
    #    diffuse chain (effect diffuse -> sampler -> surface -> image id) and
    #    point that image at <material-id>.tga.
    diff = re.search(r'<diffuse>\s*<texture texture="([^"]+)"', dae)
    if not diff:
        raise SystemExit("%s: no diffuse texture found in effect" % src_path)
    ref = diff.group(1)
    for _ in range(4):  # sampler -> surface -> image id
        m = re.search(r'<newparam sid="%s">\s*<(?:sampler2D|surface[^>]*)>\s*<(?:source|init_from)>([^<]+)<' % re.escape(ref), dae)
        if not m:
            break
        ref = m.group(1)
    dae, n = re.subn(r'(<image id="%s">\s*<init_from>)[^<]*(</init_from>)' % re.escape(ref),
                     r"\g<1>%s.tga\g<2>" % mat_id, dae)
    if n != 1:
        raise SystemExit("%s: could not rewrite init_from of diffuse image '%s'" % (src_path, ref))

    # Sanity: still well-formed XML, no controllers/joints left.
    root = ET.fromstring(dae)
    ns = root.tag.split("}")[0] + "}" if root.tag.startswith("{") else ""
    if root.findall(".//%snode[@type='JOINT']" % ns):
        raise SystemExit("%s: joint nodes survived the strip" % src_path)
    if root.findall(".//%sinstance_controller" % ns):
        raise SystemExit("%s: instance_controller survived the strip" % src_path)

    with open(dst_path, "w", encoding="utf-8", newline="") as f:
        f.write(dae)
    print("wrote %s (geometry '%s', static)" % (dst_path, geom_id))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    make_static(sys.argv[1], sys.argv[2])
