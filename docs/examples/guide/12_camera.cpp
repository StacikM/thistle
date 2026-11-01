// Guide 12: a camera. The world is much bigger than the window. WASD / arrows
// move the player; the camera eases after it. Mouse wheel zooms, SPACE shakes
// the screen. The faint shapes behind the world are parallax layers: they move
// slower than the world, which reads as distance. The ring follows the mouse
// IN THE WORLD, not on screen, so the maths for clicking things is here too.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <string>
using namespace thistle;

static const vec2 kWorld{3200, 1800};

THISTLE_MAIN {
    App app{{.title = "Camera", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    vec2 player{400, 400};
    vec2 cam = player;          // the world point shown at the center of the screen
    float zoom = 1.0f;
    float shake = 0.0f;         // 0..1, decays by itself
    std::mt19937 rng{7};
    auto jitter = [&] { return std::uniform_real_distribution<float>(-1.0f, 1.0f)(rng); };

    app.update([&](Frame f) {
        const float dt = std::min(f.dt, 1.0f / 30.0f);

        // --- move the player ------------------------------------------------
        vec2 dir{0, 0};
        if (f.key_down(Key::Left)  || f.key_down(Key::A)) dir.x -= 1;
        if (f.key_down(Key::Right) || f.key_down(Key::D)) dir.x += 1;
        if (f.key_down(Key::Up)    || f.key_down(Key::W)) dir.y -= 1;
        if (f.key_down(Key::Down)  || f.key_down(Key::S)) dir.y += 1;
        player += dir.normalized() * (420.0f * dt);
        player.x = std::clamp(player.x, 20.0f, kWorld.x - 20.0f);
        player.y = std::clamp(player.y, 20.0f, kWorld.y - 20.0f);

        // --- camera: ease toward the player, stay inside the world ------------
        // 1 - exp(-k*dt) is "move this fraction of the way each frame", but
        // correct at any frame rate (a plain 0.1f would follow slower at 30 fps).
        const float follow = 1.0f - std::exp(-6.0f * dt);
        cam += (player - cam) * follow;
        zoom = std::clamp(zoom + f.mouse_scroll() * 0.05f, 0.4f, 2.5f);
        const vec2 half{f.width / (2 * zoom), f.height / (2 * zoom)};   // half the visible world, in world pixels
        cam.x = std::clamp(cam.x, std::min(half.x, kWorld.x / 2), std::max(kWorld.x - half.x, kWorld.x / 2));
        cam.y = std::clamp(cam.y, std::min(half.y, kWorld.y / 2), std::max(kWorld.y - half.y, kWorld.y / 2));

        if (f.key_pressed(Key::Space)) shake = 1.0f;
        shake = std::max(0.0f, shake - 2.5f * dt);
        const vec2 shake_offset{jitter() * 14.0f * shake, jitter() * 14.0f * shake};

        const vec2 screen_center{f.width * 0.5f, f.height * 0.5f};
        // The mouse, converted from screen pixels to world pixels.
        const vec2 mouse_world = cam + (f.mouse() - screen_center) / zoom;

        f.clear(rgb(0.06f, 0.07f, 0.12f));

        // --- parallax layers: only translated, drawn before the world -----------
        for (int layer = 0; layer < 3; ++layer) {
            const float k = 0.15f + 0.2f * layer;   // 0 = glued to the screen, 1 = moves with the world
            f.camera({-(cam.x - screen_center.x) * k, -(cam.y - screen_center.y) * k});
            for (int i = 0; i < 14; ++i) {
                const float x = 200.0f * i + 90.0f * layer, y = 130.0f * ((i * 7 + layer * 3) % 9) + 40.0f;
                f.circle({x, y}, 18.0f + 10.0f * layer, rgba{0.25f, 0.3f, 0.5f, 0.10f + 0.05f * layer});
            }
        }

        // --- the world: one transform does position, zoom and shake -------------
        // screen = center + zoom * (world - cam)
        f.push_transform(screen_center - cam * zoom + shake_offset, 0.0f, {zoom, zoom});
        f.rect({0, 0}, kWorld, rgb(0.1f, 0.12f, 0.2f));
        for (int x = 0; x <= static_cast<int>(kWorld.x); x += 200) f.line({static_cast<float>(x), 0}, {static_cast<float>(x), kWorld.y}, rgb(0.18f, 0.2f, 0.32f), 2.0f);
        for (int y = 0; y <= static_cast<int>(kWorld.y); y += 200) f.line({0, static_cast<float>(y)}, {kWorld.x, static_cast<float>(y)}, rgb(0.18f, 0.2f, 0.32f), 2.0f);
        for (int i = 0; i < 12; ++i) {
            f.rect({300.0f + 250.0f * i, 300.0f + 110.0f * (i % 5)}, {80, 80}, rgb(0.5f, 0.35f + 0.04f * i, 0.3f));
        }
        f.circle_outline(mouse_world, 24, white, 3.0f);
        f.rect(player - vec2{20, 20}, {40, 40}, rgb(0.35f, 0.75f, 0.95f));
        f.pop_transform();

        // --- HUD: back to plain screen space, so none of the above affects it ---
        f.camera({0, 0});
        f.text("player (" + std::to_string(static_cast<int>(player.x)) + ", " + std::to_string(static_cast<int>(player.y)) +
                   ")   mouse in world (" + std::to_string(static_cast<int>(mouse_world.x)) + ", " +
                   std::to_string(static_cast<int>(mouse_world.y)) + ")",
               Anchor::TopLeft, {20, 16}, {.size = 22});
        f.text("wheel: zoom   space: shake", Anchor::BottomLeft, {20, -16}, {.size = 22});
    });

    THISTLE_RUN(app);
}
