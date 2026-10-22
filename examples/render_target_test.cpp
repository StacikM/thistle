// Checks render targets by eye, built so a mistake shows as a difference
// between two things that should match. Top row: a texture made from pixels
// in code, then a render target drawn with the same four-color pattern
// (red top-left, white top-right, green bottom-left, blue bottom-right) at
// full and at 1/4 resolution with pixelated = true (crisp blocks, not a
// blur). They must all look the same way up; on OpenGL the target is drawn
// upside down on purpose so it's stored like any other texture
// (three_render.cpp, three_targets_flip_y), and if that's wrong it shows here.
// Top right: a lit 3D scene with a shadow rendered into a target: sky on top,
// the red box on the left. Bottom left: a target cleared transparent, with
// soft-edged shapes and text, over a checkerboard: no dark rims, no boxes
// around the letters. Bottom right: the reference texture and the 2D target
// as materials on two cubes in the window's own 3D view: same way up.
#include <thistle.hpp>
#include <cstdio>
#include <vector>
using namespace thistle;
using namespace thistle::three;

namespace {
void quadrants(Frame& f, float size) {
    const float h = size * 0.5f;
    f.rect({0, 0}, {h, h}, rgba{0.9f, 0.15f, 0.15f, 1});
    f.rect({h, 0}, {h, h}, rgba{1, 1, 1, 1});
    f.rect({0, h}, {h, h}, rgba{0.15f, 0.8f, 0.2f, 1});
    f.rect({h, h}, {h, h}, rgba{0.2f, 0.3f, 0.95f, 1});
}

Texture reference_texture(int size) {
    std::vector<unsigned char> px(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool top = y < size / 2, left = x < size / 2;
            const unsigned char c[4][3] = {{230, 38, 38}, {255, 255, 255}, {38, 204, 51}, {51, 77, 242}};
            const int q = (top ? 0 : 2) + (left ? 0 : 1);
            unsigned char* p = &px[(static_cast<size_t>(y) * size + x) * 4];
            p[0] = c[q][0]; p[1] = c[q][1]; p[2] = c[q][2]; p[3] = 255;
        }
    }
    return make_texture(size, size, px.data());
}
} // namespace

int main() {
    App app{{.title = "Thistle render target test", .width = 1000, .height = 640}};
    Font font;
    for (const char* path : {"font.ttf",
#ifdef THISTLE_TOUCH_TEST_FONT
                             THISTLE_TOUCH_TEST_FONT,
#endif
         }) {
        if (std::FILE* fp = std::fopen(path, "rb")) { std::fclose(fp); font = load_font(path); break; }
    }

    const Texture reference = reference_texture(64);
    RenderTarget pattern = make_render_target(64, 64);
    RenderTarget pixels = make_render_target(16, 16, true);
    RenderTarget scene = make_render_target(320, 180);
    RenderTarget soft = make_render_target(300, 200);

    World scene_world;
    World window_world;
    const Model ground = make_model(box_mesh({8, 0.2f, 8}), Material{.color = rgb(0.55f, 0.6f, 0.5f)});
    const Model crate = make_model(box_mesh({1, 1, 1}), Material{.color = rgb(0.85f, 0.7f, 0.45f)});
    const Model marker = make_model(box_mesh({0.6f, 0.6f, 0.6f}), Material{.color = rgb(0.9f, 0.15f, 0.15f)});
    const Model ref_cube = make_model(box_mesh({1.4f, 1.4f, 1.4f}), Material{.texture = reference, .unlit = true});
    const Model target_cube = make_model(box_mesh({1.4f, 1.4f, 1.4f}), Material{.texture = pattern.texture, .unlit = true});

    app.update([&](Frame f) {
        f.clear(rgb(0.12f, 0.12f, 0.15f));

        f.begin_target(pattern);
        quadrants(f, 64);
        f.end_target();

        f.begin_target(pixels);
        quadrants(f, 16);
        f.end_target();

        f.begin_target(scene);
        f.clear(black);
        scene_world.draw(ground, {0, -0.1f, 0});
        scene_world.draw(crate, Transform{{0.6f, 0.5f, 0}, quat::euler(0, static_cast<float>(f.time) * 0.5f)});
        scene_world.draw(marker, {-1.8f, 0.3f, 0.5f});
        Camera cam;
        cam.position = {0, 3, 6};
        cam.look_at({0, 0.4f, 0});
        scene_world.render(f, cam);
        if (font.valid()) f.text("2D over 3D, in a target", {8, 8}, {.size = 16});
        f.end_target();

        f.begin_target(soft);
        f.circle({90, 100}, 70, rgba{0.2f, 0.9f, 0.4f, 0.5f}, 64);
        f.circle({200, 90}, 60, rgba{1, 1, 1, 1}, 64);
        f.rounded_rect({20, 150}, {260, 36}, 18, rgba{0.95f, 0.5f, 0.2f, 0.8f});
        if (font.valid()) f.text("Soft edges", {40, 154}, {.size = 26, .color = white});
        f.end_target();

        auto label = [&](const char* s, vec2 at) { if (font.valid()) f.text(s, at, {.size = 16, .color = rgba{0.8f, 0.8f, 0.85f, 1}}); };
        label("texture from pixels", {20, 12});
        f.sprite(reference, {20, 36}, {.size = {128, 128}});
        label("render target", {170, 12});
        f.sprite(pattern.texture, {170, 36}, {.size = {128, 128}});
        label("16x16, pixelated", {320, 12});
        f.sprite(pixels.texture, {320, 36}, {.size = {128, 128}});
        label("3D in a target", {480, 12});
        f.sprite(scene.texture, {480, 36}, {.size = {480, 270}});

        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 15; ++x)
                f.rect({20.0f + x * 20, 340.0f + y * 20}, {20, 20}, (x + y) % 2 ? rgb(0.75f, 0.75f, 0.75f) : rgb(0.45f, 0.45f, 0.45f));
        label("transparent target over a checkerboard", {20, 316});
        f.sprite(soft.texture, {20, 340});

        window_world.draw(ref_cube, Transform{{-1.1f, 0, 0}, quat::euler(0.3f, 0.6f)});
        window_world.draw(target_cube, Transform{{1.1f, 0, 0}, quat::euler(0.3f, 0.6f)});
        window_world.sky.visible = false;
        Camera wcam;
        wcam.position = {0, 0, 4.5f};
        window_world.render(f, wcam, Rect{{480, 340}, {480, 280}});
        label("as materials: reference, target", {480, 316});
    });
    return app.run();
}
