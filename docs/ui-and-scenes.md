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

`ButtonStyle` controls colors/text size per call — there's no global theme object, so if you want a consistent look, build your own `ButtonStyle` presets in game code and reuse them. The engine won't do this for you and shouldn't; it doesn't know your game's palette.

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
