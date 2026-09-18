# Thistle Editor

A minimal prop-placement tool for Thistle's minor-3D drawing mode: browse `.obj`
files, click one to drop it into the scene, nudge it around with the keyboard,
save/load the layout as a `Node` tree.

**This is not a level compiler.** There's no BSP, no lighting bake, none of what
actually makes something like Hammer valuable for a real FPS level. It's the
thinnest real GUI on top of `save_scene()`/`load_scene()` — see
[docs/ui-and-scenes.md](../../docs/ui-and-scenes.md)'s "Placing minor-3D props
on a `Node`" section for exactly what that captures (and doesn't). Use this for
laying out props in a small 3D scene, not for building a shippable level.

## Building

```bash
cd tools/thistle-editor
cmake -S . -B build
cmake --build build
```

It's a real Thistle project — it links the engine the same way a scaffolded
game would (see the comment in `CMakeLists.txt`), just pointed at this repo's
own engine two directories up instead of a copy.

## Running

The editor scans `./assets` (relative to wherever you run it from) for `.obj`
files, recursively. For each one it resolves a texture by convention, since
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

## Keybindings

| Key | Action |
|---|---|
| Click a palette entry | Spawn that prop into the scene, select it |
| Left / Right | Orbit camera |
| Up / Down | Zoom camera |
| W / A / S / D | Move selected prop on the ground plane |
| R / F | Move selected prop up / down |
| Q / E | Rotate selected prop (Y axis) |
| Z / X | Scale selected prop down / up |
| Tab | Select next prop |
| Escape | Deselect |
| Backspace | Delete selected prop |
| Ctrl+S | Save the scene |
| Ctrl+O | Load the scene |

Save/load always uses one fixed file next to your save data
(`thistle_editor_scene.json`, in the same directory `save::path()` returns) —
there's no file-picker dialog, so it's one layout at a time. If you want more
than that, copy the JSON out between sessions.
