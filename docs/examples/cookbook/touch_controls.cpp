// Recipe: controls that work on a phone AND with a mouse. Touch arrives as the
// mouse, with ONE finger: there is no multi-touch, so a joystick and a jump
// button can't be held at the same time. The layout that works with one
// finger is "drag in the left half to move, tap in the right half to jump".
// The same code runs with the mouse on a desktop.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
using namespace thistle;

THISTLE_MAIN {
    App app{{.title = "Touch controls", .width = 1000, .height = 560}};
    load_font("assets/font.ttf");

    vec2 pos{300, 400}, vel{0, 0};
    bool dragging = false;
    vec2 drag_origin{0, 0};
    int jumps = 0;

    app.update([&](Frame f) {
        const float dt = std::min(f.dt, 1.0f / 30.0f);
        const float mid = f.width * 0.5f;

        // Left half: the first press sets the stick's center, dragging moves it.
        float move = 0.0f;
        if (f.mouse_pressed(Mouse::Left) && f.mouse().x < mid) { dragging = true; drag_origin = f.mouse(); }
        if (!f.mouse_down(Mouse::Left)) dragging = false;
        vec2 stick{0, 0};
        if (dragging) {
            stick = (f.mouse() - drag_origin) / 80.0f;                    // 80 px of drag = full speed
            if (stick.length() > 1.0f) stick = stick.normalized();
            move = stick.x;
        }
        // Keyboard still works, for testing on a computer.
        if (f.key_down(Key::Left) || f.key_down(Key::A)) move = -1.0f;
        if (f.key_down(Key::Right) || f.key_down(Key::D)) move = 1.0f;

        // Right half: a tap jumps. (A whole half of the screen is a big, forgiving target.)
        const bool jump = (f.mouse_pressed(Mouse::Left) && f.mouse().x >= mid) || f.key_pressed(Key::Space);
        const bool on_ground = pos.y >= 400.0f;
        if (jump && on_ground) { vel.y = -700.0f; ++jumps; }

        vel.y += 1800.0f * dt;
        pos.x = std::clamp(pos.x + move * 320.0f * dt, 30.0f, f.width - 30.0f);
        pos.y += vel.y * dt;
        if (pos.y > 400.0f) { pos.y = 400.0f; vel.y = 0.0f; }

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        f.rect({0, 440}, {static_cast<float>(f.width), 120}, rgb(0.2f, 0.22f, 0.32f));
        f.line({mid, 0}, {mid, static_cast<float>(f.height)}, rgb(0.2f, 0.22f, 0.32f), 2.0f);
        f.text("drag here to move", {40, 30}, {.size = 26, .color = rgb(0.55f, 0.6f, 0.8f)});
        f.text("tap here to jump (" + std::to_string(jumps) + ")", {mid + 40, 30}, {.size = 26, .color = rgb(0.55f, 0.6f, 0.8f)});
        f.rect(pos - vec2{30, 60}, {60, 60}, rgb(0.35f, 0.75f, 0.95f));

        if (dragging) {   // draw the stick so the player can see it
            f.circle(drag_origin, 60, rgba{1, 1, 1, 0.12f});
            f.circle(drag_origin + stick * 60.0f, 26, rgba{1, 1, 1, 0.45f});
        }
    });

    THISTLE_RUN(app);
}
