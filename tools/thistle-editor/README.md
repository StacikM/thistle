# Thistle Editor

A 3D level editor for `thistle::three`. You can:

- place models, shapes, lights, trigger volumes and spawn points
- build block objects (voxels, flat-colored or Minecraft-style textured) block by block
- move, rotate and scale things with gizmos, and group them
- give things properties your game reads
- set the sun, sky and fog
- drag models in from anywhere

The result saves as a `.scene.json` that your game loads with `three::Scene3D` (see [docs/scene3d.md](../../docs/scene3d.md)).

It's drawn with Thistle's own 2D API, not Dear ImGui, and it renders the level with the same `three::World` renderer your game uses: what you see in the editor is what the game draws, lights and shadows included.

## Running it

```bash
thistle editor run              # from inside a game project: edits that project
thistle editor run path/to/game # or name the project
thistle editor run              # anywhere else: starts on the projects screen
thistle editor install          # put `thistle-editor` on your PATH, then: thistle-editor [project]
```

Started in a project (a folder with a `thistle.json`) or given one, it opens straight into it. Otherwise it starts on the **projects screen**.

## The projects screen

Recent projects on the right, and what to do on the left:

- **New project** is `thistle new` with a form. You pick:
  - a name and where to make it
  - what to start from: first person, third person, block world, or blank 2D
  - optional engine modules: 3D physics (Jolt), and the debug UI (Dear ImGui)
  - whether to open it in the editor straight away, and then build and run it

  It runs the engine's own `thistle` command-line tool (`tools/thistle-cli`, found next to the editor), so a project made here is exactly what the command line makes. That needs Python 3, as the CLI does. The bottom left says whether it was found.
- **Open folder** uses the system's own folder dialog: Explorer on Windows, Finder's on macOS, zenity or kdialog on Linux. Where there's none (Linux without either installed), it opens the editor's own folder browser instead, and with one, "Browse inside the editor" is there too. You can also paste a path, or drop a folder on the window.
- **Opening checks it's a Thistle project** (a `thistle.json` in it):
  - A project opens.
  - A folder inside one (its `assets/`, say) offers to open the project it's in.
  - Any other folder asks first, with three choices:
    - **Open anyway** edits its `assets/` as it is.
    - **Make it a Thistle project** runs `thistle init`, which adds `thistle.json`, a `CMakeLists.txt` and a starter `src/main.cpp`, and changes nothing that's already there.
    - **Cancel**.
- **Recent projects** are remembered between runs, with each one's template, when it was last opened, and the scene it had open. A project's scene reopens with it. One whose folder is gone says so; the x forgets it.

**Projects** at the top left of the editor goes back to this screen (asking first about unsaved changes).

## Build & run

**Build & run** in the top bar (for Thistle projects) runs `thistle run` in the project: it builds the game and starts it. A saved scene with changes is saved first, since the game loads what's on disk.

A strip at the bottom shows how it's going: the build's percentage, then "running", then how it ended. **Log** shows everything the build printed, which is where a compile error is. **Stop** stops the build, or the game and everything it started. The first build compiles the engine, which takes a few minutes; later ones only rebuild what changed. It needs CMake and a C++ compiler, like `thistle build` (see [docs/cli.md](../../docs/cli.md#installing)).

## Working in a project

The editor works on one project folder:

- **Models** are the `.glb`, `.gltf` and `.obj` files anywhere under its `assets/`. Models from elsewhere are copied in when you import them (see [Importing](#importing)).
- **Scenes** are saved to `assets/scenes/`. They're in `assets/` on purpose: `thistle_bundle_assets()` ships that folder with the game on every platform, so a level the editor saves is a level the game can load, with nothing else to set up.
- Paths inside a scene are relative to the project folder (`assets/ship.glb`), which is also how the game sees them once `assets/` is next to it.

## The window

- **Top bar**: Projects, New, Open, Save, Save as, Undo, Redo, the Move/Rotate/Scale tools, `+ Add`, Import, Build & run, and the project and file name (orange with a `*` when there are unsaved changes).
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
| Home | Frame the whole scene (ground and sea planes left out). Opening a scene does this too. |
| Numpad 1 / 3 / 7 | Look from the front / right / top |

That's Blender's orbit-and-pan plus Unity's and Unreal's right-button fly mode.

## Editing

- **Add**: `+ Add` (or Shift+A) has:
  - the shapes (Box, Sphere, Cylinder, Cone, Plane)
  - Point light, Spot light, Trigger volume, Spawn point
  - Empty (a group)
  - Blocks (a new block object; see below)
  - Model..., which lists the models in `assets/`
  - Voxel model (.vox)..., which lists the MagicaVoxel files in `assets/`

  New things go below the point the camera orbits around, resting on the ground. Lights go 3 m up.
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
- **Undo / Redo**: Ctrl+Z, and Ctrl+Shift+Z or Ctrl+Y. That covers every change (adding, gizmo drags, typed values, reparenting, scene settings, block strokes), 200 steps back.

## Block objects (voxels)

A block object is a grid of blocks placed in the level like anything else: it has a position and rotation, can sit under a parent, and moves with the gizmo. The engine gives your game a real `VoxelWorld` for each one, so Physics3D, destruction and multiplayer sync work on it directly (see [docs/scene3d.md](../../docs/scene3d.md)). Its size comes from its block size, not a scale, so it has no Scale field and the scale gizmo leaves it alone.

**Add → Blocks** makes one: an 8×8 grass platform with 13 block types to start with, among them stone, dirt, wood, brick, glass (see-through), water (see-through, not solid) and lamp (glows). It opens straight into **block mode**. **Tab**, or "Edit blocks" in the Inspector, enters block mode for the selected block object. Tab or Esc leaves it.

In block mode, clicking in the viewport edits blocks instead of selecting:

| | |
|---|---|
| **1** Add | Puts the chosen type in front of the face you click |
| **2** Erase | Removes the block you click |
| **3** Paint | Changes the block you click to the chosen type |
| **B** Box | Drag from corner to corner to fill (or erase, or paint) a box |
| **[ ]** | Brush size, 1 to 16 blocks. The Inspector switches between a square and a round brush. In box mode, it's the box's depth |
| **Shift+click** | Picks up the type of the block you click |

- **Dragging stays in one layer.** A drag stays in the layer of blocks where it started, so dragging across a floor draws on the floor, and dragging up a wall draws on the wall. Otherwise each new block would become the next surface, and a line would climb toward the camera (or, erasing, dig a pit). Fast drags are filled in between, so they leave lines, not dots.
- **The box tool works the same way.** Its rectangle lies in the plane of the face you started on, and it goes as deep as the brush size: out from the surface when adding, into it when erasing or painting. Start on a floor for a slab, on a wall for a wall.
- **Past the edge, the object's floor counts as a surface.** You can add blocks beyond the edge of what's there, and start an empty object from nothing.
- **What you'll change is outlined.** A box in the viewport shows it: white for add, red for erase, the block's color for paint.
- **Every stroke, box and block-type edit is one undo step.**

The Inspector in block mode shows the tools and the **block types**: click a swatch to use it. **+ type** adds one. The chosen type's name, color, and "See-through" / "Glows" / "Solid" switches edit it in place, so every block of that type changes. **Block size** is 0.1 m (Teardown), 0.25, 0.5 or 1 m (Minecraft). The object stays where it is and its blocks grow or shrink from its corner.

**Textured blocks** (the Minecraft look): **Texture** picks an image from `assets/` as the object's atlas, a grid of square tiles; set **Tile size** to their size in pixels. Each type then gets **Tiles** fields: which tile its top, sides and bottom use, counted left to right, top to bottom (-1 = plain color). Its color becomes a tint, so set it to white for the texture as drawn.

Adding a block type can't be undone (types only accumulate; an unused one costs nothing).

## Importing

- **Drag files onto the editor window**, or click **Import** and type or paste a path (Ctrl+V; Cmd+V on a Mac).
- **Models from outside the project** are copied into it, with everything they need to load on another machine:
  - `.glb` → `assets/models/`
  - `.gltf` → its own folder under `assets/models/`, with its `.bin` buffers and images, laid out as its references expect
  - `.obj` → its own folder under `assets/models/`, with its `.mtl` files and the textures they name

  Then it's placed in the scene. Importing the same file again reuses the copy, and a different file with the same name gets a numbered name instead of overwriting it. A file already under `assets/` is used where it is.
- **`.vox` (MagicaVoxel)** becomes a block object with 0.1 m blocks. Its blocks are stored in the scene, so the `.vox` isn't copied.
- **Images** (`.png`, `.jpg`) are copied to `assets/textures/`, where a block object's Texture can use them.
- **A `.scene.json`** is opened.

Several files dropped at once are placed side by side. A model whose references point outside its own folder (`../textures/x.png`) is imported without those files, and the status bar says how many were missing.

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
- Block objects:
  - Add → Blocks, then adding blocks one at a time and dragging lines (they stay in the top layer).
  - The box tool with depth 1 and 2, including starting on a side face, which draws a box out from the side.
  - Erase, Paint, the eyedropper, and undoing a box in one step.
  - Moving the object with the gizmo, then undoing the move and then the paint stroke before it, in that order.
  - Saving, restarting and reopening: every block came back.
  - Giving an object a 16 px texture atlas and setting grass (top/side/bottom tiles) and stone. Both drew textured, the palette swatches show the tiles, and the atlas and tiles came back after a restart.
- Importing:
  - Three files dropped onto the window at once, through a real X11 drag and drop (a small XDND source written for the test, the protocol file managers use). `tree.vox` became a 448-block object. `Duck.gltf` was copied with `Duck data.bin` (a `%20` in its reference, decoded) and `textures/duck.png`. `crate.obj` was copied with `crate.mtl` and the `wood.png` it names after a `-s 1 1 1` option. All three drew, with their textures.
  - A path pasted into Import from the X clipboard.
  - Add → Voxel model (.vox) for a `.vox` inside `assets/`.
- Engine bugs this testing turned up, all fixed:
  - The engine's text input stopped at 40 characters. Now `begin_text_input` takes a length.
  - Text input had no paste. It does now.
  - Ctrl+V typed a "v" after the pasted text.
  - Sprites drawn after any text in the same frame came out white. That's why the palette swatches were blank. It affected every Thistle game.
- The three `thistle new` template levels, opened from the Open menu. Each one came up framed (the whole level in view), after a fix: opening used to keep the previous camera, and the voxel template's scene opened looking at a wall from inside its house. Home frames the whole scene again after F zoomed to one crate.
- Editor to game: a target deleted from the fps template's level and saved, then `thistle run` showed 7 targets instead of 8 (see [docs/scene3d.md](../../docs/scene3d.md#whats-been-verified)).
- Importing a hand-written old-editor layout. The positions were checked against hand-computed ones, including a child under a rotated, stretched parent. A malformed old file is refused, not a crash. It was a crash on the first try, since `load_scene()` throws.
- The projects screen, with a fresh home folder so the list started empty:
  - **New project**: an fps project made with "then build and run it". The CLI made it (`"template": "fps"` in its `thistle.json`) and it opened with its level. The build ran with its percentage on the strip, and the game started ("Targets 0 / 8"). Stop closed it, and nothing was left running.
  - **Name checks** live as you type: an invalid name, and one whose folder exists. Esc stops typing first and closes the form second.
  - **Open folder with zenity**: the dialog came up and its folder went through the Thistle-project check. Also the other way in, with neither zenity nor kdialog installed: Open folder is the built-in browser.
  - **Make it a Thistle project** on a plain folder: `thistle init` added its files and left the folder's own `assets/` file alone, then it opened.
  - **Open anyway** on a dropped folder (a real X11 drag and drop): it opened, and nothing was added to the folder.
  - **A folder inside a project** (its `assets/`, pasted as a path): it offered the project, which reopened with the scene it last had open.
  - **The built-in browser**: going into folders, a project marked as one, with its own Open button.
  - **The recent list**: survived a restart, and one whose folder was deleted said so and was forgotten with its x.
  - **Starting with a project folder** (as `thistle editor run` does): straight in, skipping the list.
  - **A failed build**: a syntax error put "The build failed" and the compiler's error line on the strip, and Log showed the whole output.
- Bugs this testing found, all fixed:
  - Coming back to Projects after New project opened a project showed the "Creating..." dialog again.
  - Long paths in command output ran off the dialog.

`scene3d_smoketest` (ctest) covers the file format and the transform math the editor relies on. `cli_smoketest` (ctest) covers `thistle new --template --with` and `thistle init`, which the projects screen runs.

Windows: the owner ran it on Windows 11 with an NVIDIA RTX 4070 Super (D3D11) (`thistle editor run` on a template project) and reported it working. On the projects screen, later: New project (so finding Python and running the CLI as a child process work), the built-in browser, and the "isn't a Thistle project" warning worked. Open folder froze the window: Explorer's folder dialog hung (the cause and the fix are in [docs/platform-and-networking.md](../../docs/platform-and-networking.md)). The fix was checked with the Windows build under Wine, where the old build freezes the same way, but not yet on the owner's machine. Build & run built and started the game there, but the editor kept saying "Building" while the game ran and "The build failed" when it closed: the CLI's line saying the game had started never reached the editor (the cause and the fix are in [docs/cli.md](../../docs/cli.md), under `thistle run`). The fix was checked on Linux with Python's normal output buffering, where the old code shows the same two messages, but not yet on Windows. Not reported on Windows yet: Stop (which ends the build or the game and everything they started, through a job object). Not verified: macOS (CI compiles it there), including Finder's folder dialog. Drag and drop is tested only on X11. On macOS and Windows it goes through sokol's own drop support, which Thistle hadn't turned on before. On a Retina/HiDPI screen, mouse coordinates vs. drawing coordinates are the thing most likely to be off, and that hasn't been seen. The window is 1440×860 by default. The panels are fixed widths, so it's cramped much below about 1100 px wide.

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
