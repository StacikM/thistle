// Guide 2: where things go. Origin is the top-left corner, x grows to the
// right, y grows DOWN. Move the mouse to read coordinates; the three
// overlapping rectangles show that later draw calls land on top.
#include <thistle.hpp>
#include <string>
using namespace thistle;

THISTLE_MAIN {
    App app{{.title = "Coordinates", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    app.update([&](Frame f) {
        f.clear(rgb(0.08f, 0.09f, 0.14f));

        // A grid every 100 pixels, so you can see the numbers.
        for (int x = 0; x <= f.width; x += 100) {
            f.line({static_cast<float>(x), 0}, {static_cast<float>(x), static_cast<float>(f.height)},
                   rgb(0.2f, 0.22f, 0.3f), 1.0f);
            f.text(std::to_string(x), {static_cast<float>(x) + 4, 4}, {.size = 16, .color = rgb(0.5f, 0.55f, 0.7f)});
        }
        for (int y = 100; y <= f.height; y += 100) {
            f.line({0, static_cast<float>(y)}, {static_cast<float>(f.width), static_cast<float>(y)},
                   rgb(0.2f, 0.22f, 0.3f), 1.0f);
            f.text(std::to_string(y), {4, static_cast<float>(y) + 4}, {.size = 16, .color = rgb(0.5f, 0.55f, 0.7f)});
        }

        // Draw order: whatever you draw last is on top.
        f.rect({300, 200}, {200, 140}, rgb(0.85f, 0.4f, 0.3f));
        f.rect({400, 260}, {200, 140}, rgb(0.3f, 0.7f, 0.5f));
        f.rect({350, 320}, {200, 140}, rgb(0.35f, 0.55f, 0.95f));

        // The mouse is in the same pixel coordinates as everything you draw.
        const vec2 m = f.mouse();
        f.circle(m, 8, white);
        f.text("mouse: " + std::to_string(static_cast<int>(m.x)) + ", " + std::to_string(static_cast<int>(m.y)),
               {m.x + 14, m.y + 14}, {.size = 22});

        // Anchored text: pinned to a corner no matter how big the window is.
        f.text("window: " + std::to_string(f.width) + " x " + std::to_string(f.height),
               Anchor::BottomRight, {-20, -20}, {.size = 22});
    });

    THISTLE_RUN(app);
}
