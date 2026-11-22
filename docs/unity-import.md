# Importing a Unity project

```bash
thistle import-unity path/to/UnityProject            # into the current folder
thistle import-unity path/to/UnityProject --out mygame
thistle import-unity path/to/UnityProject --force    # overwrite files that already exist
thistle import-unity path/to/UnityProject --scene "Metro Depot"   # one scene, and only the files it uses
```

`--scene NAME` (repeatable; the file name without `.unity`, any case, or its path) converts just that scene and what it reaches: the models, textures, materials, meshes and prefabs it uses. Without it every scene in the project is converted, which on a project with a lot of sample scenes (a networking package's, say) can be gigabytes. An unknown name lists the scenes there are.

Unity's lights are much hotter than Thistle's for the same numbers, so the importer scales them: `--ambient-scale` (default 0.4), `--light-scale` for point and spot lights (default 0.28), `--sun-scale` (default 1.0). Those defaults were tuned by eye on one game's scenes, not derived: retune if yours comes out dark or white.

Reads the project's `Assets/` folder and writes into the Thistle project's `assets/`. It also writes `unity-import-report.md` next to it: what was converted, what needs converting by hand, what was left behind, and warnings. Read that first. Standard library Python only, like the rest of the CLI.

The Unity project has to be **text-serialized** (Edit > Project Settings > Editor > Asset Serialization = Force Text; the default in modern Unity). A binary scene is reported and skipped.

## What it converts

| Unity | Thistle |
|---|---|
| `.unity` scene | `assets/scenes/<name>.scene.json`, a `three::Scene3D` file ([scene3d.md](scene3d.md)): the GameObject hierarchy with transforms |
| `.prefab` | `assets/prefabs/<name>.scene.json`, the same format as a fragment. Prefab *instances* inside scenes are expanded in place, and the instance's overrides are applied to whichever object they name, anywhere in the prefab (including through nested prefabs): position, rotation, scale, name, active, the mesh, the material, and script fields (as properties). Overrides whose target can't be found are listed in the report by property |
| Cube, Sphere, Cylinder, Plane | `box`, `sphere`, `cylinder`, `plane` (cylinder height and plane size are scaled to match Unity's units). A Capsule becomes a cylinder, with a warning |
| Mesh from `.glb`/`.gltf`/`.obj` | a `model` entity pointing at the copied file |
| A Mesh saved inside a scene or prefab file itself | `assets/meshes/<scene>/<name>_<fileID>.obj`, the same conversion |
| Mesh saved as a Unity `.asset` (MapForge and other tools bake geometry this way) | `assets/<same path>.obj`, and objects using it become `model` entities pointing there. Positions, normals and UVs are read from the mesh's vertex data (float32 and float16, 8/16-bit normalized), indices (16 or 32-bit) and triangle submeshes; Z is mirrored and the winding reversed with it. **Not supported, and reported:** compressed meshes, meshes whose data lives in a separate `.resS` file, and non-triangle submeshes. No materials: the `.obj` has none |
| Mesh from `.fbx` | a `model` entity pointing at `<same name>.glb`. The importer makes the `.glb` itself when **Blender** or **FBX2glTF** is installed (`--blender PATH` to name one; `--no-fbx` to skip it): every animation take is kept as a clip, see [animation.md](animation.md#an-animated-character-from-unity-fbx-idle-run-and-the-rest). Without either, the report lists each file to convert by hand. Blender's FBX add-on needs `numpy`. Conversion runs once per needed `.fbx`, a few seconds each, so a whole project with `--scene` omitted can take a while |
| Other formats Thistle can't read (`.dae`, `.blend`, ...) | a `model` entity pointing at `<same name>.glb`, which you have to make: the report lists each one |
| Light: Point, Spot | `point_light`, `spot_light` (Unity's full spot angle becomes Thistle's half angle) |
| Light: Directional | the scene's sun: direction, color, intensity, shadows on/off |
| RenderSettings | linear fog (exponential fog is approximated), flat and trilight ambient colors |
| Trigger BoxCollider | a `trigger` child with the collider's size and center. A solid BoxCollider on a *model* becomes the property `collider` = `box` (one box around the whole model, not the collider's own size; see [scene3d.md](scene3d.md)); on a shape it's dropped, since shapes are solid already |
| Camera | an `empty` with a `camera_fov` property |
| MonoBehaviour | the object gets `unity_script` and each simple field (numbers, strings, bools) as a property, `ScriptName.field`. **The C# isn't translated.** Object references aren't kept |
| Inactive object, tag, layer | properties `active`=`false`, `tag`, `layer`. Thistle has no active flag; check the property |
| `.mat` | `assets/materials/<name>.json`: color, metallic, roughness (1 - smoothness), emissive, alpha mode and texture paths. A built-in shape or converted mesh that uses one also gets its base texture, bump map and tiling as `texture`, `normal_texture` and `uv_scale` on the entity, and `metallic`/`roughness` when the material is metallic (0.05+) or bumpy; a plain material adds none of that and keeps the classic look. Only PNG, JPG, BMP and TGA load: a texture in another format (PSD, TIF) is left out and reported. A glTF/FBX model's own materials aren't replaced. A built-in shape's, or a converted mesh's, first material color also becomes the entity's color (a glTF/FBX model keeps its own materials, so its color isn't touched). Two materials with the same name in different folders get different file names (`Red.json`, `Red_Walls.json`) |
| Models, textures (png/jpg/bmp/tga), sounds (wav/ogg/mp3/flac), fonts | copied to the same path under `assets/` |

Existing files are kept unless you pass `--force`.

## What it doesn't

- **C# scripts, animation clips and controllers, shaders, UI (Canvas, RectTransform), particle systems, terrain, NavMesh, Timeline, lightmaps, post-processing volumes.** Counted in the report, not converted.
- **Skyboxes.** A scene's skybox material isn't converted; set `world.sky.skybox`.
- **Some prefab overrides.** Nested prefabs expand up to 8 levels deep, but an override on an object inside a prefab *inside* a prefab uses a combined fileID this tool can't resolve; those are counted in the report. Overrides with no Thistle equivalent (UI anchors, collider sizes, particle settings) are listed there too.
- **A mesh used as one piece of a model file.** Unity can point at one mesh inside an `.fbx`; Thistle draws the whole file there, with a warning.
- **Light intensity and range** are copied as numbers. Unity and Thistle don't light the same way (Unity's built-in pipeline is not this engine's), so expect to retune.
- **Scale and handedness.** Unity is left-handed, Thistle is right-handed, so positions and rotations are mirrored across Z. Models Unity imported itself may have been mirrored by Unity's importer; one that arrives flipped is the thing to look for.

## What's been verified

**One real file:** a MapForge ramp mesh (`Ramp_300_1200_380.asset`, 24 vertices) from the owner's Unity game. It decodes, and `load_model()` reads the converted `.obj` with bounds identical to Unity's own `localAABB` (`-1.5 -0.15 -6 / 1.5 3.8 6`). That is the only real Unity file this has seen, and it is a mesh, not a scene: MapForge's map layouts (if it stores them as its own ScriptableObject `.asset`, rather than placing meshes in a scene) are **not** understood, and need a sample to support.


Beyond that one mesh, no real Unity project. No Unity install or sample project was available, so the test (`unity_import_smoketest`, ctest) imports a small project written out by hand in Unity's documented text format: a scene with render settings, a directional light, nested objects, a trigger, a script, a prefab instance with overrides, a model prefab instance, a prefab with a child light, and a `.mat`. It checks the output against hand-computed numbers (mirrored positions and rotations, the sun's direction, the spot angle, roughness) and that `three::Scene3D::load` accepts the scene and the prefab fragment. Real projects have more in them (every Unity version writes slightly differently, and real scenes hold components this doesn't know), so: **try it on one of yours, read the report, and tell me what breaks.** Not checked: that an imported scene looks right on screen.
