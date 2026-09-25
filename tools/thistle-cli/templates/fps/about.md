
A first-person starter: walk a level, shoot every target, reach the exit.

## Playing

Click the window to capture the mouse (Esc lets it go). WASD to move, Shift to sprint, Space to jump, left click to shoot, R to start over.

## Changing the level

The level is `assets/scenes/level.scene.json`, made with the Thistle Editor. Open it with:

```bash
thistle editor run
```

The game reads a few things from it. Keep them, or change the code in `src/main.cpp` that looks for them:

- **`player_start`** (a spawn point): where you begin, facing the way it faces.
- **Targets**: anything with the property `target` = `true`. They're spheres here, but any shape works.
- **`exit`** (a trigger volume): where you win, once every target is down.
- **Everything else** that's a shape or a model is solid (walls, crates, the ramp), except things with the property `solid` = `false`: the targets, and the glowing door.

After saving in the editor, run the game again (`thistle run`) to play the new version.

## Where to go from here

- **Sounds:** drop a `.wav` in `assets/` and `play_sound("assets/shot.wav")` where the gun fires, or `play_sound_at()` for sounds in 3D.
- **A real gun or enemies:** import a `.glb` in the editor, or `load_model()` one, and draw it with `world.draw()`. Animated ones play with an `Animator`.
- **Things that fall over when shot:** `thistle enable physics3d`, then `Physics3D` rigid bodies. See the engine's `docs/physics3d.md`.
