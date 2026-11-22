# Importing a Unity project

```bash
thistle import-unity path/to/UnityProject            # into the current folder
thistle import-unity path/to/UnityProject --out mygame
thistle import-unity path/to/UnityProject --force    # overwrite files that already exist
```

Reads the project's `Assets/` folder and writes into the Thistle project's `assets/`. It also writes `unity-import-report.md` next to it: what was converted, what needs converting by hand, what was left behind, and warnings. Read that first. Standard library Python only, like the rest of the CLI.

The Unity project has to be **text-serialized** (Edit > Project Settings > Editor > Asset Serialization = Force Text; the default in modern Unity). A binary scene is reported and skipped.

## What it converts

| Unity | Thistle |
|---|---|
| `.unity` scene | `assets/scenes/<name>.scene.json`, a `three::Scene3D` file ([scene3d.md](scene3d.md)): the GameObject hierarchy with transforms |
| `.prefab` | `assets/prefabs/<name>.scene.json`, the same format as a fragment. Prefab *instances* inside scenes are expanded in place |
| Cube, Sphere, Cylinder, Plane | `box`, `sphere`, `cylinder`, `plane` (cylinder height and plane size are scaled to match Unity's units). A Capsule becomes a cylinder, with a warning |
| Mesh from `.glb`/`.gltf`/`.obj` | a `model` entity pointing at the copied file |
| Mesh from `.fbx` (and other formats Thistle can't read) | a `model` entity pointing at `<same name>.glb`, **which you have to make**: convert the file (Blender, FBX2glTF) and put it there. The report lists each one |
| Light: Point, Spot | `point_light`, `spot_light` (Unity's full spot angle becomes Thistle's half angle) |
| Light: Directional | the scene's sun: direction, color, intensity, shadows on/off |
| RenderSettings | linear fog (exponential fog is approximated), flat and trilight ambient colors |
| Trigger BoxCollider | a `trigger` child with the collider's size and center. Solid colliders are dropped: shapes and models are solid already (`add_colliders`) |
| Camera | an `empty` with a `camera_fov` property |
| MonoBehaviour | the object gets `unity_script` and each simple field (numbers, strings, bools) as a property, `ScriptName.field`. **The C# isn't translated.** Object references aren't kept |
| Inactive object, tag, layer | properties `active`=`false`, `tag`, `layer`. Thistle has no active flag; check the property |
| `.mat` | `assets/materials/<name>.json`: color, metallic, roughness (1 - smoothness), emissive, alpha mode and texture paths. **Nothing loads these on its own**; build a `Material` from one in code. A built-in shape's first material color also becomes the entity's color |
| Models, textures (png/jpg/bmp/tga), sounds (wav/ogg/mp3/flac), fonts | copied to the same path under `assets/` |

Existing files are kept unless you pass `--force`.

## What it doesn't

- **C# scripts, animation clips and controllers, shaders, UI (Canvas, RectTransform), particle systems, terrain, NavMesh, Timeline, lightmaps, post-processing volumes.** Counted in the report, not converted.
- **Skyboxes.** A scene's skybox material isn't converted; set `world.sky.skybox`.
- **Objects inside a prefab that a scene overrides.** Only the prefab root's name, position, rotation and scale overrides apply; others are counted in the report. Nested prefabs expand up to 8 levels deep.
- **A mesh used as one piece of a model file.** Unity can point at one mesh inside an `.fbx`; Thistle draws the whole file there, with a warning.
- **Light intensity and range** are copied as numbers. Unity and Thistle don't light the same way (Unity's built-in pipeline is not this engine's), so expect to retune.
- **Scale and handedness.** Unity is left-handed, Thistle is right-handed, so positions and rotations are mirrored across Z. Models Unity imported itself may have been mirrored by Unity's importer; one that arrives flipped is the thing to look for.

## What's been verified

Not a real Unity project. No Unity install or sample project was available, so the test (`unity_import_smoketest`, ctest) imports a small project written out by hand in Unity's documented text format: a scene with render settings, a directional light, nested objects, a trigger, a script, a prefab instance with overrides, a model prefab instance, a prefab with a child light, and a `.mat`. It checks the output against hand-computed numbers (mirrored positions and rotations, the sun's direction, the spot angle, roughness) and that `three::Scene3D::load` accepts the scene and the prefab fragment. Real projects have more in them (every Unity version writes slightly differently, and real scenes hold components this doesn't know), so: **try it on one of yours, read the report, and tell me what breaks.** Not checked: that an imported scene looks right on screen.
