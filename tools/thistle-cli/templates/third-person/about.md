
A third-person starter: run and jump across floating islands, collect the coins, ride the jump pad up to the flag.

## Playing

Click the window to capture the mouse (Esc lets it go). The mouse turns the camera and the wheel zooms. WASD to run, Shift to walk, Space to jump, R to start over. Fall in the sea and you're back at the start.

## Changing the level

The level is `assets/scenes/level.scene.json`, made with the Thistle Editor. Open it with:

```bash
thistle editor run
```

The game reads a few things from it. Keep them, or change the code in `src/main.cpp` that looks for them:

- **`player_start`** (a spawn point): where you begin, facing the way it faces.
- **Coins**: anything with the property `coin` = `true`. They spin and bob by themselves.
- **`jump_pad`** (a trigger volume): steps into it throw you in an arc onto the entity named in its `to` property (`Pad landing`, an empty on the high island). Move that empty and the throw follows.
- **`goal`** (a trigger volume): the finish.
- **Everything else** that's a shape or a model is solid, except things with the property `solid` = `false` (the sea, the coins, the flag).

## Where to go from here

- **A real character**: `load_model("assets/hero.glb")` a rigged glTF (Mixamo exports work) and play its clips with an `Animator` instead of the box figure. See the engine's `docs/animation.md`.
- **Moving platforms, enemies**: update an entity's `transform` every frame, like the coins do. Rebuild its collider with `solid.remove()` / `solid.add()` if it has to carry the player.
- **Sounds**: `play_sound("assets/coin.wav")` when a coin is collected.
