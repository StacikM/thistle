
A block-world starter: an endless world of textured blocks, generated as you walk. Break, place and build. What you change is saved between runs, and a day passes every 10 minutes.

## Playing

Click the window to capture the mouse (Esc lets it go).

| Key | Does |
|---|---|
| WASD | Move |
| Space | Jump |
| Left click | Break the block you're looking at |
| Right click | Place a block |
| 1–9 or the wheel | Pick which block to place |
| F | Fly (Space and Shift for up and down) |
| F3 | Show your position |
| N | Start a new world |

## How it's put together

- **The world** is a `VoxelWorld` with a generator, all in `src/main.cpp`:
  - **Terrain:** `fbm` noise hills, sand at the shoreline, water below sea level, trees.
  - **Streaming:** `stream_around()` generates chunks as you walk.
  - **Textures:** a 16 px atlas drawn in code (`make_atlas()`). Replace it with an image, `load_texture("assets/blocks.png")`, laid out the same way.
- **Saving**: only the chunks you changed go to `world.tvx` in the game's save folder (`save_world()`); the generator remakes the rest. Your position, facing and the time of day go in the game's save data. It saves every minute and when the window closes.
- **The spawn house** is a block object made in the Thistle Editor: `assets/scenes/spawn.scene.json`. A new world stamps it in at the origin, matching its block types to the world's by name, with the player starting at its `player_start`. Open it with `thistle editor run`, build something else (keep the block type names: stone, planks, log, glass, glow...), and press N in the game for a new world with it.

## Where to go from here

- **More block types:** add them to `kinds` (and a tile to the atlas). Saves from before still load, since new types are added to them by name.
- **Caves:** in the generator, carve where `perlin3` noise is near zero (the engine's `examples/endless_demo.cpp` does).
- **Multiplayer:** `VoxelSync` keeps a world identical between a server and its players. See the engine's `docs/networking.md` and `examples/voxel_mp_demo.cpp`.
- **Explosions:** `VoxelDestruction`, with `thistle enable physics3d`. See `docs/physics3d.md`.
