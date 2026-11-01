# Learn by example

Nineteen small programs, in the order you'd want to learn things, then ten recipes for common game pieces. Every one is a complete file you can build, run and edit. They're in [examples/guide/](examples/guide/) and [examples/cookbook/](examples/cookbook/).

## Run them

```bash
cmake -S . -B build -DTHISTLE_BUILD_DOC_EXAMPLES=ON
cmake --build build --target doc_examples
cd build && ./doc_guide_01_hello        # or any other doc_guide_* / doc_cookbook_* program
```

Run them from the `build` folder: they load `assets/font.ttf`, which the build copies there. To use one in your own game, copy the `.cpp` into your project's `src/main.cpp`, and copy `examples/assets/font.ttf` (and any sound it plays) into your project's `assets/`.

## The entry point

Every example starts with `THISTLE_MAIN { ... THISTLE_RUN(app); }` instead of `int main() { ... return app.run(); }`. On every platform except Android they are exactly the same thing. On Android there is no `main()`, so the macros are what make the same file build there too. Write them from the start and you never have to touch it again.

## The guide

| # | Program | Shows |
|---|---|---|
| 1 | [01_hello](examples/guide/01_hello.cpp) | a window, shapes, text, loading a font |
| 2 | [02_coordinates](examples/guide/02_coordinates.cpp) | the pixel grid (y points down), draw order, mouse position, anchored text |
| 3 | [03_delta_time](examples/guide/03_delta_time.cpp) | `f.dt`: why `x += 3` is wrong and `x += 200 * f.dt` is right |
| 4 | [04_move_player](examples/guide/04_move_player.cpp) | keyboard, gamepad and touch movement, normalized diagonals |
| 5 | [05_input](examples/guide/05_input.cpp) | `pressed` vs `down`, mouse buttons, scroll, one "jump" action from many devices |
| 6 | [06_entities](examples/guide/06_entities.cpp) | a `std::vector` of things: spawn on a timer, update, remove the dead |
| 7 | [07_sprites](examples/guide/07_sprites.cpp) | textures, sprite sheets and `Anim`, flip/tint/rotate, 9-slice, additive blend |
| 8 | [08_collision](examples/guide/08_collision.cpp) | rect, circle and point tests, and pushing a rect out of a wall |
| 9 | [09_tiles](examples/guide/09_tiles.cpp) | a tile map and a platformer player with no physics engine |
| 10 | [10_physics](examples/guide/10_physics.cpp) | Box2D: dynamic bodies, sensors, applying impulses, removing bodies |
| 11 | [11_physics_platformer](examples/guide/11_physics_platformer.cpp) | a platformer on Box2D: ground checks, coyote time, jump buffering, a moving platform, coins |
| 12 | [12_camera](examples/guide/12_camera.cpp) | following a player, zoom, screen shake, parallax, mouse position in the world |
| 13 | [13_ui](examples/guide/13_ui.cpp) | `Menu`, buttons, checkbox, slider, a text field, a HUD pinned with anchors |
| 14 | [14_scenes](examples/guide/14_scenes.cpp) | `App::scene`, a fade between screens, pause, game over, a saved best score |
| 15 | [15_audio](examples/guide/15_audio.cpp) | sound effects, music, three volumes |
| 16 | [16_save](examples/guide/16_save.cpp) | `save::` for scores and settings, and `save_name` |
| 17 | [17_juice](examples/guide/17_juice.cpp) | particles, shake, flash, hit-stop, easing curves |
| 18 | [18_levels](examples/guide/18_levels.cpp) | levels from a text map and a JSON file, loaded with `read_asset_text()` |
| 19 | [19_debug](examples/guide/19_debug.cpp) | an on/off debug overlay, hitboxes, slow motion, logging |

## The recipes

| Program | What it is |
|---|---|
| [topdown_shooter](examples/cookbook/topdown_shooter.cpp) | WASD + mouse aim, hold to fire, enemies that chase |
| [asteroids_flight](examples/cookbook/asteroids_flight.cpp) | rotate and thrust with momentum, screen wrap, rocks that split |
| [health_and_damage](examples/cookbook/health_and_damage.cpp) | health, invincibility blink, knockback, a bar with a damage trail |
| [timers_and_sequences](examples/cookbook/timers_and_sequences.cpp) | "after / every N seconds", and do-this-wait-do-that with `Node` actions |
| [state_machine](examples/cookbook/state_machine.cpp) | enemy AI: patrol, chase, attack |
| [wave_spawner](examples/cookbook/wave_spawner.cpp) | enemy waves from a data table |
| [dialogue_box](examples/cookbook/dialogue_box.cpp) | typewriter text, word wrap, choices |
| [inventory_drag_drop](examples/cookbook/inventory_drag_drop.cpp) | a slot grid with drag and drop and tooltips |
| [grid_puzzle](examples/cookbook/grid_puzzle.cpp) | Sokoban with undo |
| [touch_controls](examples/cookbook/touch_controls.cpp) | one-finger controls that also work with a mouse |

## What was checked

All 29 programs build, and each one was started and stayed up for a few seconds on macOS. Four of them (the sprite sheet, the UI menu, the dialogue box, the shooter) were also looked at in a screenshot of their own window, which is how an overlapping layout in the sprite example was found and fixed. The others weren't looked at on screen, so a visual quirk in one is possible.

The logic that's easy to get subtly wrong has real assertions in [examples/tests/](examples/tests/) (run with `ctest` in a `-DTHISTLE_BUILD_DOC_EXAMPLES=ON` build):

- the tile collision in #9, including a check that a fast fall skips through a floor without a speed cap;
- the circle-vs-rect and push-out maths in #8;
- the whole platformer in #11, driven by scripted input: standing, running, jumping, coyote time, jump buffering, riding the lift, spikes, coins, falling into the pit, and a bot that crosses the pit by riding the lift (so the level is known to be beatable);
- the Sokoban rules (pushing, blocking, crates on goals, winning, undo).

Nothing here has been run on a phone, and nobody has played them for fun: they were checked to be correct, not to be good games.

## Things these examples run into (read this if something looks wrong)

- **No font, no text.** There's no built-in font. A missing `assets/font.ttf` now logs a warning, but text still just doesn't draw.
- **Clamp `f.dt`.** A window drag or a breakpoint gives a giant frame time. The examples use `std::min(f.dt, 1.0f / 30.0f)` for anything that moves.
- **Don't change a `std::vector` while looping over it.** Mark things dead, then remove them after the loop (#6).
- **Physics callbacks run inside `step()`.** Note what happened there, and change the world (remove a body) after the step (#10, #11).
- **One finger only.** Touch is the primary pointer; there's no multi-touch ([touch_controls](examples/cookbook/touch_controls.cpp)).
- **`[&]` captures are only safe while the thing they point at is alive.** The examples keep their state in `THISTLE_MAIN`, which stays alive for the whole run, including on Android.
