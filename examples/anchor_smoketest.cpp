// Checks Anchor / anchored() against hand-computed positions. Pure rect math,
// so no App, window or GPU is touched.
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
bool near(vec2 a, vec2 b) { return std::fabs(a.x - b.x) < 0.001f && std::fabs(a.y - b.y) < 0.001f; }
} // namespace

int main() {
    const Rect screen{{0, 0}, {1000, 600}};

    // anchor_point: all nine spots on the screen
    check(near(anchor_point(Anchor::TopLeft, screen), {0, 0}), "TopLeft point");
    check(near(anchor_point(Anchor::Top, screen), {500, 0}), "Top point");
    check(near(anchor_point(Anchor::TopRight, screen), {1000, 0}), "TopRight point");
    check(near(anchor_point(Anchor::Left, screen), {0, 300}), "Left point");
    check(near(anchor_point(Anchor::Center, screen), {500, 300}), "Center point");
    check(near(anchor_point(Anchor::Right, screen), {1000, 300}), "Right point");
    check(near(anchor_point(Anchor::BottomLeft, screen), {0, 600}), "BottomLeft point");
    check(near(anchor_point(Anchor::Bottom, screen), {500, 600}), "Bottom point");
    check(near(anchor_point(Anchor::BottomRight, screen), {1000, 600}), "BottomRight point");

    // anchored(): the element's own matching point lands on the anchor
    const vec2 sz{200, 80};
    check(near(anchored(Anchor::Center, sz, {}, screen).pos, {400, 260}), "Center centers the rect");
    check(near(anchored(Anchor::TopLeft, sz, {}, screen).pos, {0, 0}), "TopLeft hugs the corner");
    check(near(anchored(Anchor::TopRight, sz, {}, screen).pos, {800, 0}), "TopRight hugs the corner");
    check(near(anchored(Anchor::BottomRight, sz, {}, screen).pos, {800, 520}), "BottomRight hugs the corner");
    check(near(anchored(Anchor::Bottom, sz, {}, screen).pos, {400, 520}), "Bottom centers on the edge");
    check(near(anchored(Anchor::Left, sz, {}, screen).pos, {0, 260}), "Left centers vertically");

    // offsets are plain pixels, y down
    check(near(anchored(Anchor::TopRight, sz, {-20, 20}, screen).pos, {780, 20}), "TopRight with an inward offset");
    check(near(anchored(Anchor::Center, sz, {10, -5}, screen).pos, {410, 255}), "Center with an offset");
    check(near(anchored(Anchor::Center, sz, {}, screen).size, sz), "size passes through");

    // an explicit pivot: top-left of the element on the screen center
    check(near(anchored(Anchor::Center, sz, {}, screen, Anchor::TopLeft).pos, {500, 300}), "pivot TopLeft at Center");
    check(near(anchored(Anchor::TopLeft, sz, {}, screen, Anchor::BottomRight).pos, {-200, -80}), "pivot BottomRight at TopLeft");

    // a parent that isn't the screen
    const Rect panel{{100, 50}, {400, 300}};
    check(near(anchored(Anchor::Center, {100, 40}, {}, panel).pos, {250, 180}), "centered in a panel");
    check(near(anchored(Anchor::BottomRight, {100, 40}, {-10, -10}, panel).pos, {390, 300}), "bottom-right of a panel, inset");

    // Frame::screen / Frame::anchored use the frame's size
    Frame f;
    f.width = 1000;
    f.height = 600;
    check(near(f.screen().size, {1000, 600}), "Frame::screen size");
    check(near(f.anchored(Anchor::Center, sz).pos, {400, 260}), "Frame::anchored centers");
    check(near(f.anchored(Anchor::TopRight, {64, 64}, {-20, 20}).pos, {916, 20}), "Frame::anchored top-right button");
    check(near(f.anchored(Anchor::Center, sz, {}, Anchor::TopLeft).pos, {500, 300}), "Frame::anchored with pivot");

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
