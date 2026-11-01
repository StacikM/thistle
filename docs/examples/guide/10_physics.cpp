// Guide 10: physics with Box2D. Left click drops a box, right click drops a
// ball, hold SPACE to blast everything upward. The green zone is a sensor:
// it detects things passing through without blocking them, and flashes.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
using namespace thistle;

struct Thing {
    Body body;
    bool is_ball;
    float size;   // box side, or ball radius
    rgba color;
};

THISTLE_MAIN {
    App app{{.title = "Physics", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    Physics phys({0, 980});   // gravity in pixels/second^2; +y is down
    phys.add_static_box({640, 700}, {1280, 40});    // floor
    phys.add_static_box({-20, 360}, {40, 800});     // left wall
    phys.add_static_box({1300, 360}, {40, 800});    // right wall
    phys.add_static_box({400, 520}, {300, 20});     // a shelf
    const Body zone = phys.add_sensor_box({900, 450}, {160, 160});

    std::vector<Thing> things;
    std::vector<Body> to_remove;
    float zone_flash = 0.0f;
    int zone_hits = 0;

    // Called inside step(). Don't change the world in here: just note what happened.
    phys.on_collision([&](Body a, Body b) {
        if (a == zone || b == zone) { zone_flash = 0.25f; ++zone_hits; }
    });

    app.update([&](Frame f) {
        if (f.mouse_pressed(Mouse::Left)) {
            const float side = 30.0f + 20.0f * ((things.size() % 3));
            things.push_back({phys.add_dynamic_box(f.mouse(), {side, side}, 1.0f, 0.4f, 0.1f), false, side,
                              rgb(0.9f, 0.55f, 0.3f)});
        }
        if (f.mouse_pressed(Mouse::Right)) {
            things.push_back({phys.add_dynamic_circle(f.mouse(), 20.0f, 1.0f, 0.3f, 0.6f), true, 20.0f,
                              rgb(0.35f, 0.7f, 0.95f)});
        }
        if (f.key_down(Key::Space)) {
            for (const Thing& t : things) phys.apply_impulse(t.body, {0, -9.0f});
        }

        phys.step(std::min(f.dt, 1.0f / 30.0f));
        zone_flash = std::max(0.0f, zone_flash - f.dt);

        // Anything that falls out of the world is removed, AFTER the step.
        for (const Thing& t : things) {
            if (phys.position(t.body).y > 1500.0f) to_remove.push_back(t.body);
        }
        for (const Body& b : to_remove) phys.remove(b);
        if (!to_remove.empty()) {
            things.erase(std::remove_if(things.begin(), things.end(),
                                        [&](const Thing& t) {
                                            return std::find(to_remove.begin(), to_remove.end(), t.body) != to_remove.end();
                                        }),
                         things.end());
            to_remove.clear();
        }

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        f.rect({820, 370}, {160, 160}, zone_flash > 0 ? rgb(0.4f, 0.9f, 0.5f) : rgb(0.15f, 0.3f, 0.2f));
        f.rect({250, 510}, {300, 20}, rgb(0.5f, 0.5f, 0.55f));
        f.rect({0, 680}, {1280, 40}, rgb(0.5f, 0.5f, 0.55f));

        for (const Thing& t : things) {
            const vec2 p = phys.position(t.body);
            if (t.is_ball) {
                f.circle(p, t.size, t.color);
                // A line from the center so you can see it spin.
                const float a = phys.angle(t.body);
                f.line(p, p + vec2{std::cos(a), std::sin(a)} * t.size, rgb(0.05f, 0.1f, 0.2f), 3.0f);
            } else {
                // Boxes rotate: draw them centered on (0,0) inside a transform.
                f.push_transform(p, phys.angle(t.body));
                f.rect({-t.size / 2, -t.size / 2}, {t.size, t.size}, t.color);
                f.pop_transform();
            }
        }

        f.text("left: box   right: ball   space: blast   things: " + std::to_string(things.size()) +
                   "   zone hits: " + std::to_string(zone_hits),
               Anchor::TopLeft, {20, 16}, {.size = 22});
    });

    THISTLE_RUN(app);
}
