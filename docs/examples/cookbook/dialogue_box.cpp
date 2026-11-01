// Recipe: a dialogue box with a typewriter effect. Enter / click / Space
// shows the rest of the line instantly, or goes to the next line. At the end
// the choices appear, picked with the mouse or the Up/Down and Enter keys.
#include <thistle.hpp>
#include <algorithm>
#include <string>
#include <vector>
using namespace thistle;

struct Line { std::string speaker, text; };

THISTLE_MAIN {
    App app{{.title = "Dialogue", .width = 1100, .height = 600}};
    load_font("assets/font.ttf");

    const std::vector<Line> script = {
        {"Mara", "You made it. I wasn't sure the bridge would hold."},
        {"Mara", "The way north is closed, but there is a path through the old mine, if you aren't afraid of the dark."},
        {"Mara", "So. Which will it be?"},
    };
    const std::vector<std::string> choices = {"Take the mine path", "Wait for morning", "Ask about the bridge"};

    size_t line = 0;
    float shown = 0.0f;          // how many characters are visible (a float so it advances smoothly)
    int selected = 0;
    std::string result;

    app.update([&](Frame f) {
        const Line& current = script[line];
        const bool line_done = shown >= static_cast<float>(current.text.size());
        const bool at_end = line + 1 == script.size();

        shown = std::min(shown + 45.0f * f.dt, static_cast<float>(current.text.size()));   // 45 characters a second

        const bool advance = f.key_pressed(Key::Enter) || f.key_pressed(Key::Space) || f.mouse_pressed(Mouse::Left);
        if (advance && result.empty()) {
            if (!line_done) shown = static_cast<float>(current.text.size());         // first press: finish the line
            else if (!at_end) { ++line; shown = 0.0f; }                               // second press: next line
        }

        f.clear(rgb(0.08f, 0.09f, 0.14f));

        const Rect box = f.anchored(Anchor::Bottom, {f.width - 80.0f, 190.0f}, {0, -30});
        f.rounded_rect(box.pos, box.size, 16, rgba{0.12f, 0.14f, 0.22f, 0.95f});
        f.text(current.speaker, {box.pos.x + 24, box.pos.y + 14}, {.size = 26, .color = coral});
        // substr() shows just the first N characters; max_width wraps the text inside the box.
        f.text(current.text.substr(0, static_cast<size_t>(shown)), {box.pos.x + 24, box.pos.y + 54},
               {.size = 28, .max_width = box.size.x - 48});
        if (line_done && !at_end) f.text("v", {box.pos.x + box.size.x - 40, box.pos.y + box.size.y - 40}, {.size = 26, .color = coral});

        if (line_done && at_end && result.empty()) {
            if (f.key_pressed(Key::Down)) selected = (selected + 1) % static_cast<int>(choices.size());
            if (f.key_pressed(Key::Up)) selected = (selected + static_cast<int>(choices.size()) - 1) % static_cast<int>(choices.size());
            for (size_t i = 0; i < choices.size(); ++i) {
                const Rect r{{box.pos.x + 40, box.pos.y - 70.0f - 54.0f * (choices.size() - 1 - i)}, {460, 44}};
                if (r.contains(f.mouse())) selected = static_cast<int>(i);
                const bool picked = (r.contains(f.mouse()) && f.mouse_pressed(Mouse::Left)) ||
                                    (selected == static_cast<int>(i) && f.key_pressed(Key::Enter));
                f.rounded_rect(r.pos, r.size, 10, selected == static_cast<int>(i) ? rgb(0.3f, 0.4f, 0.65f) : rgb(0.16f, 0.18f, 0.28f));
                f.text(choices[i], {r.pos.x + 16, r.pos.y + 6}, {.size = 26});
                if (picked) result = choices[i];
            }
        }
        if (!result.empty()) f.text("you chose: " + result, Anchor::Top, {0, 40}, {.size = 36});
    });

    THISTLE_RUN(app);
}
