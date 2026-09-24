# Thistle Editor

A 3D level editor for `thistle::three`. Place models, shapes, lights, trigger volumes and spawn points, move/rotate/scale them with gizmos, group them, give them properties your game reads, set the sun, sky and fog, and save the result as a `.scene.json` that your game loads with `three::Scene3D` (see [docs/scene3d.md](../../docs/scene3d.md)).

It's drawn with Thistle's own 2D API, not Dear ImGui, and it renders the level with the same `three::World` renderer your game uses: what you see in the editor is what the game draws, lights and shadows included.

Voxel painting and sculpting aren't in it yet. That's the next step.

## Running it

```bash
thistle editor run              # from inside a game project: edits that project
thistle editor run path/to/game # or name the project
thistle editor install          # put `thistle-editor` on your PATH, then: thistle-editor [project]
```

The editor works on one project folder:

- **Models** are the `.glb`, `.gltf` and `.obj` files anywhere under its `assets/`.
- **Scenes** are saved to `assets/scenes/`. They're in `assets/` on purpose: `thistle_bundle_assets()` ships that folder with the game on every platform, so a level the editor saves is a level the game can load, with nothing else to set up.
- Paths inside a scene are relative to the project folder (`assets/ship.glb`), which is also how the game sees them once `assets/` is next to it.

Without a folder, it uses the current one if it has `assets/` or `thistle.json`, else the folder the editor itself is in.

## The window

- **Top bar**: New, Open, Save, Save as, Undo, Redo, the Move/Rotate/Scale tools, `+ Add`, and the file name (orange with a `*` when there are unsaved changes).
- **Outliner** (left): everything in the scene as a tree.
- **Viewport** (middle): the level, drawn by the engine, with a grid, gizmos, and icons for the things that don't draw in the game (lights, triggers, spawn points, empties).
- **Inspector** (right): whatever's selected. With nothing selected, it shows the scene's own settings: sun direction/height/color/intensity/shadows, sky colors, ambient light, fog.
- **Status bar**: the main shortcuts, and what just happened ("saved ...", "moved under ...").

## Moving around

| | |
|---|---|
| Middle-drag | Orbit |
| Shift + middle-drag | Pan |
| Wheel | Zoom |
| Alt + left-drag / Alt + Shift + left-drag | Orbit / pan (for laptops without a middle button) |
| Hold right button | Fly: the mouse looks, WASD moves, Q/E down/up, Shift faster |
| F | Frame the selection |
| Numpad 1 / 3 / 7 | Look from the front / right / top |

That's Blender's orbit-and-pan plus Unity's and Unreal's right-button fly mode.

## Editing

- **Add**: `+ Add` (or Shift+A) has Box, Sphere, Cylinder, Cone, Plane, Point light, Spot light, Trigger volume, Spawn point, Empty (a group), and Model..., which lists the models in `assets/`. New things go below the point the camera orbits around, resting on the ground. Lights go 3 m up.
- **Select**: click in the viewport or the Outliner. Ctrl- or Shift-click adds or removes. Ctrl+A selects everything, Esc selects nothing. Clicking picks the nearest thing under the cursor, and for models that's a test against their triangles, not their box.
- **Gizmos**: W / E / R switch between move, rotate and scale.
  - Move: drag an arrow to go along that axis, or a colored square to slide in that plane.
  - Rotate: drag a ring.
  - Scale: drag a handle to stretch along that axis, or the center square for all three.
  - Move and rotate use the world's axes. Scale uses the object's own axes, since that's what scaling means.
  - Hold **Ctrl** while dragging to snap: 0.5 m, 15°, 0.1×.
  - With several things selected, they move, turn and scale together around their middle.
- **Inspector fields**: drag a number left/right to change it (Shift for fine control), or double-click it and type. Enter or clicking elsewhere keeps what you typed, Esc doesn't. Rotation is shown in degrees.
- **Properties**: `+ property` adds a key/value pair. They're for your game (`health` = `100`, `door` = `exit`) and the editor doesn't interpret them.
- **Hierarchy**: drag a row in the Outliner onto another to put it under that one, or onto empty space to move it back to the top. It keeps its place in the world either way. Moving a parent moves what's under it. You can't put something under its own child: the editor says so and doesn't change anything.
- **Right-click a row**: Rename, Duplicate, Delete, Unparent, Frame.
- **Duplicate** Ctrl+D, **Delete** Del / X / Backspace. Deleting something deletes what's under it.
- **Undo / Redo**: Ctrl+Z, and Ctrl+Shift+Z or Ctrl+Y. That covers every change (adding, gizmo drags, typed values, reparenting, scene settings), 200 steps back.

## Files

- **Save** (Ctrl+S) writes the current file. **Save as** (Ctrl+Shift+S, or Save on a new scene) asks for a name and writes `assets/scenes/<name>.scene.json`.
- **Open** (Ctrl+O) lists the scenes under `assets/` (and any `.scene.json` lying directly in the project folder).
- **New** (Ctrl+N) starts a scene with a ground plane and a `player_start` spawn point.
- New and Open clear the undo history, so with unsaved changes they ask first ("Discard changes" / "Cancel").
- Closing the window doesn't ask. Save first.

### Layouts from the old editor

The editor that came before this one was a prop placer for the 2D engine's minor-3D mode. It saved one layout, as a `Node` tree, to `thistle_editor_scene.json` in its save folder. If that file exists, Open lists **Import the old editor's layout**. The import brings every node across with its name, hierarchy, color and world placement: primitives become shapes, `.obj` props become models, and triggers stay triggers. Use Save as to keep the result.

Two things don't carry over exactly:

- **Textures on `.obj` props.** The old editor paired an `.obj` with a same-named `.png` by convention. Models now take their textures from the `.mtl` file.
- **Rotations nested under other rotated nodes.** Node trees add rotations axis by axis down the chain, while scenes compose them properly. The import keeps each thing where the old editor showed it, so the local numbers under a rotated parent come out different.

## What it doesn't do (yet)

- No voxel tools. That's the next step.
- No prefabs, no multiple scenes open at once, no copy/paste between scenes.
- No play button: run your game to see the level in it.
- Physics isn't set up here. A game reads the scene and makes bodies for what it wants (see [docs/scene3d.md](../../docs/scene3d.md)).
- It doesn't watch `assets/` for changes. The lists refresh when you open Open or `+ Add`.
- There's one viewport. There's no split view and no orthographic camera: the numpad views are perspective.

## What's been verified

Run under Xvfb + llvmpipe (software OpenGL) on Linux and driven with `xdotool`, with a screenshot after each step:

- Adding every kind of thing from `+ Add`, including a glTF model (`Duck.glb`), which lands sitting on the ground.
- Selecting by clicking in the viewport and in the Outliner.
- Dragging the move arrow. The box followed the mouse along X and the Inspector read 2.89.
- Dragging the Y rotation ring. It turned 43.6° about Y, and the saved quaternion was a pure Y rotation, turning the right way.
- Dragging the Y scale handle on a rotated box. It stretched along the box's own axis, Y scale 1.92.
- Undo, redo (Ctrl+Shift+Z and Ctrl+Y), and undoing typed values one at a time.
- Typing values into Position, Rotation and Scale, committed with Enter, with keypad Enter, and by clicking away. Typing into Rotation was broken until this was tested: it wrote to a value that no longer existed. It's fixed.
- Orbit, zoom, fly mode, and F to frame.
- Point and spot lights lighting the ground, with their icons.
- The Outliner's drag-to-parent. After it, moving the parent moved the child too.
- The right-click menu's Delete (and undoing it) and Duplicate. Custom properties. Turning fog on.
- Save as, then Open after restarting the editor. The saved JSON was checked value by value: parents, lights' intensity/range/cone, properties, and fog.
- New with unsaved changes asks first. Cancel keeps the changes and Discard throws them away. The asking came out of this testing: before it, New silently dropped them, and undo couldn't bring them back.
- Importing a hand-written old-editor layout. The positions were checked against hand-computed ones, including a child under a rotated, stretched parent. A malformed old file is refused, not a crash. It was a crash on the first try, since `load_scene()` throws.

`scene3d_smoketest` (ctest) covers the file format and the transform math the editor relies on.

Not verified: any real GPU or display. macOS and Windows haven't been run at all. On a Retina/HiDPI screen, mouse coordinates vs. drawing coordinates are the thing most likely to be off, and that hasn't been seen. The window is 1440×860 by default. The panels are fixed widths, so it's cramped much below about 1100 px wide.

## Building it by hand

It's a regular Thistle project that points at this repo's engine two folders up:

```bash
cd tools/thistle-editor
cmake -S . -B build
cmake --build build
./build/thistle_editor path/to/game
```

## Font

The UI font, `editor_assets/inter-regular.ttf`, is **Inter** by the Inter Project Authors ([github.com/rsms/inter](https://github.com/rsms/inter)), under the OFL. Its license travels with it in `editor_assets/inter-OFL-LICENSE.txt`, as the OFL requires. It's kept in `editor_assets/`, apart from any project's `assets/`, so the editor's own files and your game's never share a folder.
