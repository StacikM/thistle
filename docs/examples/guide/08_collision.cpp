// Guide 8: collision. Drag the white rectangle (hold the mouse button) around:
// everything it touches turns red. Four kinds of test are on show: rect vs
// rect, circle vs circle, circle vs rect, and point vs rect. The last row
// pushes the dragged rect out of a solid wall.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <string>
using namespace thistle;

// Circle vs circle: they touch when the distance between centers is less than the radii added up.
static bool circles_overlap(vec2 a, float ra, vec2 b, float rb) {
    return a.distance_to(b) < ra + rb;
}

// Circle vs rect: find the point of the rect closest to the circle's center, and see how far away it is.
static bool circle_hits_rect(vec2 center, float radius, const Rect& r) {
    const vec2 closest{std::clamp(center.x, r.pos.x, r.pos.x + r.size.x),
                       std::clamp(center.y, r.pos.y, r.pos.y + r.size.y)};
    return center.distance_to(closest) < radius;
}

// Moves `mover` out of `wall` by the smallest amount, along whichever axis needs the least.
static void push_out(Rect& mover, const Rect& wall) {
    if (!mover.overlaps(wall)) return;
    const float push_left = (mover.pos.x + mover.size.x) - wall.pos.x;   // how far to go left
    const float push_right = (wall.pos.x + wall.size.x) - mover.pos.x;   // how far to go right
    const float push_up = (mover.pos.y + mover.size.y) - wall.pos.y;
    const float push_down = (wall.pos.y + wall.size.y) - mover.pos.y;
    const float smallest = std::min({push_left, push_right, push_up, push_down});
    if (smallest == push_left) mover.pos.x -= push_left;
    else if (smallest == push_right) mover.pos.x += push_right;
    else if (smallest == push_up) mover.pos.y -= push_up;
    else mover.pos.y += push_down;
}

THISTLE_MAIN {
    App app{{.title = "Collision", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    Rect player{{120, 120}, {90, 60}};
    const Rect box{{420, 100}, {140, 100}};
    const vec2 ball{760, 150};
    const float ball_radius = 55.0f;
    const Rect wall{{300, 450}, {40, 200}};
    const vec2 point_pos{1000, 150};   // we test the dragged rect's center against this
    const Rect target{{930, 250}, {160, 120}};
    bool holding = false;
    vec2 grab_offset{0, 0};

    app.update([&](Frame f) {
        // Dragging: remember where inside the rect we grabbed it.
        if (f.mouse_pressed(Mouse::Left) && player.contains(f.mouse())) {
            holding = true;
            grab_offset = f.mouse() - player.pos;
        }
        if (!f.mouse_down(Mouse::Left)) holding = false;
        if (holding) player.pos = f.mouse() - grab_offset;

        push_out(player, wall);

        const vec2 center = player.center();
        const bool hit_box = player.overlaps(box);
        const bool hit_ball = circle_hits_rect(ball, ball_radius, player);
        const bool inside_target = target.contains(center);

        const rgba normal = rgb(0.3f, 0.55f, 0.9f);
        const rgba hot = rgb(0.95f, 0.3f, 0.3f);

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        f.rect(box.pos, box.size, hit_box ? hot : normal);
        f.text("rect vs rect", {box.pos.x, box.pos.y + box.size.y + 8}, {.size = 20});
        f.circle(ball, ball_radius, hit_ball ? hot : normal);
        f.text("circle vs rect", {ball.x - 60, ball.y + ball_radius + 8}, {.size = 20});
        f.rect(target.pos, target.size, inside_target ? hot : normal);
        f.text("rect contains the center", {target.pos.x - 20, target.pos.y + target.size.y + 8}, {.size = 20});
        f.rect(wall.pos, wall.size, rgb(0.5f, 0.5f, 0.55f));
        f.text("solid: the rect can't enter", {wall.pos.x + 50, wall.pos.y + 90}, {.size = 20});

        f.rect(player.pos, player.size, white);
        f.circle(center, 4, rgb(0.9f, 0.2f, 0.2f));

        // A circle-vs-circle example that follows the mouse.
        const bool circles_touch = circles_overlap(f.mouse(), 24, {760, 450}, 70);
        f.circle({760, 450}, 70, circles_touch ? hot : normal);
        f.circle_outline(f.mouse(), 24, white);
        f.text("circle vs circle (the mouse)", {660, 535}, {.size = 20});
        (void)point_pos;
    });

    THISTLE_RUN(app);
}
