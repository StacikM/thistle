// Guide 4: moving a player. Arrow keys or WASD, a gamepad stick, or touch /
// mouse (the player walks toward where you press). Diagonals are normalized
// so they aren't faster than straight lines.
#include <thistle.hpp>
#include <algorithm>
using namespace thistle;

THISTLE_MAIN {
    App app{{.title = "Move the player", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    vec2 pos{640, 360};
    const vec2 size{48, 48};
    const float speed = 320.0f;   // pixels per second

    app.update([&](Frame f) {
        vec2 dir{0, 0};
        if (f.key_down(Key::Left)  || f.key_down(Key::A)) dir.x -= 1;
        if (f.key_down(Key::Right) || f.key_down(Key::D)) dir.x += 1;
        if (f.key_down(Key::Up)    || f.key_down(Key::W)) dir.y -= 1;
        if (f.key_down(Key::Down)  || f.key_down(Key::S)) dir.y += 1;

        if (f.pad_connected()) {
            const vec2 stick = f.pad_left_stick();
            if (stick.length() > 0.2f) dir = stick;   // ignore a stick that doesn't rest at exactly 0
        }

        // Press (mouse or finger) to walk toward that point.
        if (f.mouse_down(Mouse::Left)) {
            const vec2 to_target = f.mouse() - (pos + size * 0.5f);
            if (to_target.length() > 6.0f) dir = to_target;
        }

        // A diagonal key press is (1, 1), which is longer than 1. Cap it at 1
        // but let a half-tilted stick stay slow.
        if (dir.length() > 1.0f) dir = dir.normalized();

        pos += dir * (speed * f.dt);

        // Keep the player on screen.
        pos.x = std::clamp(pos.x, 0.0f, f.width - size.x);
        pos.y = std::clamp(pos.y, 0.0f, f.height - size.y);

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        f.rect(pos, size, rgb(0.35f, 0.65f, 0.95f));
        f.text("arrows / WASD / stick / hold the mouse", Anchor::TopLeft, {20, 20}, {.size = 22});
    });

    THISTLE_RUN(app);
}
