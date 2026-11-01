// Recipe: timers. Three ways to say "after / every N seconds":
//   1. a countdown you decrement yourself (most things),
//   2. a repeating timer that also catches up after a slow frame,
//   3. a Node's action queue for "do this, wait, do that" sequences.
#include <thistle.hpp>
#include <string>
#include <vector>
using namespace thistle;

// Counts down; returns true on the frame it reaches 0, then restarts if it repeats.
struct Timer {
    float length = 1.0f, left = 1.0f;
    bool repeat = false, running = true;
    bool update(float dt) {
        if (!running) return false;
        left -= dt;
        if (left > 0.0f) return false;
        if (repeat) left += length; else running = false;
        return true;
    }
    void restart() { left = length; running = true; }
};

THISTLE_MAIN {
    App app{{.title = "Timers", .width = 900, .height = 480}};
    load_font("assets/font.ttf");

    Timer every_second{1.0f, 1.0f, true};
    Timer once{3.0f, 3.0f, false};
    int ticks = 0;
    std::string once_text = "waiting for 3 s...";

    // A Node with queued actions: move, wait, then run any code, then move again.
    Node box;
    box.pos = {100, 400};
    std::string sequence_status = "press Space to start the sequence";
    bool in_sequence = false;

    app.update([&](Frame f) {
        if (every_second.update(f.dt)) ++ticks;
        if (once.update(f.dt)) once_text = "done!";
        if (f.key_pressed(Key::R)) { once.restart(); once_text = "waiting for 3 s..."; }

        if (f.key_pressed(Key::Space) && !in_sequence) {
            in_sequence = true;
            sequence_status = "moving right...";
            box.move_to({700, 400}, 1.0f, Ease::OutCubic)
               .call([&] { sequence_status = "waiting half a second..."; })
               .delay(0.5f)
               .call([&] { sequence_status = "moving back..."; })
               .move_to({100, 400}, 0.8f, Ease::InOutQuad)
               .call([&] { sequence_status = "finished - press Space again"; in_sequence = false; });
        }
        box.update(f.dt);

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        f.text("every second: " + std::to_string(ticks), {40, 40}, {.size = 32});
        f.text("once:  " + once_text + "     (R restarts it)", {40, 100}, {.size = 32});
        f.text(sequence_status, {40, 200}, {.size = 32});
        f.rect({box.pos.x - 25, box.pos.y - 25}, {50, 50}, rgb(0.35f, 0.75f, 0.95f));
    });

    THISTLE_RUN(app);
}
