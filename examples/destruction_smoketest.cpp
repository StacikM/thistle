// Checks Teardown-style destruction and the voxel physics under it:
// Collider::voxels box merging, Physics3D::add_static(VoxelWorld) staying in
// sync with edits (and waking what rested on changed blocks), and
// VoxelDestruction deciding what falls: a cut pillar drops its roof, a
// notched wall stays up, max_piece / min_piece / anchors, debris splitting
// when carved again, max_debris, and debris falling out of the world.
// Headless: an App is made (chips need a texture) but never run.
//
// Built without Jolt it checks the fallback instead: carving works and
// nothing falls.
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
std::string num(float v) { char b[32]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
void run(Physics3D& p, float seconds) {
    for (int i = 0; i < static_cast<int>(seconds * 60.0f + 0.5f); ++i) p.step(1.0f / 60.0f);
}
vec3 center_of(ivec3 b) { return {b.x + 0.5f, b.y + 0.5f, b.z + 0.5f}; } // for voxel_size 1 at the origin
} // namespace

int main() {
    App app{{.title = "destruction smoketest", .width = 64, .height = 64}};

    std::printf("voxel helpers\n");
    {
        VoxelWorld v;
        const BlockId stone = v.add_block({.name = "stone"});
        const BlockId water = v.add_block({.name = "water", .alpha = AlphaMode::Blend, .solid = false});
        v.fill({0, 0, 0}, {3, 3, 3}, stone);
        Collider c = Collider::voxels(v);
        check(c.kind == Collider::Kind::Voxels && c.points.size() == 2 && c.points[0] == vec3{0, 0, 0} && c.points[1] == vec3{4, 4, 4},
              "a solid 4x4x4 cube is one box");
        v.set(0, 4, 0, stone);
        v.set(2, 4, 2, water);
        c = Collider::voxels(v);
        check(c.points.size() == 4, "a cube with one block on top is two boxes, and water isn't solid");
        VoxelWorld bar;
        bar.voxel_size = 0.5f;
        const BlockId b = bar.add_block({.name = "b"});
        bar.fill({0, 0, 0}, {39, 0, 0}, b);
        const Collider cb = Collider::voxels(bar);
        check(cb.points.size() == 4 && near(cb.size.x, 0.5f), "a 40-long bar spans two chunks: two boxes, voxel size kept");
        check(v.block_count() == 66 && bar.block_count() == 40, "block_count counts non-air blocks");
        int seen = 0;
        v.each_block([&](ivec3, BlockId) { ++seen; });
        check(seen == 66, "each_block visits every block");
        VoxelWorld copy;
        copy.copy_block_types(v);
        check(copy.block_type_count() == 2 && copy.find_block("water") == water, "copy_block_types takes the ids");
        VoxelWorld spun;
        spun.rotation = quat::euler(0.3f, 0.2f, 0.1f);
        VoxelWorld moved;
        moved = std::move(spun);
        check(near(moved.rotation.x, quat::euler(0.3f, 0.2f, 0.1f).x, 1e-6f), "move assignment keeps the rotation (it didn't)");
    }

    if (!physics3d_available()) {
        std::printf("Physics3D is off in this build: checking the fallback\n");
        VoxelWorld v;
        const BlockId stone = v.add_block({.name = "stone"});
        v.fill({0, 0, 0}, {4, 0, 4}, stone);
        v.fill({2, 1, 2}, {2, 5, 2}, stone);
        Physics3D p;
        VoxelDestruction d(v, p);
        const int n = d.carve(center_of({2, 2, 2}), 0.6f);
        check(n == 1 && v.get(2, 2, 2) == 0 && v.get(2, 4, 2) == stone && d.debris_count() == 0,
              "carving works, and the part above stays where it is");
        std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
        return g_failures ? 1 : 0;
    }

    std::printf("voxel world as a static collider\n");
    {
        VoxelWorld v;
        const BlockId stone = v.add_block({.name = "stone"});
        v.fill({0, 0, 0}, {19, 1, 19}, stone); // two layers: top at y = 2
        Physics3D p;
        p.add_static(v);
        const RigidBody box = p.add_box({10, 5, 10}, {1, 1, 1});
        run(p, 3.0f);
        check(near(p.position(box).y, 2.5f), "a box lands on the blocks (y = " + num(p.position(box).y) + ")");
        check(p.sleeping(box), "and falls asleep");
        const PhysicsHit h = p.raycast(Ray{{3.5f, 10, 3.5f}, {0, -1, 0}});
        check(h && near(h.point.y, 2.0f, 1e-3f), "raycasts hit the blocks");
        v.fill({8, 1, 8}, {12, 1, 12}, 0); // dig out the top layer under the box
        run(p, 2.0f);
        check(near(p.position(box).y, 1.5f), "digging under it wakes it and it drops into the hole (y = " + num(p.position(box).y) + ")");
        v.origin = {100, 0, 0};
        run(p, 0.1f);
        check(!p.raycast(Ray{{3.5f, 10, 3.5f}, {0, -1, 0}}) && p.raycast(Ray{{103.5f, 10, 3.5f}, {0, -1, 0}}),
              "moving the world moves its colliders");
        p.remove_static(v);
        check(!p.raycast(Ray{{103.5f, 10, 3.5f}, {0, -1, 0}}), "remove_static takes them out");
    }

    // A level: a ground slab (y = 0) with a pillar holding up a roof.
    auto build = [](VoxelWorld& v, BlockId stone) {
        v.fill({0, 0, 0}, {10, 0, 10}, stone);
        v.fill({5, 1, 5}, {5, 6, 5}, stone); // pillar y 1..6
        v.fill({3, 7, 3}, {7, 7, 7}, stone); // roof, 25 blocks
    };

    std::printf("what falls\n");
    {
        VoxelWorld v;
        const BlockId stone = v.add_block({.name = "stone", .color = rgb(0.5f, 0.5f, 0.5f)});
        build(v, stone);
        Physics3D p;
        VoxelDestruction d(v, p);
        const int before = v.block_count();
        const int n = d.carve(center_of({5, 3, 5}), 0.6f);
        check(n == 1, "carve removes the one block in reach");
        check(d.debris_count() == 1 && d.debris_blocks(0).block_count() == 28, "the pillar top + roof (28 blocks) break off as one piece");
        check(v.block_count() == before - 29 && v.get(5, 7, 5) == 0 && v.get(5, 2, 5) == stone, "they leave the level; the stump stays");
        check(d.chips.count() > 0, "chips fly");
        const float top_before = p.bounds(d.debris_body(0)).max.y;
        run(p, 3.0f);
        d.update(3.0f);
        const float top_after = p.bounds(d.debris_body(0)).max.y;
        check(top_after < top_before - 0.9f, "and the piece falls (top " + num(top_before) + " -> " + num(top_after) + ")");
        check(d.is_debris(d.debris_body(0)) && !d.is_debris(RigidBody{}), "is_debris knows its bodies");
    }
    {
        VoxelWorld v;
        const BlockId stone = v.add_block({.name = "stone"});
        v.fill({0, 0, 0}, {10, 0, 10}, stone);
        v.fill({2, 1, 5}, {8, 5, 5}, stone); // a wall
        Physics3D p;
        VoxelDestruction d(v, p);
        d.carve(center_of({5, 2, 5}), 1.2f); // a hole through the middle
        check(d.debris_count() == 0 && v.get(5, 5, 5) == stone, "a hole in a wall that still stands on both sides: nothing falls");
    }
    {
        VoxelWorld v;
        const BlockId stone = v.add_block({.name = "stone"});
        build(v, stone);
        Physics3D p;
        VoxelDestruction d(v, p);
        d.max_piece = 10;
        d.carve(center_of({5, 3, 5}), 0.6f);
        check(d.debris_count() == 0 && v.get(5, 7, 5) == stone, "a loose group bigger than max_piece counts as held up");
    }
    {
        VoxelWorld v;
        const BlockId stone = v.add_block({.name = "stone"});
        v.fill({0, 0, 0}, {10, 0, 10}, stone);
        v.fill({5, 1, 5}, {5, 4, 5}, stone); // a short post: carve y=2, y 3..4 is 2 blocks
        Physics3D p;
        VoxelDestruction d(v, p);
        d.carve(center_of({5, 2, 5}), 0.6f);
        check(d.debris_count() == 0 && v.get(5, 3, 5) == 0 && v.get(5, 4, 5) == 0, "a loose bit smaller than min_piece crumbles away");
    }
    {
        VoxelWorld v;
        const BlockId stone = v.add_block({.name = "stone"});
        const BlockId hook = v.add_block({.name = "hook"});
        v.fill({0, 0, 0}, {10, 0, 10}, stone);
        v.set(5, 10, 5, hook);
        v.fill({5, 7, 5}, {5, 9, 5}, stone); // a chain hanging from it
        v.fill({6, 7, 5}, {8, 7, 5}, stone); // an arm at the bottom
        Physics3D p;
        VoxelDestruction d(v, p);
        d.anchors = {hook};
        d.carve(center_of({8, 7, 5}), 0.6f);
        check(d.debris_count() == 0 && v.get(6, 7, 5) == stone, "whatever hangs from an anchor block stays up");
        d.anchors.clear();
        d.carve(center_of({7, 7, 5}), 0.6f);
        check(d.debris_count() == 1 && d.debris_blocks(0).block_count() == 5 && v.get(5, 10, 5) == 0,
              "without the anchor the same thing falls, hook and all");
    }

    std::printf("debris\n");
    {
        VoxelWorld v;
        const BlockId stone = v.add_block({.name = "stone"});
        v.fill({-5, 0, -5}, {15, 0, 5}, stone);
        v.fill({0, 1, 0}, {0, 4, 0}, stone);  // a post...
        v.fill({0, 5, 0}, {11, 5, 0}, stone); // ...holding up a 12-long beam
        Physics3D p;
        VoxelDestruction d(v, p);
        d.carve(center_of({0, 2, 0}), 0.6f);
        check(d.debris_count() == 1 && d.debris_blocks(0).block_count() == 14, "cutting the post frees post top + beam (14 blocks)");
        const float mass14 = p.mass(d.debris_body(0));
        check(near(mass14, 14 * 800.0f, 1.0f), "debris mass is blocks x density (" + num(mass14) + " kg)");
        d.carve(center_of({6, 5, 0}), 0.6f); // cut the beam in the air
        check(d.debris_count() == 2, "carving the beam again splits it into two pieces");
        const int a = d.debris_blocks(0).block_count(), b = d.debris_blocks(1).block_count();
        check((a == 8 && b == 5) || (a == 5 && b == 8), "of 8 and 5 blocks (" + std::to_string(a) + ", " + std::to_string(b) + ")");
        check(near(p.mass(d.debris_body(0)), a * 800.0f, 1.0f) && near(p.mass(d.debris_body(1)), b * 800.0f, 1.0f),
              "each with the mass of what it has left");
        const int moved = d.explode(center_of({3, 5, 0}), 1.0f, 15.0f);
        check(moved > 0 && length(p.velocity(d.debris_body(0))) > 1.0f, "explode() carves and throws the pieces");
        run(p, 4.0f);
        d.update(4.0f);
        check(d.debris_count() >= 1, "pieces land on the ground slab and stay");
    }
    {
        VoxelWorld v;
        const BlockId stone = v.add_block({.name = "stone"});
        v.fill({0, 0, 0}, {10, 0, 10}, stone);
        for (int i = 0; i < 3; ++i) {
            v.fill({1 + 3 * i, 1, 1}, {1 + 3 * i, 1, 1}, stone);
            v.fill({1 + 3 * i, 2, 1}, {1 + 3 * i, 6, 1}, stone);
        }
        Physics3D p;
        VoxelDestruction d(v, p);
        d.max_debris = 2;
        for (int i = 0; i < 3; ++i) d.carve(center_of({1 + 3 * i, 1, 1}), 0.6f);
        check(d.debris_count() == 2, "past max_debris the oldest piece crumbles");
    }
    {
        VoxelWorld v;
        const BlockId stone = v.add_block({.name = "stone"});
        v.fill({0, 0, 0}, {2, 0, 2}, stone);
        v.fill({1, 1, 1}, {1, 3, 1}, stone);
        v.fill({1, 4, 1}, {12, 4, 1}, stone); // a beam sticking far out over nothing
        Physics3D p;
        VoxelDestruction d(v, p);
        d.carve(center_of({1, 4, 1}), 0.6f); // cut it off at the post
        check(d.debris_count() == 1, "a beam over the void breaks off");
        for (int i = 0; i < 8 * 60; ++i) {
            p.step(1.0f / 60.0f);
            d.update(1.0f / 60.0f);
        }
        check(d.debris_count() == 0, "and is removed once it has fallen out of the world");
    }

    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
