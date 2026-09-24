// VoxelDestruction showcase (needs THISTLE_PHYSICS3D): a little voxel town
// at 25 cm per block (a brick house, a stone tower, a wooden bridge, a
// tree) to take apart. Whatever loses its support falls as debris, and
// debris can be blown apart again.
//
//   WASD + mouse   fly (Shift = faster; click to capture the mouse, Esc frees it)
//   left click     blast (1.2 m)
//   right click    chip away (0.45 m)
//   Q              throw a heavy ball
//   F              show the colliders
//   R              rebuild the town
//
// Blasts and impacts are heard where they happen (3D sound: the impacts come
// from the physics contact events, louder the harder the hit). The sounds
// are made in code at startup, so the demo needs no asset files.
//
// Stats go to the log. Opens a window and never quits on its own, so CI
// builds it but doesn't run it.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
using namespace thistle;
using namespace thistle::three;

namespace {

uint32_t hash3(int x, int y, int z) {
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ static_cast<uint32_t>(z) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}

struct Palette {
    BlockId grass[3], dirt, brick[3], mortar, stone[3], roof[2], wood[2], leaves[2], glass;
};

Palette make_palette(VoxelWorld& v) {
    Palette p{};
    auto add = [&](const char* name, rgba c) { return v.add_block({.name = name, .color = c}); };
    p.grass[0] = add("grass0", rgb(0.36f, 0.56f, 0.27f));
    p.grass[1] = add("grass1", rgb(0.33f, 0.52f, 0.25f));
    p.grass[2] = add("grass2", rgb(0.40f, 0.60f, 0.30f));
    p.dirt = add("dirt", rgb(0.45f, 0.33f, 0.22f));
    p.brick[0] = add("brick0", rgb(0.66f, 0.30f, 0.22f));
    p.brick[1] = add("brick1", rgb(0.60f, 0.27f, 0.20f));
    p.brick[2] = add("brick2", rgb(0.72f, 0.36f, 0.26f));
    p.mortar = add("mortar", rgb(0.78f, 0.75f, 0.70f));
    p.stone[0] = add("stone0", rgb(0.55f, 0.55f, 0.57f));
    p.stone[1] = add("stone1", rgb(0.50f, 0.50f, 0.52f));
    p.stone[2] = add("stone2", rgb(0.60f, 0.60f, 0.62f));
    p.roof[0] = add("roof0", rgb(0.25f, 0.27f, 0.33f));
    p.roof[1] = add("roof1", rgb(0.22f, 0.24f, 0.30f));
    p.wood[0] = add("wood0", rgb(0.55f, 0.40f, 0.25f));
    p.wood[1] = add("wood1", rgb(0.50f, 0.36f, 0.22f));
    p.leaves[0] = add("leaves0", rgb(0.25f, 0.50f, 0.22f));
    p.leaves[1] = add("leaves1", rgb(0.30f, 0.56f, 0.25f));
    p.glass = v.add_block({.name = "glass", .color = rgba{0.7f, 0.85f, 0.95f, 0.35f}, .alpha = AlphaMode::Blend});
    return p;
}

void build_town(VoxelWorld& v, const Palette& p) {
    v.clear();
    auto pick = [](const BlockId* ids, int n, int x, int y, int z) { return ids[hash3(x, y, z) % n]; };
    // Ground: 1 block of grass on 2 of dirt (y = -2..0; ground_y = 0 holds it all).
    for (int z = -60; z < 60; ++z) {
        for (int x = -60; x < 60; ++x) {
            v.set(x, 0, z, pick(p.grass, 3, x, 0, z));
            v.set(x, -1, z, p.dirt);
        }
    }
    // A brick house, 20 x 14 x 16 blocks (5 x 3.5 x 4 m), mortar lines every 3 rows.
    const ivec3 h0{-24, 1, -8}, h1{-5, 14, 7};
    for (int y = h0.y; y <= h1.y; ++y) {
        for (int z = h0.z; z <= h1.z; ++z) {
            for (int x = h0.x; x <= h1.x; ++x) {
                const bool wall = x == h0.x || x == h1.x || z == h0.z || z == h1.z;
                if (!wall) continue;
                const bool door = z == h1.z && x >= -16 && x <= -13 && y <= 8;
                const bool window = (y >= 6 && y <= 10) && ((z == h1.z && (x == -21 || x == -20 || x == -9 || x == -8)) ||
                                                           (x == h0.x && (z == -2 || z == -1 || z == 2 || z == 3)));
                if (door) continue;
                if (window) { v.set(x, y, z, p.glass); continue; }
                v.set(x, y, z, y % 3 == 0 ? p.mortar : pick(p.brick, 3, x, y, z));
            }
        }
    }
    // A pitched roof: each layer steps in by one on the long sides.
    for (int i = 0; i < 9; ++i) {
        for (int x = h0.x - 1; x <= h1.x + 1; ++x) {
            for (int z = h0.z - 1 + i; z <= h1.z + 1 - i; ++z) {
                const bool edge = z == h0.z - 1 + i || z == h1.z + 1 - i || x == h0.x - 1 || x == h1.x + 1;
                if (edge) v.set(x, h1.y + 1 + i, z, pick(p.roof, 2, x, i, z));
            }
        }
    }
    // A stone tower, 8 x 44 x 8, hollow, with slit windows.
    const ivec3 t0{10, 1, -20}, t1{17, 44, -13};
    for (int y = t0.y; y <= t1.y; ++y) {
        for (int z = t0.z; z <= t1.z; ++z) {
            for (int x = t0.x; x <= t1.x; ++x) {
                const bool wall = x == t0.x || x == t1.x || z == t0.z || z == t1.z;
                const bool slit = (y % 10 >= 5 && y % 10 <= 7) && (x == 13 || x == 14 || z == -17 || z == -16);
                if (wall && !slit) v.set(x, y, z, pick(p.stone, 3, x, y, z));
            }
        }
    }
    // Battlements.
    for (int z = t0.z; z <= t1.z; ++z) {
        for (int x = t0.x; x <= t1.x; ++x) {
            const bool rim = x == t0.x || x == t1.x || z == t0.z || z == t1.z;
            if (rim && ((x + z) & 1)) v.set(x, t1.y + 1, z, pick(p.stone, 3, x, 99, z));
        }
    }
    // A wooden bridge between two stone piers.
    for (int pier : {18, 42}) {
        v.fill({pier, 1, 14}, {pier + 3, 10, 17}, p.stone[1]);
    }
    for (int x = 18; x <= 45; ++x) {
        for (int z = 13; z <= 18; ++z) v.set(x, 11, z, pick(p.wood, 2, x, 11, z));
        if (x % 3 == 0) {
            v.set(x, 12, 13, p.wood[1]);
            v.set(x, 12, 18, p.wood[1]);
        }
    }
    // A tree.
    v.fill({-6, 1, 24}, {-5, 12, 25}, p.wood[1]);
    for (int y = 10; y <= 18; ++y) {
        for (int z = 18; z <= 31; ++z) {
            for (int x = -12; x <= 1; ++x) {
                const float dx = x + 5.0f, dy = (y - 14.0f) * 1.3f, dz = z - 24.5f;
                if (dx * dx + dy * dy + dz * dz < 36.0f && hash3(x, y, z) % 7 != 0) v.set(x, y, z, pick(p.leaves, 2, x, y, z));
            }
        }
    }
}

// A mono 16-bit WAV from samples in -1..1.
void write_wav(const std::string& path, const std::vector<float>& samples, int rate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint32_t bytes = static_cast<uint32_t>(samples.size() * 2);
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(1); u32(static_cast<uint32_t>(rate)); u32(static_cast<uint32_t>(rate * 2)); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (float x : samples) {
        const int16_t v = static_cast<int16_t>(std::clamp(x, -1.0f, 1.0f) * 32000.0f);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
}

// Low-passed noise under a falling sine, dying away: a boom, or (short and
// higher) a thud. `cutoff` 0..1 is how much of the noise gets through.
std::vector<float> rumble(float seconds, float thump_hz, float cutoff, float decay, uint32_t seed) {
    const int rate = 44100, n = static_cast<int>(seconds * rate);
    std::vector<float> out(static_cast<size_t>(n));
    float lp = 0.0f, phase = 0.0f;
    for (int i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float noise = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
        lp += cutoff * (noise - lp);
        const float t = static_cast<float>(i) / rate;
        phase += 2.0f * pi * thump_hz * (1.0f - 0.5f * t / seconds) / rate;
        const float env = std::exp(-t * decay) * std::min(1.0f, t * 400.0f);
        out[static_cast<size_t>(i)] = env * (0.8f * lp * 3.0f + 0.6f * std::sin(phase));
    }
    return out;
}

} // namespace

int main() {
    App app{{.title = "thistle destruction demo", .width = 1280, .height = 720}};
    log_info("destruction demo: WASD+mouse fly, left click blasts, right click chips, Q throws a ball, F colliders, R rebuilds");

    const std::filesystem::path tmp = std::filesystem::temp_directory_path();
    const std::string boom_wav = (tmp / "thistle_demo_boom.wav").string();
    const std::string thud_wav = (tmp / "thistle_demo_thud.wav").string();
    write_wav(boom_wav, rumble(1.6f, 48.0f, 0.12f, 3.0f, 1), 44100);
    write_wav(thud_wav, rumble(0.3f, 95.0f, 0.25f, 18.0f, 7), 44100);
    preload_sound(boom_wav);
    preload_sound(thud_wav);

    VoxelWorld town;
    town.voxel_size = 0.25f;
    const Palette pal = make_palette(town);
    build_town(town, pal);
    town.remesh_all();

    Physics3D physics;
    auto boom = std::make_unique<VoxelDestruction>(town, physics);
    std::vector<RigidBody> balls;

    World world;
    world.sun.direction = {-0.5f, -0.75f, -0.4f};
    world.fog.enabled = true;
    world.fog.start = 30.0f;
    world.fog.end = 80.0f;
    Camera camera;
    camera.position = {-2.0f, 5.0f, 16.0f};
    FlyCamera fly;
    fly.look.pitch = radians(-10.0f);
    fly.speed = 6.0f;
    bool debug = false;

    app.update([&](Frame f) {
        fly.update(camera, f);
        const Ray aim{camera.position, camera.rotation.forward()};

        if (f.key_pressed(Key::R)) {
            boom.reset(); // debris bodies go with it
            for (RigidBody b : balls) physics.remove(b);
            balls.clear();
            build_town(town, pal);
            boom = std::make_unique<VoxelDestruction>(town, physics);
        }
        if (f.key_pressed(Key::F)) debug = !debug;
        if (mouse_locked() && (f.mouse_pressed(Mouse::Left) || f.mouse_pressed(Mouse::Right))) {
            if (const PhysicsHit hit = physics.raycast(aim, 120.0f)) {
                const bool big = f.mouse_pressed(Mouse::Left);
                const int n = big ? boom->explode(hit.point, 1.2f, 10.0f) : boom->carve(hit.point, 0.45f);
                play_sound_at(big ? boom_wav : thud_wav, hit.point, {.volume = big ? 1.0f : 0.6f, .min_distance = 3.0f, .max_distance = 90.0f});
                log_info("destruction demo: removed " + std::to_string(n) + " blocks, " + std::to_string(boom->debris_count()) + " pieces of debris");
            }
        }
        if (f.key_pressed(Key::Q)) {
            balls.push_back(physics.add(BodySettings{.collider = Collider::sphere(0.3f), .position = aim.at(0.8f),
                                                     .velocity = aim.direction * 30.0f, .density = 7800.0f, .fast = true}));
        }

        physics.step(f.dt);
        boom->update(f.dt);
        // Impacts, loudest first, a few per frame (a collapsing wall makes hundreds).
        std::vector<Contact> hits(physics.contacts().begin(), physics.contacts().end());
        std::sort(hits.begin(), hits.end(), [](const Contact& a, const Contact& b) { return a.speed > b.speed; });
        for (size_t i = 0; i < hits.size() && i < 4 && hits[i].speed > 2.5f; ++i) {
            const float pitch = 0.8f + 0.4f * static_cast<float>(hash3(static_cast<int>(hits[i].point.x * 10), i, 0) % 100) / 100.0f;
            play_sound_at(thud_wav, hits[i].point, {.volume = std::min(1.0f, hits[i].speed / 9.0f), .pitch = pitch, .min_distance = 2.0f});
        }

        boom->draw(world);
        for (RigidBody b : balls) world.sphere(physics.transform(b, {0.6f, 0.6f, 0.6f}), rgb(0.2f, 0.2f, 0.22f));
        if (debug) physics.draw_debug(world);
        world.render(f, camera);

        const float cx = f.width * 0.5f, cy = f.height * 0.5f;
        f.rect({cx - 1, cy - 8}, {2, 16}, rgba{1, 1, 1, 0.8f});
        f.rect({cx - 8, cy - 1}, {16, 2}, rgba{1, 1, 1, 0.8f});

        static int frames = 0;
        if (++frames % 120 == 0) {
            const RenderStats st = render_stats();
            log_info("destruction demo: blocks=" + std::to_string(town.block_count()) + " debris=" + std::to_string(boom->debris_count()) +
                     " bodies=" + std::to_string(physics.body_count()) + " chips=" + std::to_string(boom->chips.count()) +
                     " draw_calls=" + std::to_string(st.draw_calls) + " fps=" + std::to_string(static_cast<int>(1.0f / std::max(f.dt, 1e-4f))));
        }
    });
    return app.run();
}
