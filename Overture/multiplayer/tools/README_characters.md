# Making ghost characters (Blender -> game)

This is the guide for modelling a co-op character in Blender and getting it
into the game: either upgrading one of the shipped characters (`phillip`,
`malik`, `fisherman`, `red`) with a better mesh, or adding a new one.

Every character is a set of files in `Overture/multiplayer/models/`:

| file | what it is | made by |
|---|---|---|
| `<name>.dae` | skinned mesh + skeleton (COLLADA, HPL1's own layout) | `hpl_dae_export.py` (Blender) |
| `<name>.tga` | the one diffuse texture, power of two, 24-bit | `hpl_dae_export.py` |
| `<name>.mat` | HPL material pointing at `<name>.tga` | `hpl_dae_export.py` (or the pipeline) |
| `<name>_<slot>.dae` x 15 | animation clips (idle, walk, run, crouch, jump, turns, strafes ...): skeleton + keys + a one-triangle stub mesh, 0.1-0.25 MB each | `bvh_to_hpl_clip.py` |
| `<name>_clips.json` | per-clip gait speeds the runtime reads | `bvh_to_hpl_clip.py` |

One PowerShell command makes all of them: `tools\build_character.ps1`.

## 1. Requirements for the model

- **Humanoid, T-pose or A-pose**, standing, arms away from the body (`red` is
  an A-pose with the arms 44 degrees down, the other three near T-poses with
  11-13 degrees; both work, the clip converter corrects the arm angle per
  joint). Arms that hang more than 60 degrees make the report warn.
- **One mesh.** Several objects are fine: the exporter joins all meshes of
  the FBX into one. Separate parts (eyes, hair, a hat) must be skinned, or
  parented to a bone (then the exporter weights them 100 % to that bone).
- **One material, one texture.** The engine gets exactly one material per
  character (`<name>.mat` -> `<name>.tga`) for every triangle. If the mesh
  uses several materials, only the first material's texture is exported and
  the other parts show the wrong texture: bake everything into one UV
  atlas first (Blender: one UV map, Bake > Diffuse > Color into one image).
- **UVs** on the active UV map, inside 0..1 (the texture wraps, but atlases
  must stay inside).
- **Facing.** In Blender the character stands on the ground facing **-Y**
  (the Front view, numpad 1, looks at the face), its left hand toward +X.
  That is what Mixamo produces and what an FBX from Blender's own exporter
  re-imports as; the exporter converts it to the engine's convention
  (Y-up, facing +Z, left = +X) and prints which one it found
  (`armature space ...` / `axes OK` in the report). Scale: real size in
  metres (1.7 - 1.85 m tall); the report prints the final height.
- **Skeleton = Mixamo joint names.** The 15 clips are retargeted from a
  motion-capture pack by joint name, so the skeleton must use Mixamo's names.

### Rigging: use Mixamo's auto-rigger (recommended)

1. In Blender: apply all transforms (Ctrl+A > All Transforms), export the
   mesh with its texture: File > Export > FBX (Path Mode `Copy`, the
   `Embed Textures` button next to it on), or OBJ + textures in a zip.
2. Go to mixamo.com > Upload Character, place the markers (chin, wrists,
   elbows, knees, groin), **Skeleton LOD: Standard Skeleton or 2 Chain
   Fingers** (the converter drives thumb 1-3 and index 1-3; the other
   fingers only follow the hand).
3. Download **without** picking an animation: **Format FBX Binary (.fbx),
   Skin: With Skin, Pose: T-pose**. ("Without Skin" contains no mesh.)

The joints are then `mixamorig:Hips`, `mixamorig:Spine`, ... Variants that
Mixamo sometimes writes (`mixamorig1:Hips`, `mixamorig_Hips`) or bare
`Hips` are renamed automatically.

### Meshy rigs (meshy.ai auto-rigger)

The four shipped characters are Meshy AI "biped" rigs (FBX with skin and a
`Running` action, which is ignored: the exporter uses the rest pose). They
work as they come, the exporter detects them (`Meshy biped rig detected` in
the report) when the armature has `Spine02` + `Spine01` + `Spine` and no
`mixamorig` bone, and renames in one simultaneous step before the Mixamo
normalisation:

| Meshy bone | becomes | note |
|---|---|---|
| `Spine02` | `Spine` | Meshy counts the spine from the top: `Spine02` is the lowest (child of `Hips`) |
| `Spine01` | `Spine1` | |
| `Spine` | `Spine2` | the chest; parent of neck and shoulders |
| `neck` | `Neck` | |
| `head_end` | `HeadTop_End` | leaf, no weights |
| `headfront` | `HeadFront` | leaf, no weights, follows the head |

The limbs already use Mixamo names (`LeftUpLeg` ... `LeftToeBase`,
`LeftShoulder` ... `LeftHand`). 24 bones, no fingers (the thumb/index
tracks are simply absent; hands follow the forearm). Meshy's armature is
Blender Z-up facing -Y (`armature space is Blender Z-up facing -Y; rotated`
in the report, expected) and 1.70 m tall, so use `-Scale 1.06` for ~1.80 m.
Its mesh is ~30 000 triangles: pass `-DecimateTo 12000`. Its automatic
weights use up to 11 bones per vertex; the exporter keeps the 4 largest,
which moves 8-53 vertices per character by more than 15 % of their weight
(the one warning the shipped characters have; no visible artefact in the
viewer). Use the `*_texture_0.png` next to the FBX as `-Texture` (the
`_normal` / `_metallic` / `_roughness` maps are not used by the engine).

### Rigging it yourself

Allowed, but name the bones exactly like Mixamo (Blender accepts the colon),
with `mixamorig:Hips` as the **only root bone** (no extra "root"/"Armature"
bone above it):

```
mixamorig:Hips
  mixamorig:Spine > Spine1 > Spine2 > Neck > Head (> HeadTop_End)
    Spine2 > LeftShoulder > LeftArm > LeftForeArm > LeftHand
                           > LeftHandThumb1..3 (optional), LeftHandIndex1..3 (optional)
    Spine2 > RightShoulder > ... (mirror)
  mixamorig:LeftUpLeg > LeftLeg > LeftFoot > LeftToeBase (> LeftToe_End)
  mixamorig:RightUpLeg > ... (mirror)
```

(`LeftLeg` is the shin, `LeftUpLeg` the thigh, `Spine2` the chest.) This is
the `MAP` table in `bvh_to_hpl_clip.py`; the report lists any of these that
are missing. Bones with other names are exported but never animated (they
follow their parent). Export FBX with Add Leaf Bones **off**, Armature >
Primary Bone Axis `Y` (default), and weights normalized, 4 per vertex
(Weights > Limit Total, then Normalize All).

## 2. Budgets

| item | recommended | hard limit | why |
|---|---|---|---|
| triangles | 6 000 - 12 000 | 20 000 | CPU skinning, see below |
| weights per vertex | 4 | 4 | `cSubMesh::CompileBonePairs` keeps 4 and drops the rest; the exporter already keeps the 4 largest and renormalizes |
| bones | Mixamo's 41 - 65 | 256 | bone indices are stored as `unsigned char` (`cSubMesh::mpVertexBones`) |
| texture | 2048 x 2048 | 2048, power of two | `cSDLTexture` warns on non-power-of-two sizes (`SDLTexture.cpp`); the exporter resamples to a power of two and writes 24-bit TGA like the shipped ones |
| materials | 1 | 1 | the exporter writes one material |

Why the triangle budget: the engine skins ghosts **on the CPU, every frame**
(`cSubMeshEntity::UpdateGraphics`, position + normal + tangent per
influence), and the exported mesh is de-indexed, so the engine has
**3 vertices per triangle** (the loader never welds them). Indices are 32-bit,
so size is not limited by the format, only by time. A copy of that loop
measures about **0.08 ms per 1 000 triangles per ghost** (-O2, one 2.1 GHz
core): 12 000 triangles = ~1 ms per visible ghost, 20 000 = ~1.7 ms, times
up to 3 visible friends in a 4-player game, plus uploading the skinned
vertices to the GPU. The shipped characters have 12 000 triangles each
(Meshy's ~30 000 decimated with `-DecimateTo 12000`).

Decimating: `-DecimateTo <n>` (`--decimate-to <n>` of `hpl_dae_export.py`)
runs Blender's Decimate modifier (Collapse, triangulated) on the joined mesh
before anything is read, refining the ratio until the result is at or just
under `n`. Edge collapse interpolates the vertex data, so the skin weights
and UVs follow the surviving vertices; the report prints
`decimate: 30871 -> 11999 triangles (...)`. Check the texture seams in the
viewer afterwards (on the shipped four they are clean). A mesh with shape
keys is not decimated (warning): remove them first.

File size: the base `<name>.dae` is ~0.43 KB per triangle (5 MB at 12 000).
The clip files do **not** repeat the mesh (`bvh_to_hpl_clip.py --stub-mesh`,
the default): each is the skeleton, the skin binds, a one-triangle stub and
the keys, 0.1-0.25 MB, ~2.1 MB for all 15. The 2048 x 2048 TGA is 12 MB. A
shipped character is ~19 MB on disk (12 MB of it the texture), all parsed
when a map loads; every player needs the same files, so this is also the
download size. (`--full-mesh` restores the old layout, 15 extra copies of
the base: ~80 MB more per character at 12 000 triangles.)

## 3. Build it (one command)

Needs Blender 3.6 or newer (tested headless with 4.2), Python 3 on PATH
(python.org installer, "Add python.exe to PATH"), and the Motifect BVH pack
(its `BVH` folder). From a PowerShell prompt in the repository:

```powershell
cd C:\PenumbraDev\gitpull\Overture\multiplayer\tools
.\build_character.ps1 -Fbx C:\Art\ella_mixamo.fbx -Name ella -BvhDir C:\Art\motifect\BVH `
    -Redist 'D:\SteamLibrary\steamapps\common\Penumbra Overture\redist',
            'D:\SteamLibrary\steamapps\common\Penumbra Overture\redist_guest'
```

(If scripts are blocked: `powershell -ExecutionPolicy Bypass -File .\build_character.ps1 ...`;
with `-File`, write `-Redist "C:\a,C:\b"` as one string.)

| parameter | default | meaning |
|---|---|---|
| `-Fbx` | (required) | the rigged FBX |
| `-Name` | (required) | lowercase letters/digits, **no `_`** (the runtime splits `<name>_<clip>`) |
| `-BvhDir` | (required unless `-SkipClips`) | Motifect pack `BVH` folder |
| `-Blender` | `C:\Program Files\Blender Foundation\Blender 4.2\blender.exe` | falls back to the newest `Blender *` folder there |
| `-Redist` | none | game folder(s) to install into (`<redist>\multiplayer\models`, stale `*.collcach` deleted) |
| `-Scale` | 0.95 | size factor on top of the FBX (0.95 for a Mixamo FBX; the shipped Meshy characters use 1.06) |
| `-Texture` | the FBX material's image | a png/jpg/tga to use instead |
| `-MaxTex` | 2048 | largest texture edge |
| `-DecimateTo` | 0 (off) | reduce the mesh to at most this many triangles first (Blender Decimate, collapse); 12000 = the recommended budget |
| `-Out` | `Overture\multiplayer\models` | where the files are written |
| `-Python` | `python`, then `py -3` | Python 3 executable |
| `-SkipClips` | off | base model only |

What it does: Blender (headless) runs `hpl_dae_export.py` on the FBX (base
mesh only: `<name>.dae/.tga/.mat` + a validation report), then
`bvh_to_hpl_clip.py` writes the 15 clips and `<name>_clips.json` (it checks
every file it wrote, `all clips validated`), then it prints the summary and
copies everything into each `-Redist`.

The manual equivalent:

```
blender --background --factory-startup --python-exit-code 1 --python hpl_dae_export.py -- ^
    C:\Art\ella.fbx ..\models ella 0.95 --no-clips [--decimate-to 12000] [--texture ella.png]
python bvh_to_hpl_clip.py --base ..\models\ella.dae --bvh-dir C:\Art\motifect\BVH --out ..\models --name ella
```

The shipped four were made with (scale 1.06: Meshy exports 1.70 m tall):

```
blender ... --python hpl_dae_export.py -- <meshy>_Animation_Running_withSkin.fbx - ..\models red 1.06 ^
    --decimate-to 12000 --texture <meshy>_texture_0.png
```

### Reading the report

```
[hpl] Meshy biped rig detected: bones renamed Spine02->Spine, Spine01->Spine1, ...   (Meshy rigs only)
[hpl] decimate: 30871 -> 11999 triangles (target 12000, ...)                         (-DecimateTo only)
[hpl] mesh: 11999 triangles, 5993 unique vertices -> 35997 engine vertices
[hpl] weights: max 11 per vertex before the 4-weight cap; 1571 vertices capped (largest dropped share 29.9%)
[hpl] skeleton: 24 bones (skin joints), root 'mixamorigwHips'
[hpl] BVH retarget joints: 22/34 of bvh_to_hpl_clip MAP present
[hpl]   optional MAP joints absent (fine ...): LeftHandIndex1 ... RightHandThumb3
[hpl] texture: ... source 2048x2048 -> red.tga 2048x2048 24-bit
[hpl] height: 1.80 m (feet at y 0 ...)
[hpl] WARNING: 27 vertices lose more than 15% of their skin weight to the 4-weight cap; ...
[hpl] RESULT: OK with 1 warning(s)
```

Warnings to act on: triangles above 12 000 / 20 000, vertices without any
weight (they stay frozen while the body moves), more than 15 % of a
vertex's weight dropped by the 4-weight cap, several materials, missing core
joints, height outside 1.3 - 2.2 m, a non-power-of-two texture (resampled).
Errors (no `Hips`, `Hips` not the root, no legs, > 256 bones) stop the
pipeline before the clips are made.

## 4. Upgrading a shipped character (phillip, malik, fisherman, red)

Keep the **same name** so every `multiplayer.cfg` stays valid:

```powershell
.\build_character.ps1 -Fbx C:\Art\malik_v2.fbx -Name malik -BvhDir C:\Art\motifect\BVH -Redist ...
```

`malik.dae/.tga/.mat`, all 15 `malik_<slot>.dae` and `malik_clips.json` are
overwritten (the clips must be regenerated: they carry the base's joint list
and bind pose, an old clip on a new base renders garbage). A new Mixamo rig may
have a different joint set (e.g. thumbs now): that is fine, the converter
adapts. Re-check `ghost_body_y` for that model afterwards (section 6).

### The shipped characters

All four are Meshy AI bipeds, built with `-Scale 1.06 -DecimateTo 12000
-Texture <..._texture_0.png>` (2026-09):

| name | source | height | triangles | vertices (unique / engine) | texture | bones | report | on disk (base + tga + mat + 15 clips + json) |
|---|---|---|---|---|---|---|---|---|
| `phillip` | Meshy "Weathered Survivor" (parka), T-pose 11 deg | 1.80 m | 11 999 | 5 965 / 35 997 | 2048 x 2048 | 24 | OK, 1 warning (8 vertices > 15 % weight capped) | 19.1 MB (base 5.0 MB, clips 2.1 MB) |
| `malik` | Meshy "The Weathered Survivor" (hooded jacket), T-pose 13 deg | 1.80 m | 12 000 | 5 990 / 36 000 | 2048 x 2048 | 24 | OK, 1 warning (53 vertices) | 19.0 MB (4.8 / 2.1) |
| `fisherman` | Meshy "Weathered Hunter" (beanie, bag), T-pose 11 deg | 1.74 m | 12 000 | 5 938 / 36 000 | 2048 x 2048 | 24 | OK, 1 warning (9 vertices) | 19.3 MB (5.1 / 2.1) |
| `red` | Meshy "Worn Survivor" (jacket; Red from Penumbra: Overture), A-pose 44 deg | 1.80 m | 11 999 | 5 993 / 35 997 | 2048 x 2048 | 24 | OK, 1 warning (27 vertices) | 19.1 MB (5.0 / 2.1) |

(MB = MiB. Fisherman is shorter in the source; same scale as the others.)
Standing hips 0.93-0.97 m, head joint 1.48-1.55 m; gait speeds in each
`<name>_clips.json` (walk 1.18-1.23 m/s, run 3.84-4.03 m/s).

## 5. A new character

Pick a lowercase name without `_` (`ella`, `viktor2`) and run the pipeline.
With no `ghost_models=` line in `multiplayer.cfg` the game auto-discovers
every `<name>.dae` that has `<name>_*.dae` clips and orders them
phillip, fisherman, red, malik first (`kGhostCharacterOrder` in
`NetworkManager.cpp`; the host plays phillip), then any other name
alphabetically. With a `ghost_models=`
line, add the new file there (v19: the same fixed order is applied to it). hpl.log prints the final list
(`multiplayer: N character(s) ...`). See `../README.md`, section
"Characters". **Every** player must install the same model set, nothing is
sent over the network.

## 6. Checking the result

1. **Viewer:** open `tools/anim_viewer.html` in a browser and drop
   `<name>.dae`, all `<name>_*.dae`, `<name>.tga` and `<name>_clips.json` on
   it. It plays the clips with the engine's own math. Look for: the mesh
   moves with the skeleton everywhere (no stretched spikes = unweighted
   vertices), feet on the floor in idle / walk / crouch, no popping at the
   loop point, the face toward the walking direction.
2. **In game, alone:** in `multiplayer.cfg` set `ghost_preview=1` and
   `ghost_preview_model=N` (0-based index into the character list hpl.log
   prints). A ghost stands 2 m in front of you:
   - **F6 / F7** next / previous clip
   - **F8** toggle crouch (plays the stand <-> crouch transitions)
   - **F2** treadmill off / walk / run (checks gait speed and foot sliding)
3. **Height:** if the feet float or sink, set `ghost_body_ys=` (one value per
   character in list order, metres from the feet, more negative = lower;
   `ghost_body_y=` for all) and `ghost_body_ys_crouch=` for crouching.
   Exported models have their origin at the feet, so 0 is normally right.

## 7. Troubleshooting

| symptom | cause / fix |
|---|---|
| ghost stands in a T-pose (or its rest pose) in game | hpl.log `loaded 0/15 animation clips` / `failed to load clip`, or the clips do not match the base (made from an older `<name>.dae`): re-run `bvh_to_hpl_clip.py` for that name (or the whole pipeline), reinstall, delete `*.collcach` |
| ghost invisible, only the marker light | hpl.log: search `failed to load mesh`, `rejected mesh`, `Couldn't create material`. Usually a missing `<name>.mat`/`<name>.tga` next to the `.dae`, or files not copied to **that** redist |
| the old look / old animation after reinstalling | the engine prefers a `.collcach` newer than the `.dae`: delete `multiplayer\models\*.collcach` (the pipeline does this with `-Redist`) |
| a part (eyes, hair) stays in place while the body moves | unweighted vertices (report: `vertices have NO bone weight`): weight-paint them or parent the object to the head bone |
| spiky / exploding vertices | more than 4 influences dropped hard, or weights not normalized: Weights > Limit Total 4 + Normalize All |
| wrong colours on some parts | several materials: bake to one texture |
| texture looks blurry / stretched | it was not a power of two and got resampled; paint at 1024 or 2048 |
| character tiny or huge | the report's `height` line; fix `-Scale`, or `--unit` for odd FBX units |
| character faces backwards / lies down | the FBX was not Y-up/-Y-facing; the report's `armature space ...` line says what it detected. Re-export (Mixamo, or Blender FBX with default axes) |
| `blender.exe not found` / `Python 3 not found` | pass `-Blender` / `-Python` with the full path |
| clips converter fails: `has no mixamorigwHips joint` | the rig is not Mixamo-named (section 1) |

More on the clips themselves (slot table, retarget math, tuning):
`README_animations.md`.
