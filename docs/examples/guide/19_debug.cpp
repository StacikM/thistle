// Guide 19: finding bugs. F1 shows an overlay (frame time, entity count, mouse
// position, every hitbox). F2 slows the game to a quarter speed so you can
// watch what happens. log_info/log_warn/log_error print to the terminal.
// Click to add bouncing balls.
#include <thistle.hpp>
#include <algorithm>
#include <string>
#include <vector>
using namespace thistle;

struct Ball { vec2 pos, vel; float radius; };

THISTLE_MAIN {
    App app{{.title = "Debugging", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    std::vector<Ball> balls;
    bool overlay = true;
    bool slow = false;
    float worst_frame = 0.0f;

    app.update([&](Frame f) {
        if (f.key_pressed(Key::F1)) overlay = !overlay;
        if (f.key_pressed(Key::F2)) { slow = !slow; log_info(slow ? "slow motion on" : "slow motion off"); }

        const float dt = std::min(f.dt, 1.0f / 30.0f) * (slow ? 0.25f : 1.0f);
        worst_frame = std::max(worst_frame * 0.995f, f.dt);

        if (f.mouse_pressed(Mouse::Left)) {
            balls.push_back({f.mouse(), {(balls.size() % 2 ? 240.0f : -240.0f), -200}, 18.0f + (balls.size() % 3) * 8});
            if (balls.size() > 200) log_warn("lots of balls: " + std::to_string(balls.size()));
        }
        for (Ball& b : balls) {
            b.vel.y += 900.0f * dt;
            b.pos += b.vel * dt;
            if (b.pos.y > f.height - b.radius) { b.pos.y = f.height - b.radius; b.vel.y *= -0.85f; }
            if (b.pos.x < b.radius || b.pos.x > f.width - b.radius) b.vel.x = -b.vel.x;
            b.pos.x = std::clamp(b.pos.x, b.radius, f.width - b.radius);
        }

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        for (const Ball& b : balls) f.circle(b.pos, b.radius, rgb(0.35f, 0.75f, 0.95f));

        if (overlay) {
            // Hitboxes drawn over the top, in a color nothing else uses.
            for (const Ball& b : balls) {
                f.rect(b.pos - vec2{b.radius, b.radius}, {b.radius * 2, b.radius * 2}, rgba{1, 0, 1, 0.25f});
                f.circle_outline(b.pos, b.radius, rgb(1, 0, 1), 1.0f);
            }
            const int fps = f.dt > 0 ? static_cast<int>(1.0f / f.dt + 0.5f) : 0;
            f.rect({10, 10}, {330, 118}, rgba{0, 0, 0, 0.6f});
            f.text(std::to_string(fps) + " fps   frame " + std::to_string(static_cast<int>(f.dt * 1000)) + " ms", {20, 14}, {.size = 22});
            f.text("worst frame " + std::to_string(static_cast<int>(worst_frame * 1000)) + " ms", {20, 42}, {.size = 22});
            f.text("balls " + std::to_string(balls.size()), {20, 70}, {.size = 22});
            f.text("mouse " + std::to_string(static_cast<int>(f.mouse().x)) + ", " + std::to_string(static_cast<int>(f.mouse().y)), {20, 98}, {.size = 22});
        }
        f.text("click: ball   F1: overlay   F2: slow motion", Anchor::BottomRight, {-16, -12}, {.size = 22});
    });

    THISTLE_RUN(app);
}
