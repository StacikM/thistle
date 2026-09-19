# Thistle Editor

A minimal prop-placement tool for Thistle's minor-3D drawing mode: spawn a
built-in primitive (cube/sphere/cylinder/cone/plane) or browse `.obj` files,
nudge props around with the keyboard, group them into real parent/child
hierarchies, save/load the layout as a `Node` tree.

**This is not a level compiler.** There's no BSP, no lighting bake, none of what
actually makes something like Hammer valuable for a real FPS level. It's the
thinnest real GUI on top of `save_scene()`/`load_scene()` — see
[docs/ui-and-scenes.md](../../docs/ui-and-scenes.md)'s "Placing minor-3D props
on a `Node`" section for exactly what that captures (and doesn't). Use this for
laying out props in a small 3D scene, not for building a shippable level.

## Layout

Outliner (hierarchy tree) on the left, Inspector (selected prop's transform)
on the right, spawn palette along the bottom, a real viewport grid with
colored X/Z axis lines instead of a flat ground plane, a wireframe cage
around whatever's selected instead of a floating marker cube. This is a
deliberate redesign toward how Blender/Unity actually lay their tools out —
an real screenshot of Blender's default window (docs.blender.org's Window
System Introduction page) was checked before writing it, not worked from
memory. Adapted to what Thistle's immediate-mode UI actually has, though:
there's no text-input widget anywhere in the engine, so the Inspector's
Position/Rotation/Scale fields are a read-only number plus +/- stepper
buttons rather than click-to-type boxes, and there's no orbiting 3D gizmo
ball (no way to draw a fixed screen-space overlay independent of the main
camera) — just the axis-colored grid lines for orientation instead.

## Building

```bash
thistle editor install   # build it and put `thistle-editor` on your PATH
thistle editor run       # or just build+run it once, without installing
```

Or by hand, since it's a real Thistle project — it links the engine the same
way a scaffolded game would (see the comment in `CMakeLists.txt`), just
pointed at this repo's own engine two directories up instead of a copy:

```bash
cd tools/thistle-editor
cmake -S . -B build
cmake --build build
```

## Running

Just open the built app/executable — it finds its own `assets/`/`editor_assets/`
folders relative to itself, not relative to whatever directory it happened to
be launched from (double-clicking a `.app` in Finder starts it with an
unrelated working directory, so this matters).

**Primitives** (cube/sphere/cylinder/cone/plane) are always available, no
assets required — click one in the bottom bar to spawn it.

**Models**: the editor also scans `./assets` (relative to the built
executable) for `.obj` files, recursively, shown in that same bottom bar next
to the primitives. For each one it resolves a texture by convention, since
`load_mesh()` doesn't read `.mtl`:

1. A same-basename `.png` next to the `.obj` (`chest.obj` → `chest.png`), else
2. a shared `atlas.png` or `colormap.png` in that same folder (the common case
   for a kit that shares one texture atlas across many props), else
3. no texture — the prop draws flat-colored.

No `.obj` files ship with this tool (kept it asset-free, same as the engine's
own smoketest) — drop your own into `assets/` next to the built executable.
It was verified during development against Kenney's CC0 "Mini Dungeon" pack
(kenney.nl), which is exactly the atlas-sharing layout convention #2 above is
for.

The editor's own UI font (`editor_assets/kenney-future.ttf`, bundled and
copied next to the executable at build time) is **Kenney Future** by Kenney
(kenney.nl/assets/kenney-fonts), CC0. That's a separate folder from `assets/`
on purpose — the tool's own resources and your project's props should never
collide over a shared folder name.

## Hierarchy

Select a prop, then **Shift+click** a palette entry to spawn the new prop as
a *child* of the selected one instead of at the scene root. Moving, rotating,
or scaling a parent moves everything nested under it too — real grouping, not
just a visual nesting in a list. See `Node::draw_meshes()`'s doc comment in
`include/thistle.hpp` for exactly how the transform composes (short version:
per-axis rotation addition, not a true rotation-matrix multiply — exact for
the common case of everything sharing one rotation axis, an approximation
once nested nodes rotate around different axes at once).

## Keybindings

| Key | Action |
|---|---|
| Click a palette entry | Spawn into the scene, select it |
| Shift+click a palette entry | Spawn as a child of the current selection |
| Left / Right | Orbit camera |
| Up / Down | Zoom camera |
| W / A / S / D | Move selected prop (relative to its own parent) |
| R / F | Move selected prop up / down |
| Q / E | Rotate selected prop (Y axis) |
| Z / X | Scale selected prop down / up |
| Tab | Select next prop (whole tree, not just root-level) |
| Escape | Deselect |
| Backspace | Delete selected prop (and everything nested under it) |
| Ctrl+S | Save the scene |
| Ctrl+O | Load the scene |

Save/load always uses one fixed file next to your save data
(`thistle_editor_scene.json`, in the same directory `save::path()` returns) —
there's no file-picker dialog, so it's one layout at a time. If you want more
than that, copy the JSON out between sessions.
