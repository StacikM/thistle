// Visual check for three::VoxelWorld, and a starting point for a block
// game: rolling terrain from a texture atlas generated in code (grass, dirt,
// stone, logs, cutout leaves, glass, water, a glowing block), next to a
// flat-colored MagicaVoxel-style statue — both block styles in one world,
// with ambient occlusion and sun shadows. And it's playable: click to capture
// the mouse, WASD + Space to walk and jump (a CharacterController against the
// voxels), left click breaks the block under the crosshair, right click
// places one, 1-9 or the scroll wheel picks which, Tab toggles flying.
// Needs no asset files. Opens a window and never quits on its own, so CI
// builds it but doesn't run it.
#include <thistle.hpp>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
using namespace thistle;
using namespace thistle::three;

namespace {

constexpr int kTile = 16;
constexpr int kCols = 4;

uint32_t hash(int x, int y, int salt) {
    uint32_t h = static_cast<uint32_t>(x * 374761393 + y * 668265263 + salt * 2147483647);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// A 4x4 grid of 16px tiles, each a little procedural pixel-art texture.
Texture make_atlas() {
    const int size = kTile * kCols;
    std::vector<unsigned char> px(size * size * 4, 0);
    auto put = [&](int tile, int x, int y, int r, int g, int b, int a = 255) {
        const int ox = (tile % kCols) * kTile + x, oy = (tile / kCols) * kTile + y;
        unsigned char* p = &px[(oy * size + ox) * 4];
        p[0] = static_cast<unsigned char>(std::clamp(r, 0, 255));
        p[1] = static_cast<unsigned char>(std::clamp(g, 0, 255));
        p[2] = static_cast<unsigned char>(std::clamp(b, 0, 255));
        p[3] = static_cast<unsigned char>(a);
    };
    for (int y = 0; y < kTile; ++y) {
        for (int x = 0; x < kTile; ++x) {
            const int n = static_cast<int>(hash(x, y, 1) % 40) - 20;
            put(0, x, y, 90 + n, 160 + n, 60 + n / 2);                                 // grass top
            const bool green_lip = y < 3 + static_cast<int>(hash(x, 0, 2) % 3);
            if (green_lip) put(1, x, y, 90 + n, 160 + n, 60 + n / 2);                  // grass side
            else put(1, x, y, 125 + n, 88 + n, 55 + n);
            put(2, x, y, 125 + n, 88 + n, 55 + n);                                     // dirt
            put(3, x, y, 128 + n, 128 + n, 132 + n);                                   // stone
            const bool leaf = hash(x, y, 5) % 5 != 0;
            put(4, x, y, 50 + n, 125 + n, 40, leaf ? 255 : 0);                         // leaves (holes)
            const bool bark_line = x % 4 == 0;
            put(5, x, y, (bark_line ? 70 : 100) + n / 2, (bark_line ? 48 : 70) + n / 2, 35);   // log side
            const int ring = static_cast<int>(std::sqrt((x - 7.5f) * (x - 7.5f) + (y - 7.5f) * (y - 7.5f)));
            put(6, x, y, ring % 2 ? 160 : 135, ring % 2 ? 125 : 100, 70);             // log top
            put(7, x, y, 220 + n / 2, 205 + n / 2, 150 + n / 2);                       // sand
            put(8, x, y, 40, 90 + n / 2, 200, 255);                                    // water (alpha from block color)
            const bool frame = x == 0 || y == 0 || x == kTile - 1 || y == kTile - 1;
            put(9, x, y, 220, 235, 245, frame ? 255 : 60);                             // glass
            const bool seam = y % 4 == 0;
            put(10, x, y, seam ? 120 : 175 + n / 2, seam ? 85 : 130 + n / 2, seam ? 50 : 80); // planks
            put(11, x, y, 255, 210 + n, 90 + n);                                       // glow block
        }
    }
    return make_texture(size, size, px.data());
}

} // namespace

int main() {
    App app{{.title = "thistle voxel demo", .width = 1280, .height = 720}};

    VoxelWorld voxels;
    voxels.set_atlas(make_atlas(), kTile);
    const BlockId grass = voxels.add_block(BlockType{.name = "grass"}.set_tiles(0, 1, 2));
    const BlockId dirt = voxels.add_block(BlockType{.name = "dirt"}.set_tiles(2));
    const BlockId stone = voxels.add_block(BlockType{.name = "stone"}.set_tiles(3));
    const BlockId leaves = voxels.add_block(BlockType{.name = "leaves", .alpha = AlphaMode::Cutout}.set_tiles(4));
    const BlockId log = voxels.add_block(BlockType{.name = "log"}.set_tiles(6, 5, 6));
    const BlockId sand = voxels.add_block(BlockType{.name = "sand"}.set_tiles(7));
    const BlockId water = voxels.add_block(BlockType{.name = "water", .color = rgba{1, 1, 1, 0.6f}, .alpha = AlphaMode::Blend, .solid = false}.set_tiles(8));
    const BlockId glass = voxels.add_block(BlockType{.name = "glass", .alpha = AlphaMode::Blend}.set_tiles(9));
    const BlockId planks = voxels.add_block(BlockType{.name = "planks"}.set_tiles(10));
    const BlockId glow = voxels.add_block(BlockType{.name = "glow", .emissive = rgb(1.0f, 0.8f, 0.4f)}.set_tiles(11));
    // Flat-colored blocks, the MagicaVoxel / Teardown look.
    const BlockId red = voxels.add_block({.name = "red", .color = rgb(0.85f, 0.25f, 0.2f)});
    const BlockId cream = voxels.add_block({.name = "cream", .color = rgb(0.95f, 0.9f, 0.78f)});
    const BlockId dark = voxels.add_block({.name = "dark", .color = rgb(0.18f, 0.18f, 0.22f)});

    constexpr int R = 40;
    for (int z = -R; z < R; ++z) {
        for (int x = -R; x < R; ++x) {
            const float h = 6.0f + 3.0f * std::sin(x * 0.12f) * std::cos(z * 0.09f) + 2.0f * std::sin((x + z) * 0.05f);
            const int top = static_cast<int>(h);
            for (int y = 0; y <= top; ++y) voxels.set(x, y, z, y == top ? (top <= 4 ? sand : grass) : (y > top - 3 ? dirt : stone));
            for (int y = top + 1; y <= 4; ++y) voxels.set(x, y, z, water);
        }
    }
    auto tree = [&](int x, int z) {
        int y = 0;
        while (voxels.get(x, y + 1, z) != 0) ++y;
        for (int i = 1; i <= 4; ++i) voxels.set(x, y + i, z, log);
        for (int dz = -2; dz <= 2; ++dz)
            for (int dy = 3; dy <= 6; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    if (std::abs(dx) + std::abs(dz) + (dy == 6 ? 2 : 0) <= 3 && voxels.get(x + dx, y + dy, z + dz) == 0)
                        voxels.set(x + dx, y + dy, z + dz, leaves);
    };
    tree(-6, -4);
    tree(8, -10);
    tree(-14, 6);
    // A little glass-walled hut with a glowing block inside.
    voxels.fill({2, 9, 2}, {8, 9, 8}, planks);
    voxels.fill({2, 10, 2}, {8, 13, 8}, glass);
    voxels.fill({3, 10, 3}, {7, 13, 7}, 0);
    voxels.fill({2, 14, 2}, {8, 14, 8}, planks);
    voxels.set(5, 10, 5, glow);
    // A flat-colored statue on a stone plinth.
    voxels.fill({-4, 9, 6}, {-2, 9, 8}, stone);
    voxels.fill({-4, 10, 7}, {-2, 12, 7}, red);
    voxels.fill({-4, 13, 7}, {-2, 15, 7}, cream);
    voxels.set(-4, 14, 6, dark);
    voxels.set(-2, 14, 6, dark);

    CollisionWorld level;
    level.add(voxels);
    CharacterController player;
    player.position = {0.5f, 20.0f, 12.5f};
    MouseLook look;
    look.pitch = radians(-15.0f);
    bool flying = false;
    const BlockId palette[9] = {grass, dirt, stone, planks, log, leaves, glass, glow, red};
    int selected = 0;

    World world;
    world.sun.direction = {-0.5f, -0.8f, -0.35f};
    world.fog.enabled = true;
    world.fog.start = 35.0f;
    world.fog.end = 70.0f;
    Camera camera;

    app.update([&](Frame f) {
        look.update(camera, f);
        if (f.key_pressed(Key::Tab)) flying = !flying;
        const Key number_keys[9] = {Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5, Key::Num6, Key::Num7, Key::Num8, Key::Num9};
        for (int i = 0; i < 9; ++i) if (f.key_pressed(number_keys[i])) selected = i;
        if (f.mouse_scroll() > 0.0f) selected = (selected + 8) % 9;
        if (f.mouse_scroll() < 0.0f) selected = (selected + 1) % 9;

        if (flying) {
            vec3 move = look.move_input(f) * 12.0f;
            if (f.key_down(Key::Space)) move.y += 12.0f;
            if (f.key_down(Key::LeftShift)) move.y -= 12.0f;
            player.position += move * f.dt;
            player.velocity = {};
        } else {
            player.update(level, look.move_input(f), f.key_pressed(Key::Space), f.dt);
        }
        if (player.position.y < -30.0f) player.position = {0.5f, 20.0f, 12.5f}, player.velocity = {};
        camera.position = player.eye();

        // Break/place what the crosshair points at (only once the mouse is
        // captured, so the click that captures it doesn't also dig).
        const VoxelWorld::Hit target = voxels.raycast(Ray{camera.position, camera.forward()}, 6.0f, true);
        if (target && mouse_locked()) {
            world.wire_box(voxels.block_bounds(target.block), rgba{0, 0, 0, 0.8f});
            if (f.mouse_pressed(Mouse::Left)) voxels.set(target.block, 0);
            if (f.mouse_pressed(Mouse::Right)) {
                const ivec3 place = target.block + target.normal;
                // Don't wall yourself in: skip placing into the player's own box.
                if (!voxels.block_bounds(place).overlaps(player.bounds())) voxels.set(place, palette[selected]);
            }
        }
        world.draw(voxels);
        world.light(PointLight{.position = voxels.block_center({5, 10, 5}), .color = rgb(1, 0.75f, 0.4f), .range = 7, .intensity = 1.5f});
        world.render(f, camera);

        const float cx = f.width * 0.5f, cy = f.height * 0.5f;
        f.rect({cx - 1, cy - 9}, {2, 18}, rgba{1, 1, 1, 0.8f});
        f.rect({cx - 9, cy - 1}, {18, 2}, rgba{1, 1, 1, 0.8f});
        for (int i = 0; i < 9; ++i) {
            const vec2 slot{cx - 9 * 26.0f + i * 52.0f, f.height - 64.0f};
            f.rect(slot, {46, 46}, i == selected ? rgba{1, 1, 1, 0.9f} : rgba{0, 0, 0, 0.45f});
            f.rect(slot + vec2{5, 5}, {36, 36}, voxels.block_type(palette[i]).textured() ? rgb(0.55f, 0.5f, 0.45f) : voxels.block_type(palette[i]).color);
        }
        static int frames = 0;
        if (++frames % 120 == 0) {
            const RenderStats st = render_stats();
            log_info("voxel demo: chunks=" + std::to_string(voxels.chunk_count()) + " draw_calls=" + std::to_string(st.draw_calls) +
                     " triangles=" + std::to_string(st.triangles));
        }
    });
    return app.run();
}
