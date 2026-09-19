# Thistle Editor

A prop-placement tool for Thistle's minor-3D drawing mode: spawn a built-in
primitive (cube/sphere/cylinder/cone/plane/trigger) or browse `.obj` files,
move things with the mouse or keyboard, name them, group them into real
parent/child hierarchies, save/load the layout as a `Node` tree.

**This is not a level compiler.** There's no BSP, no lighting bake, none of what
actually makes something like Hammer valuable for a real FPS level. It's the
thinnest real GUI on top of `save_scene()`/`load_scene()` — see
[docs/ui-and-scenes.md](../../docs/ui-and-scenes.md)'s "Placing minor-3D props
on a `Node`" section for exactly what that captures (and doesn't). Use this for
laying out props in a small 3D scene, not for building a shippable level. The
**Trigger** primitive is the same story: it's a named position + size, nothing
more — see "Trigger volumes" below.

## Layout

Outliner (hierarchy tree) on the left, Inspector (selected prop's name +
transform) on the right, both running the full height of the window below
the top bar — there's no spawn palette taking up bottom-of-window space
anymore, since spawning now happens through the Outliner's right-click menu
(see "Adding and deleting" below). A real viewport grid with colored X/Z
axis lines instead of a flat ground plane, a wireframe cage around whatever's
selected instead of a floating marker cube. This is a deliberate redesign
toward how Blender/Unity actually lay their tools out — a real screenshot of
Blender's default window (docs.blender.org's Window System Introduction
page) was checked before writing it, not worked from memory.

There's still no orbiting 3D gizmo ball (no way to draw a fixed screen-space
overlay independent of the main camera) — just the axis-colored grid lines
for orientation. The Inspector's Position/Rotation/Scale fields stay
read-only-number-plus-steppers rather than click-to-type — a mouse-drag
already covers coarse repositioning (see below), and steppers are enough for
fine nudges. The Name field, though, **is** a real click-to-type box: it uses
Thistle's actual `begin_text_input()`/`text_input()` capture (the engine does
have text input, it just has no built-in visual widget for it anywhere — this
is that widget, built for exactly one field).

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

**Primitives** (cube/sphere/cylinder/cone/plane/trigger) are always
available, no assets required — right-click the Outliner to spawn one (see
"Adding and deleting" below).

**Models**: the editor also scans `./assets` (relative to the built
executable) for `.obj` files, recursively, and lists them in that same
right-click menu alongside the primitives. For each one it resolves a
texture by convention, since `load_mesh()` doesn't read `.mtl`:

1. A same-basename `.png` next to the `.obj` (`chest.obj` → `chest.png`), else
2. a shared `atlas.png` or `colormap.png` in that same folder (the common case
   for a kit that shares one texture atlas across many props), else
3. no texture — the prop draws flat-colored.

No `.obj` files ship with this tool (kept it asset-free, same as the engine's
own smoketest) — drop your own into `assets/` next to the built executable.
It was verified during development against Kenney's CC0 "Mini Dungeon" pack
(kenney.nl), which is exactly the atlas-sharing layout convention #2 above is
for.

The editor's own UI font (`editor_assets/inter-regular.ttf`, bundled and
copied next to the executable at build time) is **Inter** by the Inter
Project Authors (github.com/rsms/inter), OFL — the same font Blender's own
UI has used since ~2.9, picked for the same reason. License text travels
with it (`editor_assets/inter-OFL-LICENSE.txt`), as OFL requires. That's a
separate folder from `assets/` on purpose — the tool's own resources and
your project's props should never collide over a shared folder name.

## Mouse control

- **Left-click** a prop (in the viewport, or a row in the Outliner) to select
  it. Clicking empty viewport space deselects — matches Blender's/Unity's own
  convention. Picking works by projecting every node's position to screen
  space and taking the nearest one within ~40px of the click, not real
  ray-mesh intersection (there's no such thing in Thistle to hit-test
  against) — good enough at editor prop-counts, but two overlapping objects
  can be hard to tell apart by click alone (Tab-cycle or the Outliner instead).
- **Left-click-drag** a selected prop to slide it along the ground plane at
  its current height, preserving the offset between where you clicked and its
  center (it doesn't snap its center to the cursor). Height still needs R/F.
- **Right-click-drag** anywhere to orbit the camera (yaw + pitch).
- **Scroll** to zoom. Up/Down keys also zoom, Left/Right keys also orbit yaw.

All of this is built on real camera-ray math (screen position → world ray →
intersect the ground plane), not a shortcut — verified with a standalone
round-trip test (project a known world point to screen, unproject it back,
confirm you land on the same point) before being wired into the editor,
since this is exactly the kind of math that's easy to get subtly backwards.

## Hierarchy

**Adding and deleting**: right-click empty space in the Outliner for an
"Add: X" menu that spawns a new prop at the scene root; right-click an
existing row instead for "Add child: X" (spawns as a *child* of that row)
plus a "Delete" entry (removes that row and everything nested under it).
There's no more bottom palette bar or Shift+click-to-spawn — this replaced
both. Moving, rotating, or scaling a parent moves everything nested under it
too — real grouping, not just a visual nesting in a list.

**Re-parent an existing prop**: select it, then **Ctrl+click** a *different*
row in the Outliner to move the selection under that row instead, preserving
its world position (it won't visually jump). Refuses to create a cycle (you
can't re-parent something onto its own descendant).

See `Node::draw_meshes()`'s doc comment in `include/thistle.hpp` for exactly
how the transform composes (short version: per-axis rotation addition, not a
true rotation-matrix multiply — exact for the common case of everything
sharing one rotation axis, an approximation once nested nodes rotate around
different axes at once). Re-parenting keeps the child's own rotation/scale
values as-is, so its world rotation/scale can visibly shift if the new
parent has a non-identity one — fine under a plain unrotated/unscaled group
node, the common case.

## Trigger volumes

The **Trigger** primitive is pure data: a named position + size, drawn as a
cyan wireframe box in the editor (rotation-aware — a rotated trigger visibly
looks rotated) and **never** drawn as a solid mesh by `Node::draw_meshes()`,
in the editor or in your own game. This is exactly what a brush in a level
editor like Hammer actually is on its own: geometry and a name, with the
*engine* (Source, in Hammer's case — your own game code, here) responsible
for actually testing overlap and doing something about it — Thistle now has
basic 3D overlap tests (`Box3D`/`Sphere3D`, `box3d_sphere3d_overlap()`, etc.,
see docs/drawing.md's "3D collision" section) but nothing calls them
automatically; there's still no "on enter" callback, no event, nothing that
fires on its own. Load the scene, walk the tree for `mesh_prim ==
Prim::Trigger` nodes, build a `Box3D` from `node->world_mesh_transform()`
(parent-composed the same way as any other node — see `Node::draw_meshes()`'s
doc comment), and call the overlap test yourself, every frame.

## Keybindings

| Key | Action |
|---|---|
| Click a prop / Outliner row | Select it |
| Click empty viewport | Deselect |
| Click-drag a selected prop | Move it along the ground at its current height |
| Right-drag | Orbit camera |
| Scroll / Up / Down | Zoom camera |
| Left / Right | Orbit camera (yaw) |
| Right-click empty Outliner space | Open the "Add" menu (spawns at scene root) |
| Right-click an Outliner row | Open the "Add child" / "Delete" menu for that row |
| Ctrl+click a different Outliner row | Re-parent the current selection onto it |
| W / A / S / D | Move selected prop (relative to its own parent) |
| R / F | Move selected prop up / down |
| Q / E | Rotate selected prop (Y axis) |
| Z / X | Scale selected prop down / up |
| Tab | Select next prop (whole tree, not just root-level) |
| Escape | Deselect (or cancel a Name edit in progress) |
| Backspace | Delete selected prop (and everything nested under it) |
| Ctrl+S | Save the scene |
| Ctrl+O | Load the scene |
| Enter (while naming) | Save the Name field |

Save/load always uses one fixed file next to your save data
(`thistle_editor_scene.json`, in the same directory `save::path()` returns) —
there's no file-picker dialog, so it's one layout at a time. If you want more
than that, copy the JSON out between sessions.
