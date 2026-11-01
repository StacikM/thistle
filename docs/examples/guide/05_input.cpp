// Guide 5: input. "pressed" is true on the one frame a key goes down;
// "down" is true on every frame it's held. Space shows both counters.
// Click to drop a circle, scroll to change its size, and press the gamepad
// A button or Space to "jump" through the same action.
#include <thistle.hpp>
#include <algorithm>
#include <string>
#include <vector>
using namespace thistle;

struct Dot { vec2 pos; float radius; rgba color; };

THISTLE_MAIN {
    App app{{.title = "Input", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    int pressed_count = 0;
    int down_frames = 0;
    int jumps = 0;
    float brush = 24.0f;
    std::vector<Dot> dots;

    app.update([&](Frame f) {
        if (f.key_pressed(Key::Space)) ++pressed_count;   // once per press
        if (f.key_down(Key::Space)) ++down_frames;        // every frame while held

        // One "jump" action, however it was asked for. The rest of the game
        // only asks "did the player jump?", not "which device?".
        const bool jump = f.key_pressed(Key::Space) || f.key_pressed(Key::W) ||
                          (f.pad_connected() && f.pad_pressed(Pad::A));
        if (jump) ++jumps;

        brush = std::clamp(brush + f.mouse_scroll() * 2.0f, 6.0f, 80.0f);
        if (f.mouse_pressed(Mouse::Left)) {
            const float shade = 0.4f + 0.6f * static_cast<float>(dots.size() % 5) / 4.0f;
            dots.push_back({f.mouse(), brush, rgb(0.3f, shade, 0.9f)});
        }
        if (f.mouse_pressed(Mouse::Right) && !dots.empty()) dots.pop_back();

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        for (const Dot& d : dots) f.circle(d.pos, d.radius, d.color);
        f.circle_outline(f.mouse(), brush, white, 2.0f);

        f.text("Space pressed: " + std::to_string(pressed_count) + " times", {20, 20}, {.size = 26});
        f.text("Space held:    " + std::to_string(down_frames) + " frames", {20, 56}, {.size = 26});
        f.text("jump action:   " + std::to_string(jumps), {20, 92}, {.size = 26});
        f.text("click: drop   right-click: undo   scroll: size", Anchor::BottomLeft, {20, -20}, {.size = 22});
    });

    THISTLE_RUN(app);
}
