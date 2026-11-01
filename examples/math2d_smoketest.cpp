// Checks the small 2D helpers on vec2 and Rect against hand-computed results.
// Pure math, so no App, window or GPU is touched.
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
bool near(float a, float b) { return std::fabs(a - b) < 0.0001f; }
bool near(vec2 a, vec2 b) { return near(a.x, b.x) && near(a.y, b.y); }
} // namespace

int main() {
    // vec2
    check(near(vec2{3, 4}.length(), 5.0f), "length of (3,4) is 5");
    check(near(vec2{0, 0}.length(), 0.0f), "length of zero is 0");
    check(near(vec2{3, 4}.normalized(), {0.6f, 0.8f}), "normalized (3,4)");
    check(near(vec2{-10, 0}.normalized(), {-1, 0}), "normalized keeps direction");
    check(near(vec2{0, 0}.normalized(), {0, 0}), "normalized zero stays zero, not NaN");
    check(near(vec2{1, 2}.dot({3, 4}), 11.0f), "dot");
    check(near(vec2{1, 0}.dot({0, 1}), 0.0f), "perpendicular dot is 0");
    check(near(vec2{1, 1}.distance_to({4, 5}), 5.0f), "distance_to");
    check(near(vec2{6, 8} / 2.0f, {3, 4}), "divide by a scalar");
    check(near(-vec2{1, -2}, {-1, 2}), "unary minus");
    check(vec2{1, 2} == vec2{1, 2}, "equality");
    check(vec2{1, 2} != vec2{1, 3}, "inequality");
    check(near(vec2{1, 1}.normalized().length(), 1.0f), "a normalized vector has length 1");

    // Rect::overlaps
    const Rect a{{0, 0}, {10, 10}};
    check(a.overlaps({{5, 5}, {10, 10}}), "partly overlapping rects overlap");
    check(a.overlaps({{2, 2}, {2, 2}}), "a rect inside another overlaps");
    check(!a.overlaps({{20, 0}, {10, 10}}), "separated on x");
    check(!a.overlaps({{0, 20}, {10, 10}}), "separated on y");
    check(!a.overlaps({{10, 0}, {10, 10}}), "touching along an edge is not overlap");
    check(!a.overlaps({{10, 10}, {5, 5}}), "touching at a corner is not overlap");
    check(a.overlaps(a), "a rect overlaps itself");
    check(a.overlaps({{-5, -5}, {6, 6}}), "overlap from the top-left side");
    check(Rect{{5, 5}, {10, 10}}.overlaps(a) == a.overlaps({{5, 5}, {10, 10}}), "overlap is symmetric");

    // Rect::center / expanded
    check(near(a.center(), {5, 5}), "center");
    check(near(Rect{{10, 20}, {30, 40}}.center(), {25, 40}), "center of an offset rect");
    const Rect grown = a.expanded(2);
    check(near(grown.pos, {-2, -2}) && near(grown.size, {14, 14}), "expanded grows every side");
    const Rect shrunk = a.expanded(-1);
    check(near(shrunk.pos, {1, 1}) && near(shrunk.size, {8, 8}), "negative expanded shrinks");

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
