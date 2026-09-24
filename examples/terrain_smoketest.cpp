// Checks noise (deterministic, seeded, continuous, in range, zero on the
// lattice), Terrain (height_at matches the grid points and the triangle
// split, normals, bounds, a character landing on it via CollisionWorld),
// and VoxelWorld streaming (generates nearest-first within a budget, drops
// far chunks, keeps player-edited ones, never regenerates over a loaded
// save). CPU only.
#include <thistle.hpp>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
using namespace thistle;
using namespace thistle::three;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (cond) { std::printf("  ok  %s\n", msg.c_str()); }
    else { std::printf("  FAIL %s\n", msg.c_str()); ++g_failures; }
}
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
} // namespace

int main() {
    // --- noise ---
    {
        check(perlin(3.7f, -12.2f, 42) == perlin(3.7f, -12.2f, 42), "perlin is deterministic");
        check(perlin(3.7f, -12.2f, 1) != perlin(3.7f, -12.2f, 2), "different seeds give different noise");
        check(perlin(5.0f, -3.0f) == 0.0f && perlin3(1.0f, 2.0f, 3.0f) == 0.0f, "zero at whole-number points (Perlin's lattice)");
        float lo = 1e9f, hi = -1e9f, max_step = 0.0f;
        float prev = perlin(0.0f, 0.3f);
        for (int i = 1; i < 20000; ++i) {
            const float v = perlin(i * 0.01f, 0.3f + i * 0.0037f);
            lo = std::fmin(lo, v);
            hi = std::fmax(hi, v);
            max_step = std::fmax(max_step, std::fabs(v - prev));
            prev = v;
        }
        check(lo >= -1.01f && hi <= 1.01f && hi - lo > 1.0f, "2D perlin stays in -1..1 and uses most of it");
        check(max_step < 0.05f, "perlin is continuous (tiny steps -> tiny changes)");
        float f_lo = 1e9f, f_hi = -1e9f, r_lo = 1e9f, r_hi = -1e9f;
        for (int i = 0; i < 5000; ++i) {
            const float x = i * 0.137f, y = i * 0.071f;
            const float f = fbm(x, y, 6, 9), r = ridged(x, y, 5, 9);
            f_lo = std::fmin(f_lo, f); f_hi = std::fmax(f_hi, f);
            r_lo = std::fmin(r_lo, r); r_hi = std::fmax(r_hi, r);
        }
        check(f_lo >= -1.01f && f_hi <= 1.01f, "fbm stays in -1..1");
        check(r_lo >= 0.0f && r_hi <= 1.0f, "ridged stays in 0..1");
    }

    // --- terrain ---
    {
        Terrain t(64, 48, 0.5f);
        t.origin = {-10, 2, 5};
        t.set_height(3, 4, 7.0f);
        check(near(t.height_at(-10 + 3 * 0.5f, 5 + 4 * 0.5f), 9.0f), "height_at hits grid points exactly (+ origin.y)");
        t.set_height(4, 4, 7.0f);
        t.set_height(3, 5, 7.0f);
        t.set_height(4, 5, 7.0f);
        check(near(t.height_at(-10 + 3.5f * 0.5f, 5 + 4.5f * 0.5f), 9.0f), "flat patch interpolates flat");
        check(near(t.normal_at(-10 + 3.5f * 0.5f, 5 + 4.5f * 0.5f).y, 1.0f), "flat patch normal points up");
        Terrain ramp(4, 4, 1.0f);
        ramp.generate([](float x, float) { return x * 0.5f; });
        check(near(ramp.height_at(2.3f, 1.7f), 1.15f) && near(ramp.height_at(0.0f, 3.9f), 0.0f), "generate() + a planar ramp interpolates linearly");
        const vec3 n = ramp.normal_at(1.5f, 1.5f);
        check(near(n.x, -0.5f / std::sqrt(1.25f)) && near(n.y, 1.0f / std::sqrt(1.25f)), "ramp normal leans against the slope");
        // Non-planar cell: the value must come from whichever triangle the point is in.
        Terrain bump(1, 1, 1.0f);
        bump.set_height(1, 1, 4.0f); // only the far corner raised
        check(near(bump.height_at(0.25f, 0.75f), 0.25f * 4.0f) && near(bump.height_at(0.75f, 0.25f), 0.25f * 4.0f),
              "height_at follows the (0,0)-(1,1) diagonal split on both sides");
        const Bounds b = ramp.bounds();
        check(near(b.max.x, 4.0f) && near(b.max.y, 2.0f) && near(b.min.y, 0.0f), "bounds covers the grid and the height range");

        Terrain hills(40, 40, 1.0f);
        hills.origin = {-20, 0, -20};
        hills.generate([](float x, float z) { return 3.0f * fbm(x * 0.05f, z * 0.05f, 4, 7); });
        CollisionWorld level;
        level.add(hills);
        CharacterController c;
        c.position = {1.3f, 20.0f, -2.7f};
        for (int i = 0; i < 180; ++i) c.update(level, {}, false, 1.0f / 60.0f);
        check(c.on_ground() && std::fabs(c.position.y - hills.height_at(1.3f, -2.7f)) < 0.35f, "a character lands on the terrain surface");
        const RaycastHit h = level.raycast(Ray{{5.5f, 50.0f, 5.5f}, {0, -1, 0}});
        check(h && near(h.point.y, hills.height_at(5.5f, 5.5f), 1e-3f), "raycast hits the terrain at height_at()");
    }

    // --- voxel streaming ---
    {
        VoxelWorld w;
        const BlockId stone = w.add_block({.name = "stone"});
        std::set<std::string> calls;
        w.set_generator([&](VoxelWorld& world, ivec3 c) {
            calls.insert(std::to_string(c.x) + "," + std::to_string(c.y) + "," + std::to_string(c.z));
            if (c.y != 0) return; // one layer of ground chunks, air elsewhere
            for (int z = 0; z < 32; ++z)
                for (int x = 0; x < 32; ++x) world.set(c.x * 32 + x, 0, c.z * 32 + z, stone);
        });
        w.stream_around({16, 16, 16}, 40.0f, 3);
        check(calls.size() == 3 && calls.count("0,0,0"), "budget 3: three chunks, the one we're in first");
        for (int i = 0; i < 100; ++i) w.stream_around({16, 16, 16}, 40.0f, 3);
        const size_t settled = calls.size();
        check(settled > 3 && settled < 100, "keeps going until every chunk in the radius is generated (" + std::to_string(settled) + ")");
        w.stream_around({16, 16, 16}, 40.0f, 3);
        check(calls.size() == settled, "nothing is generated twice");
        check(w.get(5, 0, 5) == stone && w.get(-20, 0, 40) == stone, "generated ground is there");
        w.set(5, 1, 5, stone); // the player builds something at home
        for (int i = 0; i < 200; ++i) w.stream_around({3000, 16, 16}, 40.0f, 8); // walk far away
        check(w.get(40, 0, 40) == 0, "far chunks were dropped");
        check(w.get(5, 1, 5) == stone && w.get(5, 0, 5) == stone, "the edited chunk was kept, edits and all");
        calls.clear();
        for (int i = 0; i < 200; ++i) w.stream_around({40, 16, 40}, 40.0f, 8); // and back
        check(!calls.count("0,0,0") && w.get(5, 1, 5) == stone, "coming back doesn't regenerate over the player's chunk");
        check(calls.count("1,0,1") == 1 && w.get(40, 0, 40) == stone, "but dropped untouched chunks come back");

        const std::vector<uint8_t> save = w.serialize();
        VoxelWorld loaded;
        loaded.deserialize(save.data(), save.size());
        int regenerated = 0;
        loaded.set_generator([&](VoxelWorld& world, ivec3 c) { ++regenerated; if (c.y == 0) world.set(c.x * 32, 0, c.z * 32, 0); });
        loaded.stream_around({16, 16, 16}, 20.0f, 50);
        check(loaded.get(5, 1, 5) == stone && loaded.get(0, 0, 0) == stone, "a loaded save is never regenerated over");
    }

    if (g_failures) { std::printf("%d check(s) failed\n", g_failures); return 1; }
    std::printf("all terrain checks passed\n");
    return 0;
}
