// Checks the built-in collision: VoxelWorld::raycast (exact grid walk, hit
// face, solid_only), CollisionWorld overlap/raycast against voxels, boxes
// and triangle meshes, and CharacterController behavior simulated at 60 Hz
// — falling and landing, walls stopping it, sliding along a wall, climbing a
// step but not a wall, jumping onto a block, head bumps, and walking on a
// mesh floor. CPU only (models are never uploaded).
#include <thistle.hpp>
#include <cmath>
#include <cstdio>
#include <string>
using namespace thistle;
using namespace thistle::three;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (cond) { std::printf("  ok  %s\n", msg.c_str()); }
    else { std::printf("  FAIL %s\n", msg.c_str()); ++g_failures; }
}
bool near(float a, float b, float eps = 0.02f) { return std::fabs(a - b) <= eps; }

void simulate(CharacterController& c, const CollisionWorld& w, vec3 wish, float seconds, bool jump_at_start = false) {
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < static_cast<int>(seconds * 60.0f); ++i) c.update(w, wish, jump_at_start && i == 0, dt);
}
} // namespace

int main() {
    VoxelWorld v;
    const BlockId stone = v.add_block({.name = "stone"});
    const BlockId water = v.add_block({.name = "water", .alpha = AlphaMode::Blend, .solid = false});
    v.fill({-20, -1, -20}, {20, -1, 20}, stone); // floor: top surface at y = 0

    // --- voxel raycast ---
    {
        const auto h = v.raycast(Ray{{0.5f, 5.0f, 0.5f}, {0, -1, 0}});
        check(h.hit && h.block == ivec3{0, -1, 0} && h.normal == ivec3{0, 1, 0} && near(h.distance, 5.0f, 1e-4f),
              "raycast down hits the floor's top face at the right distance");
        v.set(3, 0, 0, stone);
        const auto side = v.raycast(Ray{{0.5f, 0.5f, 0.5f}, {1, 0, 0}});
        check(side.hit && side.block == ivec3{3, 0, 0} && side.normal == ivec3{-1, 0, 0} && near(side.distance, 2.5f, 1e-4f),
              "raycast sideways hits a block's -X face");
        check(side.block + side.normal == ivec3{2, 0, 0}, "block + normal is the empty cell in front of the face");
        const auto diag = v.raycast(Ray{{0.5f, 2.5f, 0.5f}, normalize(vec3{1, -1, 0.3f})});
        check(diag.hit && diag.distance < 5.0f, "a diagonal ray finds the floor");
        check(!v.raycast(Ray{{0.5f, 5.0f, 0.5f}, {0, 1, 0}}), "raycast upward into empty sky misses");
        check(!v.raycast(Ray{{0.5f, 5.0f, 0.5f}, {0, -1, 0}}, 3.0f), "max_distance stops short of the floor");
        v.set(0, 2, 0, water);
        check(v.raycast(Ray{{0.5f, 5.0f, 0.5f}, {0, -1, 0}}).id == water, "a ray stops at water by default");
        check(v.raycast(Ray{{0.5f, 5.0f, 0.5f}, {0, -1, 0}}, 100.0f, true).id == stone, "solid_only passes through water");
        const auto neg = v.raycast(Ray{{-3.5f, 0.5f, 0.5f}, {-1, 0, 0}}, 30.0f);
        check(!neg.hit, "negative-direction ray across negative coordinates, nothing there");
        v.set(-8, 0, 0, stone);
        check(v.raycast(Ray{{-3.5f, 0.5f, 0.5f}, {-1, 0, 0}}).block == ivec3{-8, 0, 0}, "and it hits a block placed there");
        v.set(3, 0, 0, 0);
        v.set(0, 2, 0, 0);
        v.set(-8, 0, 0, 0);
    }

    CollisionWorld world;
    world.add(v);

    // --- falling and landing ---
    {
        CharacterController c;
        c.position = {0.5f, 3.0f, 0.5f};
        simulate(c, world, {}, 2.0f);
        check(c.on_ground() && near(c.position.y, 0.0f), "falls and lands on the floor (y = 0)");
        check(near(c.velocity.y, 0.0f, 1e-3f), "vertical velocity is zeroed on landing");
    }
    // --- walls ---
    {
        v.fill({5, 0, -5}, {5, 3, 5}, stone); // wall at x = 5..6, 4 high
        CharacterController c;
        c.position = {0.5f, 0.0f, 0.5f};
        simulate(c, world, {1, 0, 0}, 3.0f);
        check(near(c.position.x, 5.0f - c.radius, 0.01f), "walking into a wall stops flush against it");
        CharacterController s;
        s.position = {0.5f, 0.0f, 0.5f};
        simulate(s, world, normalize(vec3{1, 0, 1}), 1.5f);
        check(near(s.position.x, 5.0f - s.radius, 0.01f) && s.position.z > 3.0f, "walking diagonally into the wall slides along it");
    }
    // --- steps ---
    {
        v.fill({-15, 0, -5}, {-5, 0, 5}, stone); // a platform one block up, from x = -4 outward
        CharacterController c;
        c.position = {-2.5f, 0.0f, 0.5f};
        c.step_height = 0.55f;
        simulate(c, world, {-1, 0, 0}, 1.5f);
        check(c.position.x > -4.8f && near(c.position.y, 0.0f), "a 1-block step stops a character with step_height 0.55");
        c.step_height = 1.05f; // Minecraft-style auto-jump height
        simulate(c, world, {-1, 0, 0}, 1.5f);
        check(c.position.x < -5.0f && near(c.position.y, 1.0f), "with step_height 1.05 it walks up onto the block");
        CharacterController j;
        j.position = {-2.5f, 0.0f, 0.5f};
        simulate(j, world, {}, 0.2f);
        for (int i = 0; i < 90; ++i) j.update(world, {-1, 0, 0}, i == 0, 1.0f / 60.0f);
        check(j.position.x < -5.0f && near(j.position.y, 1.0f), "jumping while walking gets onto the 1-block step");
        CharacterController tall;
        tall.position = {2.5f, 0.0f, 0.5f};
        tall.step_height = 1.05f;
        simulate(tall, world, {1, 0, 0}, 2.0f);
        check(tall.position.x < 5.0f && near(tall.position.y, 0.0f), "a 4-high wall is not climbable even with auto-step");
    }
    // --- head bump ---
    {
        v.fill({10, 2, 10}, {10, 2, 10}, stone); // ceiling block 2 above the floor
        CharacterController c;
        c.position = {10.5f, 0.0f, 10.5f};
        c.height = 1.8f;
        simulate(c, world, {}, 0.1f);
        c.update(world, {}, true, 1.0f / 60.0f);
        float max_y = 0.0f;
        for (int i = 0; i < 60; ++i) { c.update(world, {}, false, 1.0f / 60.0f); max_y = std::fmax(max_y, c.position.y); }
        check(max_y < 0.21f && c.on_ground(), "jumping under a low ceiling bumps the head and lands again");
    }
    // --- coyote time / jump buffer ---
    {
        CharacterController c;
        c.position = {0.5f, 0.0f, 0.5f};
        simulate(c, world, {}, 0.2f);
        c.update(world, {}, true, 1.0f / 60.0f);
        check(c.velocity.y > 7.0f, "pressing jump while grounded jumps");
        for (int i = 0; i < 20; ++i) c.update(world, {}, false, 1.0f / 60.0f);
        check(!c.on_ground(), "and it's airborne afterwards");
    }

    // --- meshes and boxes ---
    {
        CollisionWorld mw;
        const Model floor_model = make_model(plane_mesh(20, 20));
        mw.add(floor_model, Transform{0.0f, 2.0f, 0.0f}); // a mesh floor at y = 2
        mw.add_box(Bounds{{3, 2, -2}, {4, 5, 2}});         // a box wall standing on it
        // A cube tilted 20 degrees: its face toward +X is a 70-degree slope (a
        // wall, past max_slope) and it's hit well before its bounding box.
        const Model tilted = make_model(box_mesh({2, 2, 2}));
        mw.add(tilted, Transform{{-4.0f, 2.0f, 0.0f}, quat::axis_angle({0, 0, 1}, radians(20))});
        // A 30-degree ramp (a long thin box, tipped) that should be walkable.
        const Model ramp = make_model(box_mesh({8, 0.2f, 4}));
        mw.add(ramp, Transform{{0.0f, 3.9f, 7.0f}, quat::axis_angle({0, 0, 1}, radians(30))}); // low end at floor level

        check(mw.overlaps(Bounds{{-1, 1.9f, -1}, {1, 2.1f, 1}}), "a box straddling the mesh floor overlaps it");
        check(!mw.overlaps(Bounds{{-1, 2.1f, -1}, {1, 3, 1}}), "a box just above the mesh floor doesn't");
        const RaycastHit rh = mw.raycast(Ray{{0, 10, 0}, {0, -1, 0}});
        check(rh.hit && near(rh.point.y, 2.0f, 1e-3f), "raycast down hits the mesh floor");
        check(mw.raycast(Ray{{0, 3, 0}, {1, 0, 0}}).hit && near(mw.raycast(Ray{{0, 3, 0}, {1, 0, 0}}).distance, 3.0f, 1e-3f),
              "raycast sideways hits the box wall");

        CharacterController c;
        c.position = {0.0f, 4.0f, 0.0f};
        simulate(c, mw, {}, 2.0f);
        check(c.on_ground() && near(c.position.y, 2.0f, 0.01f), "lands on a triangle-mesh floor");
        simulate(c, mw, {1, 0, 0}, 2.0f);
        check(near(c.position.x, 3.0f - c.radius, 0.01f), "stopped by a box collider");
        CharacterController r;
        r.position = {-1.0f, 2.0f, 0.0f};
        simulate(r, mw, {}, 0.1f);
        simulate(r, mw, {-1, 0, 0}, 2.0f);
        // The face meets the floor at x = -4 + 1/cos(20) = -2.936; the box's left side stops there.
        check(near(r.position.x, -2.936f + r.radius, 0.03f) && near(r.position.y, 2.0f, 0.01f),
              "a 70-degree mesh face blocks like a wall, exactly where the triangles are");
        CharacterController up;
        up.position = {-4.5f, 2.0f, 7.0f};
        simulate(up, mw, {}, 0.1f);
        simulate(up, mw, {1, 0, 0}, 1.5f);
        check(up.position.y > 4.5f, "a 30-degree mesh ramp can be walked up");
    }

    // --- high frame rates ---
    // Standing still at 144-3000 fps, with uneven frame times. Found on a
    // real GPU (Windows, RTX 4070 Super): a whole frame's fall there is less
    // than the 1 mm the controller keeps from surfaces, and those tiny steps
    // sank a character through a block floor (falling out of the world at
    // 240 fps) and made on_ground() flicker. Everything above runs at 60 Hz,
    // where a frame's fall is ~7 mm, and never saw it.
    {
        VoxelWorld hv;
        hv.fill({-10, -1, -10}, {10, -1, 10}, hv.add_block({.name = "stone"})); // top at y = 0
        CollisionWorld vw;
        vw.add(hv);
        CollisionWorld bw;
        bw.add_box(Bounds{{-10, -1, -10}, {10, 0, 10}});
        CollisionWorld mw;
        const Model floor_model = make_model(plane_mesh(20, 20));
        mw.add(floor_model, Transform{0.0f, 0.0f, 0.0f});
        Terrain land(32, 32, 1.0f);
        land.origin = {-16, 0, -16};
        land.generate([](float x, float z) { return 3.0f + 0.2f * x + 0.1f * z; }); // a gentle slope
        CollisionWorld tw;
        tw.add(land);
        struct Floor { const char* name; const CollisionWorld* w; float surface; };
        const Floor floors[] = {{"block", &vw, 0.0f}, {"box", &bw, 0.0f}, {"mesh", &mw, 0.0f}, {"terrain", &tw, land.height_at(0.5f, 0.5f)}};
        for (const Floor& fl : floors) {
            for (float fps : {144.0f, 240.0f, 1000.0f, 3000.0f}) {
                CharacterController c;
                c.position = {0.5f, fl.surface + 2.0f, 0.5f};
                simulate(c, *fl.w, {}, 2.0f); // land at 60 Hz
                const float rest = c.position.y;
                uint32_t seed = 7;
                int grounded = 0, frames = 0;
                for (float t = 0.0f; t < 5.0f; ++frames) {
                    seed = seed * 1664525u + 1013904223u;
                    const float dt = (0.5f + static_cast<float>(seed >> 8) / 16777216.0f) / fps; // 0.5x..1.5x
                    c.update(*fl.w, {}, false, dt);
                    grounded += c.on_ground();
                    t += dt;
                }
                char what[96];
                std::snprintf(what, sizeof what, "%s floor, %.0f fps: standing still for 5 s", fl.name, fps);
                check(std::fabs(c.position.y - rest) < 1e-4f, std::string(what) + " doesn't sink (moved " + std::to_string(c.position.y - rest) + " m)");
                check(grounded == frames, std::string(what) + " is on the ground every frame (" + std::to_string(grounded) + "/" + std::to_string(frames) + ")");
                // And a jump still happens and lands where it started.
                c.update(*fl.w, {}, true, 1.0f / fps);
                const float launched = c.position.y;
                for (int i = 0; i < static_cast<int>(fps * 1.5f); ++i) c.update(*fl.w, {}, false, 1.0f / fps);
                check(launched > rest && std::fabs(c.position.y - rest) < 2e-3f && c.on_ground(),
                      std::string(what) + ", then a jump lands back on it");
            }
        }
    }

    if (g_failures) { std::printf("%d check(s) failed\n", g_failures); return 1; }
    std::printf("all character checks passed\n");
    return 0;
}
