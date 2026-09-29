# Making ghost characters (Blender -> game)

This is the guide for modelling a co-op character in Blender and getting it
into the game: either upgrading `malik` / `phillip` with a better mesh, or
adding a new character so all four players look different.

Every character is a set of files in `Overture/multiplayer/models/`:

| file | what it is | made by |
|---|---|---|
| `<name>.dae` | skinned mesh + skeleton (COLLADA, HPL1's own layout) | `hpl_dae_export.py` (Blender) |
| `<name>.tga` | the one diffuse texture, power of two, 24-bit | `hpl_dae_export.py` |
| `<name>.mat` | HPL material pointing at `<name>.tga` | `hpl_dae_export.py` (or the pipeline) |
| `<name>_<slot>.dae` x 15 | animation clips (idle, walk, run, crouch, jump, turns, strafes ...) | `bvh_to_hpl_clip.py` |
| `<name>_clips.json` | per-clip gait speeds the runtime reads | `bvh_to_hpl_clip.py` |

One PowerShell command makes all of them: `tools\build_character.ps1`.

## 1. Requirements for the model

- **Humanoid, T-pose or A-pose**, standing, arms away from the body (malik is
  an A-pose with the arms 39 degrees down, phillip a T-pose; both work, the clip
  converter corrects the arm angle per joint). Arms that hang more than 60
  degrees make the report warn.
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
vertices to the GPU. The shipped characters have ~3 100 triangles.

File size: every clip file embeds the whole mesh, so a character is about
**16 x the base file** (~0.4 KB per triangle per file): ~20 MB at 3 000
triangles, ~80 MB at 12 000, ~130 MB at 20 000, all parsed when a map
loads. Every player needs the same files, so this is also download size.

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
| `-Scale` | 0.95 | size factor on top of the FBX (0.95 = what malik/phillip use) |
| `-Texture` | the FBX material's image | a png/jpg/tga to use instead |
| `-MaxTex` | 2048 | largest texture edge |
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
    C:\Art\ella.fbx ..\models ella 0.95 --no-clips
python bvh_to_hpl_clip.py --base ..\models\ella.dae --bvh-dir C:\Art\motifect\BVH --out ..\models --name ella
```

### Reading the report

```
[hpl] mesh: 9870 triangles, 5102 unique vertices -> 29610 engine vertices
[hpl] weights: max 6 per vertex before the 4-weight cap; 212 vertices capped (largest dropped share 8.3%)
[hpl] skeleton: 41 bones (skin joints), root 'mixamorigwHips'
[hpl] BVH retarget joints: 34/34 of bvh_to_hpl_clip MAP present
[hpl] texture: ... source 2048x2048 -> ella.tga 2048x2048 24-bit
[hpl] height: 1.74 m (feet at y 0 ...)
[hpl] RESULT: OK with 0 warning(s)
```

Warnings to act on: triangles above 12 000 / 20 000, vertices without any
weight (they stay frozen while the body moves), more than 15 % of a
vertex's weight dropped by the 4-weight cap, several materials, missing core
joints, height outside 1.3 - 2.2 m, a non-power-of-two texture (resampled).
Errors (no `Hips`, `Hips` not the root, no legs, > 256 bones) stop the
pipeline before the clips are made.

## 4. Upgrading malik or phillip

Keep the **same name** so every `multiplayer.cfg` stays valid:

```powershell
.\build_character.ps1 -Fbx C:\Art\malik_v2.fbx -Name malik -BvhDir C:\Art\motifect\BVH -Redist ...
```

`malik.dae/.tga/.mat`, all 15 `malik_<slot>.dae` and `malik_clips.json` are
overwritten (the clips must be regenerated: they embed the base mesh and its
bind pose, an old clip on a new base renders garbage). A new Mixamo rig may
have a different joint set (e.g. thumbs now): that is fine, the converter
adapts. Re-check `ghost_body_y` for that model afterwards (section 6).

## 5. A new character

Pick a lowercase name without `_` (`ella`, `viktor2`) and run the pipeline.
With no `ghost_models=` line in `multiplayer.cfg` the game auto-discovers
every `<name>.dae` that has `<name>_*.dae` clips and sorts them
alphabetically; player N gets entry (N-1) mod count. With a `ghost_models=`
line, add the new file there. hpl.log prints the final list
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
