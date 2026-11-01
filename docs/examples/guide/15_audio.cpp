// Guide 15: sound. 1, 2, 3 play sound effects, M starts and stops the music,
// and the sliders set the volumes. Sound effects are fire-and-forget: you can
// play the same one ten times at once and they simply overlap.
#include <thistle.hpp>
using namespace thistle;

THISTLE_MAIN {
    App app{{.title = "Audio", .width = 900, .height = 420}};
    load_font("assets/font.ttf");

    bool music_playing = false;
    float master = 1.0f, music = 0.6f, sfx = 1.0f;

    app.update([&](Frame f) {
        if (f.key_pressed(Key::Num1)) play_sound("assets/jump.wav");
        if (f.key_pressed(Key::Num2)) play_sound("assets/coin.wav");
        if (f.key_pressed(Key::Num3)) play_sound("assets/hit.wav");
        if (f.key_pressed(Key::M)) {
            music_playing = !music_playing;
            if (music_playing) play_music("assets/music.wav", 1.0f);   // loops until stopped
            else stop_music();
        }

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        f.text("1 jump   2 coin   3 hit   M music " + std::string(music_playing ? "(on)" : "(off)"), {40, 30}, {.size = 28});

        // Each slider returns the new value; assign it back, then tell the engine.
        const Rect row{{40, 120}, {500, 28}};
        master = f.slider(master, 0, 1, row);
        music = f.slider(music, 0, 1, {{40, 190}, row.size});
        sfx = f.slider(sfx, 0, 1, {{40, 260}, row.size});
        set_master_volume(master);
        set_music_volume(music);
        set_sfx_volume(sfx);
        f.text("master", {570, 118}, {.size = 26});
        f.text("music", {570, 188}, {.size = 26});
        f.text("sound effects", {570, 258}, {.size = 26});
    });

    THISTLE_RUN(app);
}
