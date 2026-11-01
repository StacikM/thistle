// Guide 17: making things feel good. Click to burst particles. Space shakes
// the screen, F flashes it, H does a "hit stop" (a split-second freeze). The
// bouncing square uses a Tween; the curves at the bottom are the easing
// functions you can pick from.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <random>
using namespace thistle;

THISTLE_MAIN {
    App app{{.title = "Juice", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    Particles fx;
    Tween bounce(0.7f, Ease::OutBounce, /*loop=*/true);
    float shake = 0.0f, flash = 0.0f, freeze = 0.0f;
    std::mt19937 rng{3};

    app.update([&](Frame f) {
        // Hit stop: while frozen, time doesn't advance for the game.
        const float dt = freeze > 0.0f ? 0.0f : std::min(f.dt, 1.0f / 30.0f);
        freeze = std::max(0.0f, freeze - f.dt);

        if (f.mouse_pressed(Mouse::Left)) {
            fx.emit(f.mouse(), 40, {.spread = 320, .life = 0.7f, .size = 7, .color = coral, .gravity = 900});
        }
        if (f.key_pressed(Key::Space)) shake = 1.0f;
        if (f.key_pressed(Key::F)) flash = 1.0f;
        if (f.key_pressed(Key::H)) freeze = 0.12f;

        fx.update(dt);
        bounce.update(dt);
        shake = std::max(0.0f, shake - 3.0f * dt);
        flash = std::max(0.0f, flash - 4.0f * dt);

        f.clear(rgb(0.08f, 0.09f, 0.14f));

        // Screen shake: shift everything by a random amount that fades out.
        const auto rnd = [&] { return std::uniform_real_distribution<float>(-1, 1)(rng); };
        f.camera({rnd() * 16.0f * shake, rnd() * 16.0f * shake});

        // A tween drives 0..1; you decide what that 0..1 means (here, height).
        f.rect({200, 80 + 300 * bounce.t()}, {60, 60}, rgb(0.35f, 0.75f, 0.95f));
        fx.draw(f);

        // Plot each easing curve.
        const Ease curves[] = {Ease::Linear, Ease::InQuad, Ease::OutQuad, Ease::InOutQuad,
                               Ease::OutCubic, Ease::OutBack, Ease::OutElastic, Ease::OutBounce};
        for (int i = 0; i < 8; ++i) {
            const float x0 = 60.0f + 150.0f * i, y0 = 640.0f;
            f.rect({x0, y0 - 100}, {110, 100}, rgb(0.12f, 0.14f, 0.22f));
            for (int k = 0; k < 40; ++k) {
                const float t = k / 39.0f;
                f.circle({x0 + t * 110.0f, y0 - ease(curves[i], t) * 100.0f}, 2.0f, white, 8);
            }
        }

        f.camera({0, 0});
        if (flash > 0.0f) {
            f.blend(Blend::Additive);
            f.rect({0, 0}, {static_cast<float>(f.width), static_cast<float>(f.height)}, rgba{flash * 0.5f, flash * 0.5f, flash * 0.5f, 1});
            f.blend(Blend::Alpha);
        }
        f.text("click: particles   space: shake   F: flash   H: hit stop", Anchor::TopLeft, {20, 16}, {.size = 22});
    });

    THISTLE_RUN(app);
}
