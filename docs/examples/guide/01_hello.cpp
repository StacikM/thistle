// Guide 1: the smallest useful Thistle program. A window, a moving circle,
// a rectangle, and some text.
#include <thistle.hpp>
#include <cmath>
using namespace thistle;

THISTLE_MAIN {
    App app{{.title = "Hello Thistle", .width = 1280, .height = 720}};

    // There is no built-in font. The first one you load becomes the default
    // for every f.text() call that doesn't name a font.
    load_font("assets/font.ttf");

    app.update([&](Frame f) {
        f.clear(rgb(0.08f, 0.09f, 0.14f));

        const float t = static_cast<float>(f.time);
        f.circle({640.0f + std::cos(t) * 200.0f, 360.0f + std::sin(t) * 120.0f}, 40.0f,
                 rgb(0.35f, 0.65f, 0.95f));
        f.rect({40, 40}, {200, 60}, rgb(0.85f, 0.4f, 0.3f));
        f.text("Hello, Thistle!", {40, 120}, {.size = 48});
    });

    THISTLE_RUN(app);
}
