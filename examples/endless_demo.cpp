// Visual check for VoxelWorld streaming: an endless Minecraft-style world —
// fbm hills, perlin3 caves, sand beaches, water, trees — generated chunk by
// chunk around a flying camera (click to capture the mouse, WASD/E/Q, Shift
// = fast). Chunk counts go to the log. Opens a window and never quits on
// its own, so CI builds it but doesn't run it.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <string>
using namespace thistle;
using namespace thistle::three;

int main() {
    App app{{.title = "thistle endless demo", .width = 1280, .height = 720}};

    VoxelWorld voxels;
    const BlockId grass = voxels.add_block({.name = "grass", .color = rgb(0.38f, 0.62f, 0.3f)});
    const BlockId dirt = voxels.add_block({.name = "dirt", .color = rgb(0.5f, 0.36f, 0.24f)});
    const BlockId stone = voxels.add_block({.name = "stone", .color = rgb(0.52f, 0.52f, 0.55f)});
    const BlockId sand = voxels.add_block({.name = "sand", .color = rgb(0.86f, 0.8f, 0.58f)});
    const BlockId water = voxels.add_block({.name = "water", .color = rgba{0.25f, 0.5f, 0.85f, 0.65f}, .alpha = AlphaMode::Blend, .solid = false});
    const BlockId wood = voxels.add_block({.name = "wood", .color = rgb(0.45f, 0.32f, 0.2f)});
    const BlockId leaves = voxels.add_block({.name = "leaves", .color = rgb(0.28f, 0.52f, 0.25f)});
    constexpr int sea = 12;

    voxels.set_generator([=](VoxelWorld& w, ivec3 c) {
        if (c.y < -1 || c.y > 1) return; // the world lives between y = -32 and y = 63
        for (int lz = 0; lz < 32; ++lz) {
            for (int lx = 0; lx < 32; ++lx) {
                const int x = c.x * 32 + lx, z = c.z * 32 + lz;
                const int h = static_cast<int>(16.0f + 14.0f * fbm(x * 0.012f, z * 0.012f, 5, 1));
                for (int ly = 0; ly < 32; ++ly) {
                    const int y = c.y * 32 + ly;
                    BlockId id = 0;
                    if (y <= h) id = y == h ? (h <= sea + 1 ? sand : grass) : (y > h - 4 ? dirt : stone);
                    else if (y <= sea) id = water;
                    // Worm-ish caves: where 3D noise is near zero, below the surface.
                    if (id != 0 && id != water && y < h - 2 && std::fabs(perlin3(x * 0.05f, y * 0.08f, z * 0.05f, 7)) < 0.09f) id = 0;
                    if (id) w.set(x, y, z, id);
                }
                // A tree now and then, fully inside this chunk so chunks never write into each other.
                const bool tree_spot = (static_cast<uint32_t>(x * 73856093 ^ z * 19349663) % 97) == 0;
                if (tree_spot && h > sea + 1 && lx > 2 && lx < 29 && lz > 2 && lz < 29 && c.y == (h + 6) / 32) {
                    for (int i = 1; i <= 4; ++i) w.set(x, h + i, z, wood);
                    for (int dz = -2; dz <= 2; ++dz)
                        for (int dy = 3; dy <= 6; ++dy)
                            for (int dx = -2; dx <= 2; ++dx)
                                if (std::abs(dx) + std::abs(dz) + (dy == 6 ? 2 : 0) <= 3 && w.get(x + dx, h + dy, z + dz) == 0)
                                    w.set(x + dx, h + dy, z + dz, leaves);
                }
            }
        }
    });

    World world;
    world.sun.direction = {-0.5f, -0.75f, -0.3f};
    world.sun.shadow_distance = 70.0f;
    world.fog.enabled = true;
    // Fog fully hides the world a little before the streaming radius ends,
    // so the edge of the loaded chunks is never visible.
    world.fog.start = 55.0f;
    world.fog.end = 100.0f;
    Camera camera;
    camera.position = {0.0f, 40.0f, 0.0f};
    FlyCamera fly;
    fly.speed = 16.0f;
    fly.look.pitch = radians(-20.0f);

    app.update([&](Frame f) {
        fly.update(camera, f);
        voxels.stream_around(camera.position, 128.0f, 3);
        world.draw(voxels);
        world.render(f, camera);
        static int frames = 0;
        if (++frames % 60 == 0) log_info("endless demo: chunks loaded=" + std::to_string(voxels.chunk_count()));
    });
    return app.run();
}
