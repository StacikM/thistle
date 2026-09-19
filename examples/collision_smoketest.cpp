// Checks the 3D collision overlap/intersection math (Box3D/Sphere3D and the
// box3d_*/sphere3d_*/ray_box3d functions) against hand-computed expected
// results. Pure geometry functions, no App/window/GPU touched at all — even
// more headless than net_smoketest.
#include <thistle.hpp>
#include <cmath>
#include <cstdio>
using namespace thistle;

namespace {
int g_failures = 0;
void check(bool cond, const char* msg) {
    if (cond) { std::printf("  ok  %s\n", msg); }
    else { std::printf("  FAIL %s\n", msg); ++g_failures; }
}
} // namespace

int main() {
    // box3d_overlap
    check(box3d_overlap({{0, 0, 0}, {1, 1, 1}}, {{1.5f, 0, 0}, {1, 1, 1}}), "overlapping boxes (touching edges) detected");
    check(!box3d_overlap({{0, 0, 0}, {1, 1, 1}}, {{2.5f, 0, 0}, {1, 1, 1}}), "separated boxes on X not overlapping");
    check(box3d_overlap({{0, 0, 0}, {1, 1, 1}}, {{0, 0, 0}, {0.1f, 0.1f, 0.1f}}), "one box inside another overlaps");
    check(!box3d_overlap({{0, 0, 0}, {1, 1, 1}}, {{0, 3, 0}, {1, 1, 1}}), "separated on Y not overlapping");
    check(!box3d_overlap({{0, 0, 0}, {1, 1, 1}}, {{0, 0, 3}, {1, 1, 1}}), "separated on Z not overlapping");

    // box3d_contains_point
    check(box3d_contains_point({{0, 0, 0}, {1, 1, 1}}, {0.5f, 0.5f, -0.5f}), "point inside box contained");
    check(box3d_contains_point({{0, 0, 0}, {1, 1, 1}}, {1, 1, 1}), "point exactly on box corner contained (inclusive)");
    check(!box3d_contains_point({{0, 0, 0}, {1, 1, 1}}, {1.1f, 0, 0}), "point just outside box not contained");
    check(box3d_contains_point({{5, 2, -3}, {2, 2, 2}}, {5, 2, -3}), "point at an offset box's own center contained");

    // sphere3d_overlap
    check(sphere3d_overlap({{0, 0, 0}, 1.0f}, {{1.5f, 0, 0}, 1.0f}), "overlapping spheres detected");
    check(!sphere3d_overlap({{0, 0, 0}, 1.0f}, {{3, 0, 0}, 1.0f}), "separated spheres not overlapping");
    check(sphere3d_overlap({{0, 0, 0}, 1.0f}, {{2, 0, 0}, 1.0f}), "spheres exactly touching detected (inclusive)");

    // box3d_sphere3d_overlap
    check(box3d_sphere3d_overlap({{0, 0, 0}, {1, 1, 1}}, {{0, 0, 0}, 0.5f}), "sphere centered inside box overlaps");
    check(box3d_sphere3d_overlap({{0, 0, 0}, {1, 1, 1}}, {{2, 0, 0}, 1.5f}), "sphere reaching into box overlaps");
    check(!box3d_sphere3d_overlap({{0, 0, 0}, {1, 1, 1}}, {{5, 0, 0}, 1.0f}), "far sphere doesn't overlap box");
    check(box3d_sphere3d_overlap({{0, 0, 0}, {1, 1, 1}}, {{1, 1, 1}, 0.1f}), "small sphere touching box corner overlaps");

    // ray_box3d
    {
        float t = 0.0f;
        const bool hit = ray_box3d({-5, 0, 0}, {1, 0, 0}, {{0, 0, 0}, {1, 1, 1}}, t);
        check(hit, "ray toward box from outside hits");
        check(std::fabs(t - 4.0f) < 0.001f, "hit distance matches the hand-computed value (4.0)");
    }
    {
        float t = 0.0f;
        check(!ray_box3d({-5, 5, 0}, {1, 0, 0}, {{0, 0, 0}, {1, 1, 1}}, t), "ray offset past the box (parallel miss) reports no hit");
    }
    {
        float t = 0.0f;
        check(ray_box3d({0, 0, 0}, {1, 0, 0}, {{0, 0, 0}, {1, 1, 1}}, t), "ray starting inside the box hits");
    }
    {
        float t = 0.0f;
        check(!ray_box3d({5, 0, 0}, {1, 0, 0}, {{0, 0, 0}, {1, 1, 1}}, t), "ray pointing away from the box behind it misses");
    }

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
