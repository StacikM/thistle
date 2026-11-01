// Guide 3: delta time. Both squares try to move at the same speed. The top
// one moves by "200 pixels per second * the time the last frame took", the
// bottom one by "3 pixels per frame". Hold SPACE to make every frame slow
// (about 20 fps) and watch only the bottom one get slower.
#include <thistle.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
using namespace thistle;

THISTLE_MAIN {
    App app{{.title = "Delta time", .width = 1280, .height = 400}};
    load_font("assets/font.ttf");

    float good_x = 0.0f;
    float bad_x = 0.0f;

    app.update([&](Frame f) {
        if (f.key_down(Key::Space)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        good_x += 200.0f * f.dt;   // pixels per second * seconds
        bad_x += 3.0f;             // pixels per frame: speed depends on the frame rate

        if (good_x > f.width) good_x = 0.0f;
        if (bad_x > f.width) bad_x = 0.0f;

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        f.rect({good_x, 100}, {40, 40}, rgb(0.3f, 0.7f, 0.5f));
        f.text("200 * f.dt", {20, 60}, {.size = 24});
        f.rect({bad_x, 250}, {40, 40}, rgb(0.85f, 0.4f, 0.3f));
        f.text("+= 3 every frame", {20, 210}, {.size = 24});

        const int fps = f.dt > 0.0f ? static_cast<int>(std::lround(1.0f / f.dt)) : 0;
        f.text(std::to_string(fps) + " fps  (hold SPACE to slow it down)", Anchor::BottomRight, {-20, -20},
               {.size = 22});
    });

    THISTLE_RUN(app);
}
