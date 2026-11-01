# Coming from GDevelop

GDevelop is events and objects you click together; Thistle is C++ code you write. The ideas carry over. This is the translation table, then what's missing.

| In GDevelop | In Thistle |
|---|---|
| **Scene** | `app.scene("name", {.enter, .update, .exit})` and `set_scene("name")` ([14](examples/guide/14_scenes.cpp)). Or one `app.update` lambda with an `enum Screen` |
| **Events** (conditions → actions, every frame) | the body of `app.update([&](Frame f) { ... })`: it runs once per frame, top to bottom |
| **Condition: key is pressed / is down** | `f.key_pressed(Key::Space)` (once) / `f.key_down(Key::Space)` (held) |
| **Condition: mouse button / touch** | `f.mouse_pressed(Mouse::Left)`, `f.mouse_down(...)`, `f.mouse()`; a tap is a click |
| **Trigger once** | `*_pressed` is already once. For your own: set a `bool` the first time, clear it when the condition ends |
| **Every X seconds** | `timer -= f.dt; if (timer <= 0) { timer += X; ... }` ([timers](examples/cookbook/timers_and_sequences.cpp)) |
| **Wait X seconds** (then do things) | a `Node` action queue: `.delay(1.0f).call([&]{ ... })`, or a countdown variable |
| **Object (sprite) + instances** | a `struct` and a `std::vector` of them ([06](examples/guide/06_entities.cpp)) |
| **Create object / Delete object** | `v.push_back(...)` / mark `dead = true`, then `erase(remove_if(...))` |
| **For each object** | `for (Enemy& e : enemies) { ... }` |
| **Object variable** | a field in the struct |
| **Global / scene variable** | a variable declared in `main` that the lambda captures with `[&]` |
| **Condition: collision** | `a.overlaps(b)` for rects ([08](examples/guide/08_collision.cpp)), `distance_to` for circles, or Box2D's `on_collision` |
| **Pick nearest object** | a loop that tracks the smallest `distance_to` |
| **Move toward position / Move at angle** | `pos += (target - pos).normalized() * speed * f.dt`; `pos += vec2{cos(a), sin(a)} * speed * f.dt` |
| **Random** | `<random>`: `std::mt19937` and `std::uniform_real_distribution` (examples 6 and 14) |
| **Sprite animation** | `Anim(sheet, w, h).frames(first, count).fps(12)` and `f.sprite(sheet, pos, {.src = anim.frame_at(f.time)})` ([07](examples/guide/07_sprites.cpp)) |
| **Flip / angle / opacity / scale** | `SpriteOpts` `.flip_x`, `.rotation`, `.tint` (alpha), `.size` |
| **Layers** | draw order: later calls draw on top. `f.camera(...)` for the world, `f.camera({0, 0})` for the HUD |
| **Camera (follow, zoom, shake)** | [12](examples/guide/12_camera.cpp): your own `cam` position and `push_transform` |
| **Text object** | `f.text("...", pos, {.size = 32})`; `Anchor::...` to pin to a corner or the center |
| **Button / UI behaviors** | `f.button`, `f.checkbox`, `f.slider`, `f.progress_bar`, `Menu` ([13](examples/guide/13_ui.cpp)) |
| **Text input** | `begin_text_input()` / `text_input()` / `end_text_input()` |
| **Platformer behavior** | build it: tiles ([09](examples/guide/09_tiles.cpp)) or Box2D ([11](examples/guide/11_physics_platformer.cpp)) |
| **Top-down movement behavior** | [04](examples/guide/04_move_player.cpp), [topdown_shooter](examples/cookbook/topdown_shooter.cpp) |
| **Physics behavior** | `Physics` (Box2D): [10](examples/guide/10_physics.cpp) |
| **Tween behavior** | `Tween` and `ease()`, or `Node::move_to(...)` ([17](examples/guide/17_juice.cpp)) |
| **Health extension** | a struct with `hp` and an invincibility timer ([health_and_damage](examples/cookbook/health_and_damage.cpp)) |
| **Tiled map** | `Tilemap::load_tiled_json("assets/map.tmj")` (Tiled's own format) |
| **Particles** | `Particles fx; fx.emit(pos, 30, {...})` |
| **Sound / Music action** | `play_sound("assets/x.wav")`, `play_music(...)`, `set_*_volume(...)` ([15](examples/guide/15_audio.cpp)) |
| **Storage (save/load)** | `save::set_int / get_int / set / get` ([16](examples/guide/16_save.cpp)) |
| **Fade / transitions** | a black `f.rect` with an alpha you animate ([14](examples/guide/14_scenes.cpp)) |
| **Export to Android / iOS / desktop** | the same C++ builds for each; see [building.md](building.md), [android.md](android.md), [cli.md](cli.md) |

## What you won't find

- **No visual editor for 2D.** Levels are text files, JSON or [Tiled](https://www.mapeditor.org/) maps you load (the Thistle Editor is for 3D scenes only). The upside is that placing things is data, not typed coordinates: see [18_levels](examples/guide/18_levels.cpp) and `f.anchored()` for UI.
- **No automatic behaviors.** Nothing moves, collides or animates unless your code does it. That's why platformer and top-down movement are examples to copy, not checkboxes.
- **No object picker for "all Enemy objects".** You keep your own vectors. In exchange, a loop over them is just a loop.
- **No multi-touch.** One finger.
- **No hot reload of code.** Edit, rebuild (`thistle run` rebuilds only what changed), run. Level and text files you load at startup can be reloaded without restarting (press R in example 18).
- **C++ errors are not GDevelop errors.** A typo is a compiler error with a line number; a crash is usually something removed while you still pointed at it. Read the first `error:` line and fix that one; the ones after it are usually knock-on effects.
