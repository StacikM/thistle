# UI, scenes, and the scene graph

## Immediate-mode widgets

```cpp
if (f.button("Play", {{100, 100}, {200, 60}})) start_game();

static bool music_on = true;
f.checkbox("music", music_on, {{100, 180}, {24, 24}});

value = f.slider(value, 0.0f, 1.0f, {{100, 220}, {200, 20}});
f.progress_bar(loading_t, {{100, 260}, {200, 16}}, coral);
```

`button()` draws itself and returns `true` on the exact frame it's clicked (or tapped — a tap delivers as a left-mouse click, you don't need separate touch handling for buttons). There's no retained widget tree, no IDs, no diffing. You call it every frame with the same rectangle and it just works, because it *is* the drawing — there's nothing to keep in sync because nothing is cached.

The tradeoff, and it's a real one: **you lay out every rectangle yourself, every frame.** There's no flow layout, no anchors, no percentage sizing beyond what you compute from `f.width`/`f.height` yourself. Fling's entire UI is hand-computed percentages of screen height (`H * 0.14f` for a button width, etc.) specifically so it holds up across a 1280×720 desktop window and a phone's actual framebuffer size. That's not a shortcut, that's the actual technique — learn it, don't fight it looking for a layout system that isn't coming.

`ButtonStyle` controls colors/text size/font per call — there's no global theme object, so if you want a consistent look, build your own `ButtonStyle` presets in game code and reuse them. The engine won't do this for you and shouldn't; it doesn't know your game's palette.

### Making a button that doesn't look like every other engine's default button

`f.button()` draws a flat rect + centered text — fine for a prototype, not what you want for a real UI. Two primitives get you the rest of the way to a genuinely custom look, no shader/asset-pipeline work required:

```cpp
// A filled rounded rectangle — pure geometry, works with any color/gradient
// you can express as a fill (draw two, offset, for a border effect).
f.rounded_rect({100, 100}, {200, 60}, 12, rgb(0.2f, 0.6f, 0.9f));

// 9-slice: your own button-skin art, drawn once at any convenient size in an
// image editor, scaled cleanly to any target size. The 32px border stays at
// native scale (no blurry stretched corners); only the edges/center stretch.
Texture skin = load_texture("assets/button_skin.png");
f.sprite9(skin, {100, 200}, {200, 60}, 32);
```

Neither of these knows anything about clicking — that's deliberate, and it's not a gap, because the interaction logic `f.button()` uses internally is three lines you already have full access to:

```cpp
const bool hover = area.contains(f.mouse());
const bool held = hover && f.mouse_down(Mouse::Left);
const bool clicked = hover && f.mouse_pressed(Mouse::Left);
```

Draw whatever you want above (a `sprite9` skin, a `rounded_rect`, an icon `sprite`, custom-font `text` via `TextOpts::font`), check `clicked` for the action, and you have a fully custom button — same amount of code as before, just not locked into `f.button()`'s specific look. This is the same relationship `Menu` has to raw `f.button()` calls: a convenience for the common shape, not a wall around the engine's actual drawing primitives.

## `Menu`: for when you actually have a vertical stack

```cpp
Menu m(f, f.time - menu_open_time, MenuAnim::FromTop);
m.title("My Game");
m.label("v1.0");
if (m.button("Play")) set_scene("play");
if (m.button("Quit")) request_quit();
```

Handles vertical centering, entrance animation, and gamepad focus/navigation between items for you, for the specific shape "a centered vertical list of buttons with a title." If your menu isn't that shape — Fling's isn't, it's a swipeable card carousel — don't force it through `Menu`, just draw with raw `f.button()` calls like the rest of the game does. `Menu` is a convenience for the common case, not a mandatory abstraction layer.

## Particles

```cpp
Particles fx;   // you own this — put it wherever your game state lives

fx.emit(pos, 20, { .spread = 200, .life = 0.4f, .color = coral });
fx.update(dt);   // every frame
fx.draw(f);       // every frame, wherever in your draw order they should appear
```

A flat pool, gravity built in, nothing fancier. No emitters-as-objects, no curves, no sub-emitters. If you need real VFX authoring, this isn't it — it's "throw some squares that fall and fade," which covers hit-flashes, dust, sparkles, and most of what a small game actually needs.

## `Node`: a scene graph, if you want one

```cpp
Node root;
Node* enemy = root.add_child();
enemy->pos = {200, 100};
enemy->sprite = enemy_tex;
enemy->move_to({400, 100}, 1.5f, Ease::OutCubic).delay(0.5f).call([]{ /* ... */ });

// every frame:
root.update(f.dt);
root.draw(f);
```

Parent/child transforms (position, rotation, scale, alpha all inherit down the tree), and a chainable action queue (`move_to`/`move_by`/`scale_to`/`rotate_to`/`fade_to`/`delay`/`call`) that runs in order per node. This is the closest thing here to a "real" scene structure.

Use it if your game actually has a hierarchy worth modeling — a formation of enemies that move together, a UI element with child decorations that should inherit its fade. Don't reach for it by default. A single `vec2 pos` and a hand-written tween is simpler and faster for the common case of "one thing moves from A to B," and every line of `Node` machinery you don't need is a line you have to read later when something's wrong. Fling doesn't use `Node` anywhere — it does everything with plain structs and direct math, because a physics-driven slingshot game has no hierarchy to speak of. Let the shape of your game decide, not a habit from a bigger engine.

### Saving and loading a `Node` tree

```cpp
save_scene(root, save::path() + "/checkpoint.json");   // whatever pos/rotation/alpha/etc. actually are right now
std::unique_ptr<Node> loaded = load_scene(save::path() + "/checkpoint.json");
```

This is a checkpoint/save-game mechanism, not a level-authoring format — nobody is meant to open the JSON and hand-edit it to change a level. It captures live values at the instant you call `save_scene()` (wherever physics/gameplay actually moved things to), not however the tree looked when it was first built. `sprite` (a `Texture` — just a runtime id, meaningless after a restart) doesn't survive the round trip on its own; set `sprite_path` alongside it and `load_scene()` calls `load_texture()` for you, once per unique path even if many nodes share one.

Queued actions (`move_to`, `delay`, `call`, ...) are **not** saved — only the static pose. A `call()` action holds a `std::function`, which fundamentally can't be written to JSON, and "this node is 60% through a move_to" isn't something a level snapshot needs anyway. If you save mid-animation, the loaded node just has wherever that animation had gotten to as its resting pose — the motion itself doesn't resume.

### Placing minor-3D props on a `Node`

```cpp
Node* prop = root.add_child();
prop->mesh_path = "assets/chest.obj";           // load_scene() calls load_mesh() for you
prop->mesh_texture_path = "assets/atlas.png";   // optional — omit for a flat mesh_tint
prop->mesh_pos = {2, 0, -3};
prop->mesh_rotation = {0, 1.2f, 0};             // Euler radians, same order as Frame::mesh3d
prop->mesh_scale = {1, 1, 1};

// every frame, in your own 3D pass:
f.camera3d(cam);
root.draw_meshes(f);   // walks the subtree, calling f.mesh3d() for every node with a mesh
f.camera({0, 0});
```

This is deliberately a second, separate draw pass from `draw()` — `draw()` composes each node's `pos`/`rotation`/`scale` as a 2D affine transform for `sprite`, and there's no sane way to reuse that same 2D transform to place a 3D mesh (a 2D rotation isn't a 3D rotation, a 2D position doesn't have a Z). So `mesh_pos`/`mesh_rotation`/`mesh_scale` are their own independent `vec3` fields, untouched by the node's 2D `pos`/`rotation`/`scale`, and `draw_meshes()` is a call you make yourself, after `f.camera3d(...)`, exactly like any other minor-3D drawing. A node can carry both a `sprite` and a `mesh` if you genuinely want that (they're drawn in separate passes either way), but the common case is one or the other.

**`mesh_pos`/`mesh_rotation`/`mesh_scale` are relative to the parent's own composed mesh transform, not world space** — real hierarchy, the same way `pos`/`rotation`/`scale` compose down the tree in `draw()`. Move/rotate/scale a parent node and everything nested under it follows, even a parent with no mesh of its own (a pure grouping/anchor node):

```cpp
Node* group = root.add_child();
group->mesh_pos = {5, 0, 0};       // the group's own position

Node* wheel = group->add_child();
wheel->mesh_path = "assets/wheel.obj";
wheel->mesh_pos = {0.6f, 0, 0};    // 0.6 units from the GROUP's position, not world origin
```

Rotation composes by simple per-axis addition, not a real rotation-matrix multiply — exact when everything shares one rotation axis (the common case: `mesh_rotation.y` for a turntable spin), an approximation once nested nodes rotate around different axes at once. Fine for grouping a handful of props; not a general character-rig kind of hierarchy.

Same `save_scene()`/`load_scene()` round trip as sprites: `mesh`/`mesh_texture` are runtime ids that don't survive a restart, so `mesh_path`/`mesh_texture_path` are what actually get saved, and `load_scene()` resolves them back through `load_mesh()`/`load_texture()` for you (once per unique path). A `mesh` built from `make_cube_mesh()`/`make_sphere_mesh()`/etc. instead of a file round-trips the same way — set `mesh_prim` (a `Prim` enum) alongside `mesh` instead of `mesh_path`, and `load_scene()` calls the matching `make_*_mesh()` for you.

`Prim::Trigger` is a different kind of `mesh_prim` value: it never gets a real `mesh` at all, and `draw_meshes()` never draws it — `mesh_pos`/`mesh_scale` describe an invisible volume's center/half-extent, purely as data. It's exactly what a brush in a level editor like Hammer actually is on its own: geometry and a name (`Node::name`, a plain `std::string` with no behavior attached, there purely for your own identification — an editor's Outliner, a lookup in your own code, a trigger's label). Thistle does now have basic 3D overlap tests (`Box3D`/`Sphere3D`, `box3d_sphere3d_overlap()`, etc. — see docs/drawing.md's "3D collision" section) so a trigger can actually be checked against, but nothing calls them for you automatically — there's still no callback, no "on enter" event, nothing that fires on its own. You build a `Box3D` from `node.world_mesh_transform()` and call the overlap test yourself, every frame, exactly the way Source (not Hammer) is what actually processes a trigger brush at runtime, not Hammer itself. `Node::detach_child()` exists alongside `remove_child()` for exactly this kind of tooling: it hands a child back instead of destroying it, so you can re-parent it onto a different node (`new_parent->add_child(old_parent->detach_child(x))`) rather than only ever being able to delete it.

This is the actual prerequisite for anything like a level-placement tool — a way to describe "these props, at these positions, in this hierarchy, with these names/trigger volumes" that survives a save/load round trip — not a level-authoring format on its own; nothing here helps you *build* a layout, only persist one. (`tools/thistle-editor` used to be the GUI for this. It now edits `three::Scene3D` levels for the 3D renderer instead, see [scene3d.md](scene3d.md). It can import a layout its old version saved, but it doesn't edit `Node` trees any more.)

## `App::scene` — named states, if a giant lambda stops being enough

```cpp
app.scene("menu", {
    .enter = [&]{ selected = 0; },
    .update = [&](Frame f) { /* draw menu, maybe set_scene("play") */ },
});
app.scene("play", {
    .enter = [&]{ world.load(level); },
    .update = [&](Frame f) { /* run gameplay */ },
    .exit = [&]{ world.clear(); },
});
```

The first scene registered is the initial one. `set_scene(name)` switches at the start of the next frame — not immediately, so don't call it and then expect the new scene's state to exist later in the *same* frame. `exit` runs on the old scene, then `enter` on the new one, then that scene's `update` starts getting called from the next frame on.

This is entirely optional. Fling doesn't use it — it has one `app.update` lambda with a `Screen` enum and a giant `if (screen == Screen::X) { ... return; }` chain, because that gave it one thing `App::scene` doesn't: trivially sharing local state (input edge-detection flags, the physics world, animation timers) across screens without threading it through every scene's captures. Pick whichever shape matches how much your "screens" actually need to share. Neither is more correct than the other; the giant-lambda-with-an-enum pattern is real and used in the actual shipped game, don't let anyone tell you it's not idiomatic just because it's not `App::scene`.

## `Tilemap`

```cpp
Tilemap map;
map.load_tiled_json("assets/level1.tmj");   // or load_csv() for a bare CSV export
map.draw(f, camOffset);
int tile = map.at(col, row);
```

Loads [Tiled](https://www.mapeditor.org/) exports — full `.tmj`/`.json` (image path resolved relative to the JSON, all tile layers loaded back-to-front) or a bare single-layer CSV if that's all you exported. If you're not building a tile-grid game, ignore this entirely — it's not a general "level format," it's specifically Tiled's format, because reinventing a worse version of Tiled's format is a waste of everyone's time.

## Debug UI (optional: Dear ImGui)

Windows for tweaking and inspecting a game while you make it: sliders for values you're tuning, checkboxes for cheats, stats, lists of what's alive. It's [Dear ImGui](https://github.com/ocornut/imgui) (v1.92.9b, via sokol_imgui), opt-in like 3D physics:

```bash
thistle enable debug_ui     # or -DTHISTLE_DEBUG_UI=ON
```

Then call ImGui directly, anywhere in your update callback:

```cpp
#if THISTLE_DEBUG_UI
#include <imgui.h>
#endif

app.update([&](Frame f) {
#if THISTLE_DEBUG_UI
    debug_stats_window();                       // fps + frame-time graph, 3D draw calls/triangles, sounds
    ImGui::Begin("Tuning");
    ImGui::SliderFloat("jump speed", &player.jump_speed, 2.0f, 20.0f);
    ImGui::End();
#endif
    // ... the game
});
```

- It's drawn last, **on top of everything**, post-effects included.
- **Clicks, typing and scrolling that land on a debug window don't reach the game**, so clicking a checkbox doesn't also fire your gun. Releases always get through, so no key gets stuck down. `debug_ui_wants_mouse()` / `debug_ui_wants_keyboard()` are there if you need to know yourself.
- `#if THISTLE_DEBUG_UI` keeps the game building with the module off, where `<imgui.h>` isn't on the include path. `ImGui::ShowDemoWindow()` is compiled in and is the best tour of what ImGui can do.
- It's a debug tool, and it looks like one on purpose. Build the game's own menus with the widgets above.

Verified: the physics demo's stats and tuning windows were used under Xvfb + llvmpipe. Toggling a checkbox and dragging a slider changed the game while the click didn't reach it (the fly camera didn't grab the mouse). The same build cross-compiles for Windows with MinGW. The owner's MSVC build of the demos on Windows 11 with an NVIDIA RTX 4070 Super (D3D11) had the debug UI on, and they reported the demos working. CI builds it on macOS, Windows and Linux with the modules on.
