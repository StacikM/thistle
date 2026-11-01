// Checks the 2D physics wrapper (Box2D) with real simulation: falling, resting,
// raycasts, sensors, moving platforms, impulses, and removing bodies. Headless:
// Physics needs no App, window or GPU.
#include <thistle.hpp>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace thistle;

namespace {
int g_failures = 0;
void check(bool cond, const char* msg) {
    if (cond) { std::printf("  ok  %s\n", msg); }
    else { std::printf("  FAIL %s\n", msg); ++g_failures; }
}
void run(Physics& p, int steps) { for (int i = 0; i < steps; ++i) p.step(1.0f / 60.0f); }
} // namespace

int main() {
    // A box dropped onto a floor comes to rest on top of it.
    {
        Physics p({0, 980});
        const Body floor = p.add_static_box({400, 500}, {800, 40});   // top surface at y = 480
        const Body box = p.add_dynamic_box({400, 100}, {40, 40});
        run(p, 240);
        const vec2 pos = p.position(box);
        check(std::fabs(pos.y - 460.0f) < 2.0f, "a dropped box rests on the floor (center 20 px above the top surface)");
        check(std::fabs(pos.x - 400.0f) < 1.0f, "and didn't drift sideways");
        check(p.velocity(box).length() < 5.0f, "and has stopped moving");
        check(p.touching(box, floor) && p.touching(floor, box), "touching() sees the contact, in both orders");
        check(p.position(floor).y == 500.0f, "a static body doesn't move");
    }

    // Gravity direction and strength: +y is down, in pixels per second squared.
    {
        Physics p({0, 980});
        const Body ball = p.add_dynamic_circle({100, 0}, 10);
        run(p, 60);   // one second of falling
        const float y = p.position(ball).y;
        check(y > 400.0f && y < 520.0f, "one second of 980 px/s^2 gravity falls about 490 px");
        check(p.velocity(ball).y > 900.0f && p.velocity(ball).y < 1000.0f, "and is moving about 980 px/s downward");
    }
    {
        Physics p({0, 0});
        const Body ball = p.add_dynamic_circle({100, 100}, 10);
        run(p, 120);
        check(p.position(ball).y == 100.0f, "with zero gravity nothing falls (top-down games)");
    }

    // Raycasts.
    {
        Physics p({0, 980});
        const Body floor = p.add_static_box({400, 500}, {800, 40});
        const RayHit above = p.raycast({400, 300}, {400, 600});
        check(above.hit && above.body == floor, "a ray from open air down onto the floor hits it");
        check(std::fabs(above.point.y - 480.0f) < 0.5f, "at the top surface");
        check(std::fabs(above.fraction - (180.0f / 300.0f)) < 0.01f, "fraction is how far along the ray (0..1)");
        check(above.normal.y < -0.9f, "the surface normal points up (negative y)");
        check(!p.raycast({100, 300}, {100, 400}).hit, "a ray that stops short hits nothing");

        // The player's own body: a ray that STARTS inside a shape ignores that shape.
        const Body player = p.add_dynamic_box({400, 440}, {40, 40});   // sits on the floor
        run(p, 120);
        const vec2 c = p.position(player);
        const RayHit from_center = p.raycast(c, {c.x, c.y + 60.0f});
        check(from_center.hit && from_center.body == floor, "a ray from the middle of the player ignores the player and hits the floor under it");
        const RayHit inside_floor = p.raycast({400, 500}, {400, 700});
        check(!inside_floor.hit, "a ray that starts inside the floor doesn't hit the floor (so cast from outside what you test)");
    }

    // Sensors fire on_collision but don't push.
    {
        Physics p({0, 980});
        p.add_static_box({400, 500}, {800, 40});
        const Body zone = p.add_sensor_box({400, 300}, {100, 100});
        const Body ball = p.add_dynamic_circle({400, 100}, 15);
        std::vector<std::pair<int, int>> events;
        p.on_collision([&](Body a, Body b) { events.push_back({a.id, b.id}); });
        run(p, 120);
        bool saw_zone = false;
        for (auto& e : events) if ((e.first == zone.id && e.second == ball.id) || (e.second == zone.id && e.first == ball.id)) saw_zone = true;
        check(saw_zone, "a body falling through a sensor triggers on_collision with both bodies");
        check(p.position(ball).y > 440.0f, "and the sensor didn't stop it: it fell through to the floor");
    }

    // A kinematic platform carries what's resting on it.
    {
        Physics p({0, 980});
        const Body platform = p.add_kinematic_box({200, 400}, {200, 20});
        const Body box = p.add_dynamic_box({200, 370}, {30, 30}, 1.0f, 0.9f);
        p.set_velocity(platform, {100, 0});
        run(p, 90);   // 1.5 s
        check(p.position(platform).x > 340.0f, "the kinematic platform moves at the velocity you gave it");
        check(p.position(box).x > 300.0f, "and carries a box resting on it");
        check(std::fabs(p.position(box).y - 375.0f) < 3.0f, "which stays on top of it");
    }

    // Impulses, fixed rotation, teleporting.
    {
        Physics p({0, 0});
        const Body ball = p.add_dynamic_circle({100, 100}, 10);
        p.apply_impulse(ball, {50, 0});
        check(p.velocity(ball).x > 0.0f, "an impulse changes velocity");
        p.set_velocity(ball, {0, 0});
        p.set_angular_velocity(ball, 5.0f);
        run(p, 30);
        check(std::fabs(p.angle(ball)) > 1.0f, "angular velocity spins a body");
        p.set_velocity(ball, {200, 0});
        p.reset_body(ball, {300, 300});
        check(p.position(ball).x == 300.0f && p.position(ball).y == 300.0f && p.velocity(ball).length() == 0.0f,
              "reset_body teleports and stops the body");
    }

    // Fixed rotation: a ball rolls across a floor; one with rotation fixed slides without turning.
    {
        Physics p({0, 980});
        p.add_static_box({600, 520}, {1200, 40});   // top surface at y = 500
        const Body rolling = p.add_dynamic_circle({100, 470}, 20, 1.0f, 0.8f);
        const Body fixed = p.add_dynamic_circle({700, 470}, 20, 1.0f, 0.8f);
        p.set_fixed_rotation(fixed, true);
        run(p, 30);
        p.set_velocity(rolling, {200, 0});
        p.set_velocity(fixed, {200, 0});
        run(p, 60);
        check(std::fabs(p.angle(rolling)) > 1.0f, "a free ball rolls (its angle changes)");
        check(std::fabs(p.angle(fixed)) < 0.01f, "a ball with fixed rotation doesn't turn");
        check(p.position(fixed).x > 710.0f, "but it still moves");
    }

    // Removing bodies.
    {
        Physics p({0, 980});
        const Body floor = p.add_static_box({400, 500}, {800, 40});
        const Body a = p.add_dynamic_box({300, 440}, {40, 40});
        const Body b = p.add_dynamic_box({500, 440}, {40, 40});
        run(p, 60);
        p.remove(a);
        check(p.position(a).x == 0.0f && p.position(a).y == 0.0f, "a removed body reads as {0, 0}");
        check(!p.touching(a, floor), "and touches nothing");
        p.set_velocity(a, {100, 0});   // must be harmless
        p.apply_impulse(a, {1, 1});
        p.remove(a);                   // removing twice must be harmless too
        run(p, 30);
        check(std::fabs(p.position(b).x - 500.0f) < 1.0f, "other bodies keep working, and their handles stay valid");
        const RayHit through = p.raycast({300, 300}, {300, 600});
        check(through.hit && through.body == floor, "the removed body no longer blocks rays");
        const Body c = p.add_dynamic_box({300, 100}, {40, 40});
        check(!(c == a), "a new body doesn't reuse a removed body's handle");
        run(p, 120);
        check(std::fabs(p.position(c).y - 460.0f) < 2.0f, "and works normally");
    }

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
