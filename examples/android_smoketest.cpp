// Graphical smoketest for the Android backend: opens a real window via
// NativeActivity and draws a rect every frame, forever. When built with
// test assets present (see examples/android/build_apk.sh) it also loads a
// texture, a font, and plays a sound — exercising the AAssetManager-backed
// asset loading (thistle_stbi_load / android_fons_add_font / the miniaudio
// VFS) instead of just the flat-color path. See docs/android.md.
#include <thistle.hpp>
using namespace thistle;

THISTLE_MAIN {
    App app{{.title = "AndroidSmoketest", .width = 1080, .height = 2280}};

    Texture tex = load_texture("assets/icon.png");
    Font font = load_font("assets/font.ttf");
    bool played_sound = false;

    app.update([&](Frame f) {
        f.clear(rgb(0.05f, 0.05f, 0.08f));
        f.rect({100, 100}, {200, 80}, rgb(0.3f, 0.6f, 0.9f));

        if (tex.valid()) {
            f.sprite(tex, {100, 250}, {.size = {200, 200}});
        }
        f.text(tex.valid() ? "texture: OK" : "texture: FAILED", {100, 480}, {.size = 40, .font = font});
        f.text(font.valid() ? "font: OK" : "font: FAILED", {100, 540}, {.size = 40, .font = font});

        if (!played_sound) {
            play_sound("assets/nav.wav");
            played_sound = true;
        }
        f.text("audio: play_sound() called (check logcat for miniaudio errors)", {100, 600},
               {.size = 32, .font = font, .max_width = 900});
    });

    THISTLE_RUN(app);
}
