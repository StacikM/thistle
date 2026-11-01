// Guide 16: saving. Press Space to add to the score, S to bank it as the best
// score, R to reset. The best score and the "sound" setting come back the next
// time you run the program. save:: is a small key/value file in the right
// place for each platform.
#include <thistle.hpp>
#include <string>
using namespace thistle;

THISTLE_MAIN {
    // save_name decides the save folder. Set it, so changing the title later
    // (for example "My Game v1.2") doesn't make every player start from zero.
    App app{{.title = "Save data", .width = 900, .height = 420, .save_name = "thistle-doc-save"}};
    load_font("assets/font.ttf");

    int score = 0;
    int best = save::get_int("best", 0);                  // 0 if it was never saved
    bool sound = save::get("sound", "on") == "on";        // strings work too

    app.update([&](Frame f) {
        if (f.key_pressed(Key::Space)) ++score;
        if (f.key_pressed(Key::S) && score > best) { best = score; save::set_int("best", best); }
        if (f.key_pressed(Key::T)) { sound = !sound; save::set("sound", sound ? "on" : "off"); }
        if (f.key_pressed(Key::R)) { score = 0; best = 0; save::remove("best"); }

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        f.text("score " + std::to_string(score) + "      best " + std::to_string(best), {40, 40}, {.size = 40});
        f.text("Space: +1    S: save as best    T: sound " + std::string(sound ? "on" : "off") + "    R: reset", {40, 120}, {.size = 24});
        f.text("saved in: " + save::path(), {40, 340}, {.size = 18, .color = rgb(0.6f, 0.65f, 0.8f), .max_width = 820});
    });

    THISTLE_RUN(app);
}
