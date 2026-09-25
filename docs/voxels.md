# Block worlds (`three::VoxelWorld`)

A world made of blocks on a grid, for anything from Minecraft (1 m textured blocks, endless land you dig and build in) to Teardown or MagicaVoxel (small flat-colored blocks making up props and buildings). It's optional: nothing else in the engine needs it. The rest of the 3D engine is in [3d.md](3d.md).

```cpp
VoxelWorld blocks;
const BlockId stone = blocks.add_block({.name = "stone", .color = rgb(0.55f, 0.55f, 0.6f)});
const BlockId glass = blocks.add_block({.name = "glass", .color = rgba{0.8f, 0.9f, 1.0f, 0.3f}, .alpha = AlphaMode::Blend});
blocks.fill({-10, 0, -10}, {10, 0, 10}, stone);   // a 21x1x21 floor
blocks.set(0, 1, 0, glass);

// every frame:
world.draw(blocks);
world.render(f, camera);
```

Blocks are stored in 32×32×32 chunks, which only exist where something was placed, so a world can be as big as you like (coordinates are `int`s, negatives included). Each chunk becomes an ordinary mesh, so blocks get lit, shadowed, fogged and culled like everything else.

## Block types

Block ids are 16-bit numbers: 0 is always air, and `add_block()` hands out 1, 2, 3... in order, up to 65,534 types. A `BlockType` is:

- `name`: `find_block("stone")` gives the id back (0 if there's none), so code doesn't have to hard-code ids.
- `color`: the whole look of a flat-colored block, or a tint over a textured one.
- `tile_top`, `tile_side`, `tile_bottom`: tiles from the atlas (below). -1 (the default) means untextured. `set_tiles(all)` or `set_tiles(top, side, bottom)` fills them: `BlockType{.name = "grass"}.set_tiles(0, 1, 2)`.
- `alpha`: `Opaque`; `Cutout` for leaves (see-through where the texture is); `Blend` for glass and water (`color.a` says how see-through).
- `emissive`: glows (lava, lamps). It doesn't light what's around it: add a `PointLight` for that.
- `solid`: false means characters and raycasts with `solid_only` go through it (water, tall grass).

`set_block_type(id, type)` changes an existing type, and every block of it changes.

## Textures: the atlas

```cpp
blocks.set_atlas(load_texture("assets/blocks.png"), 16);   // 16x16-pixel tiles
blocks.add_block(BlockType{.name = "log"}.set_tiles(6, 5, 6));
```

One image holds every block texture in a grid of square tiles. Tiles are numbered left to right, top to bottom, starting at 0. The filter defaults to `Nearest`, for crisp pixels. Flat-colored and textured types mix freely in one world. The voxel template draws its atlas in code (`make_texture()`), so it needs no image file.

## Placing blocks

- `set(x, y, z, id)` places one (id 0 removes it). `get(x, y, z)` reads one.
- `fill(min, max, id)` does an inclusive box. `fill_sphere(center, radius, id)` does a ball (id 0 carves a round hole).
- `clear()` removes every block, and keeps the types, the atlas and the generator.
- `each_block(fn)` visits every block that isn't air. `block_count()` and `chunk_count()` count them.

Where the grid is in the world:

- `voxel_size` is the size of a block in meters: 1 for Minecraft, around 0.1 for Teardown.
- `origin` is where block (0, 0, 0)'s corner is.
- `rotation` turns the whole grid around `origin`, for a voxel prop standing at an angle. Drawing and raycasts follow it. The built-in collision doesn't (below).

To convert: `to_block(world_point)` is the block a point is in, `block_center(b)` and `block_bounds(b)` are where a block is, `bounds()` is the world-space box around every block, and `grid_bounds()` is the same box in the grid's own units.

## Drawing

`world.draw(blocks)` re-meshes whatever chunks changed since the last draw, then draws them.

- **Faces between two blocks aren't drawn.** Neither are faces between two blocks of the same see-through type, so glass walls have no inner faces.
- **Flat-colored faces are merged** into big rectangles (greedy meshing): the top of a flat stone floor is one rectangle per chunk, not a square per block. Textured faces aren't merged, because an atlas tile can't repeat across a bigger rectangle.
- **Ambient occlusion** darkens inside corners where blocks meet. Turn it off with `ambient_occlusion = false`.
- **Re-meshing is capped** at `max_remesh_per_frame` chunks per draw (24), nearest first, so one big edit doesn't stall a frame. `remesh_all()` does it all now, for behind a loading screen.

## Pointing at blocks: break and place

```cpp
const VoxelWorld::Hit hit = blocks.raycast(Ray{camera.position, camera.forward()}, 6.0f, true);
if (hit) {
    world.wire_box(blocks.block_bounds(hit.block), black);          // outline what's aimed at
    if (f.mouse_pressed(Mouse::Left)) blocks.set(hit.block, 0);        // break
    if (f.mouse_pressed(Mouse::Right)) {
        const ivec3 place = hit.block + hit.normal;                    // the empty cell in front of the face
        if (!blocks.block_bounds(place).overlaps(player.bounds())) blocks.set(place, stone);
    }
}
```

`raycast()` walks the grid cell by cell, which is exact, and stops at the first block (or with `solid_only`, the first solid one, so a ray aims through water). `hit.normal` says which face was hit, so `hit.block + hit.normal` is where a new block goes. `overlaps_solid(box)` asks whether any solid block is inside a box.

## Walking on it

```cpp
CollisionWorld solid;
solid.add(blocks);                       // read live: a broken block stops blocking at once
CharacterController player;
// every frame:
player.update(solid, look.move_input(f), f.key_pressed(Key::Space), f.dt);
```

See [3d.md](3d.md#walking-around-collisionworld-and-charactercontroller) for the controller. Set `player.step_height = 1.05f` for Minecraft's auto-jump onto one block. The world must outlive the `CollisionWorld`. **Turned grids collide as if unturned**: the built-in collision is axis-aligned. For a turned voxel prop that things should hit, use `Physics3D` ([physics3d.md](physics3d.md)), which follows `rotation`.

## Endless worlds

```cpp
blocks.set_generator([&](VoxelWorld& w, ivec3 chunk) {
    for (int lz = 0; lz < 32; ++lz)
        for (int lx = 0; lx < 32; ++lx) {
            const int x = chunk.x * 32 + lx, z = chunk.z * 32 + lz;
            const int height = static_cast<int>(16 + 10 * fbm(x * 0.01f, z * 0.01f));
            for (int ly = 0; ly < 32; ++ly) {
                const int y = chunk.y * 32 + ly;
                if (y <= height) w.set(x, y, z, y == height ? grass : stone);
            }
        }
});
// every frame:
blocks.stream_around(player.position, 100.0f, 3);   // 100 m around the player, at most 3 new chunks a frame
```

A generator fills one chunk: its blocks run from `chunk * 32` to `chunk * 32 + 31`. `stream_around()` generates the missing chunks near a point, nearest first, at most `budget` per call. Walking into new land then costs a few chunks a frame instead of a hitch. It works in whole chunks, in a ball around the point (up and down too), and drops chunks a little past it, except ones the player changed or that came from a save, which stay loaded.

- **The generator has to be deterministic.** The same chunk must always give the same blocks, because dropped chunks are made again when you come back. Use the noise functions (`perlin`, `fbm`, `ridged`, `perlin3` for caves: see [3d.md](3d.md#terrain-and-noise)), or a hash of the coordinates, not `rand()`.
- **Write only inside the chunk you're given.** A tree that spills into the next chunk leaves blocks there that aren't counted as the player's. They're gone when that chunk is dropped and made again, and a chunk generated earlier gets them pasted on top. The voxel template keeps its trees away from chunk edges for this reason.
- **Fog hides the edge.** Set `world.fog.end` a little short of the streaming radius.

## Saving and loading

```cpp
blocks.save("world.tvx");                // everything
blocks.save("world.tvx", true);          // an endless world: only the chunks the player changed
blocks.load("world.tvx");                // false if it can't: see below
```

A save holds the block types, the blocks (each chunk run-length compressed), `voxel_size`, `origin` and `rotation`. It doesn't hold the atlas image or the generator, which are code. They're kept as they were: `load()` replaces the blocks and types, and leaves the atlas and generator alone. If a newer version of your game has block types an old save doesn't, add the missing ones after loading, by name, as the voxel template's `ensure_types()` does.

- **When `load()` fails**: a missing file leaves the world as it was, and damaged data leaves it empty, block types included. Either way it logs why. Check `std::filesystem::exists()` first if a missing file just means "new world".
- **`changed_only`** saves only the chunks changed after the generator made them (and all of a world with no generator). The generator remakes the rest. Chunks the player dug out completely are saved too, empty, so they don't grow back. Loaded chunks count as changed from then on, so the generator never overwrites them and saving again keeps them. Measured in `voxel_smoketest`: one placed block and one dug-out chunk in a generated world is 455 bytes, against 7,779 for everything.
- **`serialize()` / `deserialize()`** are the same bytes in memory, for your own save format or the network. `serialize_chunk()` / `deserialize_chunk()` do one chunk's blocks (ids only, so both worlds need the same types). That's what `VoxelSync` sends.
- **Where to put the file**: next to the game's other saves, `std::filesystem::path(save::path()).parent_path() / "world.tvx"`, like the template. See [platform-and-networking.md](platform-and-networking.md) for where that is on each platform, and set `AppConfig::save_name` so it doesn't move when the title changes.

## MagicaVoxel files

```cpp
VoxelWorld statue;
statue.voxel_size = 0.1f;
statue.load_vox("assets/knight.vox", {0, 0, 0});   // its minimum corner at block (0, 0, 0)
```

MagicaVoxel is Z-up, so models are turned to stand upright here, not mirrored. Each palette color used becomes a flat-colored block type named `vox:rrggbb`. Loading more files into the same world reuses the types. Glass materials become see-through (`Blend`, with MagicaVoxel's transparency) and emissive ones glow. Files with several models are laid out the way MagicaVoxel's scene places them, including its 90° turns. The model viewer (`thistle_model_viewer file.vox`) and the editor's Import open them too.

## Copies, pieces, exporting

- `copy()` makes a separate world with the same blocks, types, atlas and placement. It doesn't copy the generator. Worlds can't be copied with `=`, because they own GPU meshes.
- `copy_block_types(other)` takes another world's types and atlas, so ids mean the same thing in both, for pieces broken off, or a preview of what's about to be placed.
- `mesh_chunk(chunk)` gives the mesh the renderer draws for one chunk, to export or inspect.
- `chunks()` lists the chunks that hold blocks.

## With the rest of the engine

- **Levels**: a block object in a Thistle Editor level is a `VoxelWorld` (`SceneEntity::voxels`), painted in the editor's block mode. See [scene3d.md](scene3d.md). The voxel template stamps one into its generated world.
- **Physics**: `physics.add_static(blocks)` makes it solid for rigid bodies, and follows every edit. `VoxelDestruction` makes blocks break off and fall when what held them up is gone. See [physics3d.md](physics3d.md).
- **Multiplayer**: `VoxelSync` keeps one world identical on a server and every client, edits included. See [networking.md](networking.md#multiplayer-block-worlds-threevoxelsync).

## Things to know

- **Memory**: a chunk that holds any block is 64 KB (32,768 two-byte ids), plus its mesh. A streamed world of radius 100 m holds a few hundred chunks. Chunks the player changed are never dropped, so a world someone has built all over grows with what they built.
- **See-through blocks are sorted per chunk**, not per face: glass behind glass in the same chunk can blend in the wrong order. One layer of water or a glass wall looks right.
- **Textured worlds have more triangles** than flat-colored ones of the same shape, because textured faces aren't merged.
- **Turned grids** (`rotation`) collide as unturned with the built-in collision (above). They're fine to draw and raycast.

## What's been verified

- **`voxel_smoketest`** (ctest, every CI run on macOS, Windows and Linux):
  - storage across chunks and at negative coordinates
  - hidden faces across a chunk seam
  - how many faces greedy merging leaves, and the see-through rules
  - ambient occlusion, and faces pointing outward
  - `fill_sphere()` carving, and bounds
  - save/load of a multi-chunk world with negative coordinates, and truncated or wrong data rejected
  - a hand-built `.vox`: palette, glass and glow materials, a moved scene node, with the expected positions worked out by hand
  - raycasting a turned grid
  - changed-only saves (above)
- **`scene3d_smoketest`**: `copy()` makes a separate world, `set_block_type()` changes a type (and ignores air and unknown ids), `grid_bounds()` follows edits.
- **`terrain_smoketest`**: streaming generates nearest first and keeps to its budget, never generates a chunk twice, drops far chunks, keeps edited ones, makes dropped ones again, and never overwrites a loaded save.
- **`character_smoketest`**: walking, stepping and jumping on blocks. **`voxelnet_smoketest`** and **`destruction_smoketest`**: see their docs.
- **Seen rendered (Xvfb + Mesa llvmpipe, a software renderer, not a GPU):**
  - `voxel_demo`: textured terrain, cutout leaves, water, a glass hut, a flat-colored statue, ambient occlusion and shadows. Played: landed, dug a hole, placed blocks.
  - `endless_demo`: flown forward, going from 62 to 90 chunks as new land streamed in.
  - Four of MagicaVoxel's own sample files, upright and not mirrored.
  - The voxel template: a new world with the house, then a placed block surviving a real quit and relaunch.
- **Not yet seen on Metal or D3D11** (macOS, iOS, Windows), or on any real GPU. See [3d.md](3d.md#whats-been-verified).
