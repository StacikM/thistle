// Guide 7: sprites and animation. There are no art files here: the sprite
// sheet is drawn into memory with make_texture(), so the program runs as-is.
// With real art you'd write load_texture("assets/hero.png") instead and
// everything after that line stays the same.
#include <thistle.hpp>
#include <cmath>
#include <vector>
using namespace thistle;

// A 4-frame sheet of 32x32 cells in one row: a ball that squashes and stretches.
static Texture make_ball_sheet() {
    const int cell = 32, frames = 4;
    const int w = cell * frames, h = cell;
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4, 0);
    for (int frame = 0; frame < frames; ++frame) {
        const float squash = 1.0f + 0.25f * std::sin(frame * 3.14159f / 2.0f);
        const float rx = 11.0f * squash, ry = 11.0f / squash;
        for (int y = 0; y < cell; ++y) {
            for (int x = 0; x < cell; ++x) {
                const float dx = (x - 15.5f) / rx, dy = (y - 15.5f) / ry;
                if (dx * dx + dy * dy <= 1.0f) {
                    unsigned char* p = &px[(static_cast<size_t>(y) * w + frame * cell + x) * 4];
                    p[0] = 255; p[1] = static_cast<unsigned char>(120 + y * 3); p[2] = 80; p[3] = 255;
                }
            }
        }
    }
    return make_texture(w, h, px.data());
}

// A 24x24 rounded panel, to stretch with sprite9().
static Texture make_panel() {
    const int n = 24;
    std::vector<unsigned char> px(static_cast<size_t>(n) * n * 4, 0);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float cx = std::fmax(8.0f, std::fmin(n - 9.0f, static_cast<float>(x)));
            const float cy = std::fmax(8.0f, std::fmin(n - 9.0f, static_cast<float>(y)));
            const float d = std::hypot(x - cx, y - cy);
            unsigned char* p = &px[(static_cast<size_t>(y) * n + x) * 4];
            if (d <= 8.0f) {
                const bool border = d > 6.0f;
                p[0] = border ? 240 : 40; p[1] = border ? 240 : 46; p[2] = border ? 255 : 70; p[3] = 255;
            }
        }
    }
    return make_texture(n, n, px.data());
}

THISTLE_MAIN {
    App app{{.title = "Sprites", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    Texture sheet = make_ball_sheet();
    Texture panel = make_panel();
    Anim bounce = Anim(sheet, 32, 32).frames(0, 4).fps(8);

    float x = 200.0f;
    float dir = 1.0f;

    app.update([&](Frame f) {
        x += dir * 220.0f * f.dt;
        if (x > f.width - 200.0f) dir = -1.0f;
        if (x < 200.0f) dir = 1.0f;

        f.clear(rgb(0.08f, 0.09f, 0.14f));

        // 1) The animated sprite. frame_at() turns a time into the source
        // rectangle on the sheet; flip_x mirrors it when walking left.
        f.sprite(sheet, {x, 380}, {.size = {128, 128}, .src = bounce.frame_at(f.time), .flip_x = dir < 0});

        // 2) The same sprite tinted, rotated around its center, and half see-through.
        f.sprite(sheet, {200, 120}, {.size = {96, 96}, .tint = rgba{0.4f, 0.8f, 1.0f, 0.6f},
                                     .rotation = static_cast<float>(f.time), .src = bounce.frame_at(0.0)});

        // 3) A 9-slice panel: corners stay crisp, only the middle stretches.
        f.sprite9(panel, {700, 60}, {420, 140}, 8.0f);
        f.sprite9(panel, {700, 230}, {200, 80}, 8.0f);
        f.text("sprite9: any size", {720, 80}, {.size = 26});
        f.text("same art", {722, 252}, {.size = 22});

        // 4) Blend modes: additive adds light instead of covering.
        f.blend(Blend::Additive);
        for (int i = 0; i < 3; ++i) {
            f.circle({920.0f + i * 60.0f, 600.0f}, 70.0f, rgba{i == 0 ? 0.6f : 0.1f, i == 1 ? 0.6f : 0.1f, i == 2 ? 0.6f : 0.1f, 1.0f});
        }
        f.blend(Blend::Alpha);
    });

    THISTLE_RUN(app);
}
