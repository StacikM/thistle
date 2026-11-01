// Guide 13: user interface. A title screen built with Menu, a settings screen
// built from checkbox/slider/button, and a HUD pinned to the corners with
// anchors. Everything is sized from the window height, so the same code looks
// right on a laptop and on a phone. Press ESC in the game to go back.
#include <thistle.hpp>
#include <algorithm>
#include <string>
using namespace thistle;

enum class Screen { Title, Settings, Game };

THISTLE_MAIN {
    App app{{.title = "UI", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    Screen screen = Screen::Title;
    float screen_started = 0.0f;
    bool music_on = true;
    float volume = 0.7f;
    float health = 0.8f;
    int score = 0;
    std::string player_name = "Player";
    bool editing_name = false;

    auto go = [&](Screen next, const Frame& f) { screen = next; screen_started = static_cast<float>(f.time); };

    app.update([&](Frame f) {
        // One number that makes every size scale with the window: 1.0 at 720 px tall.
        const float s = f.height / 720.0f;

        f.clear(rgb(0.08f, 0.09f, 0.14f));

        if (screen == Screen::Title) {
            Menu m(f, static_cast<float>(f.time) - screen_started, MenuAnim::FromTop);
            m.title("Thistle UI");
            m.label("a tour of the widgets");
            m.gap();
            if (m.button("Play")) go(Screen::Game, f);
            if (m.button("Settings")) go(Screen::Settings, f);
        }

        else if (screen == Screen::Settings) {
            const Rect panel = f.anchored(Anchor::Center, {560 * s, 440 * s});
            f.rounded_rect(panel.pos, panel.size, 18 * s, rgb(0.14f, 0.16f, 0.24f));
            f.text("Settings", {panel.pos.x + 28 * s, panel.pos.y + 20 * s}, {.size = 40 * s});

            // Rows inside the panel, placed relative to it.
            const float x = panel.pos.x + 28 * s;
            float y = panel.pos.y + 90 * s;
            f.checkbox("Music", music_on, {{x, y}, {32 * s, 32 * s}}, {.text_size = 28 * s});
            y += 60 * s;
            f.text("Volume  " + std::to_string(static_cast<int>(volume * 100)) + "%", {x, y}, {.size = 26 * s});
            y += 40 * s;
            volume = f.slider(volume, 0.0f, 1.0f, {{x, y}, {panel.size.x - 56 * s, 28 * s}});
            y += 60 * s;

            // A text field: click it to type; Enter or clicking elsewhere finishes.
            const Rect field{{x, y}, {panel.size.x - 56 * s, 44 * s}};
            if (f.mouse_pressed(Mouse::Left)) {
                const bool inside = field.contains(f.mouse());
                if (inside && !editing_name) { begin_text_input(player_name, 12); editing_name = true; }
                else if (!inside && editing_name) { player_name = text_input(); end_text_input(); editing_name = false; }
            }
            if (editing_name && f.key_pressed(Key::Enter)) { player_name = text_input(); end_text_input(); editing_name = false; }
            f.rect(field.pos, field.size, editing_name ? rgb(0.25f, 0.3f, 0.45f) : rgb(0.18f, 0.2f, 0.3f));
            f.text(editing_name ? text_input() + "|" : player_name, {field.pos.x + 12 * s, field.pos.y + 6 * s}, {.size = 28 * s});
            f.text("name (click to edit)", {x, y - 26 * s}, {.size = 18 * s, .color = rgb(0.6f, 0.65f, 0.8f)});

            if (f.button("Back", anchored(Anchor::Bottom, {160 * s, 52 * s}, {0, -20 * s}, panel))) go(Screen::Title, f);
        }

        else {   // Screen::Game
            // The HUD never uses absolute coordinates: it's pinned to corners.
            f.text(player_name, Anchor::TopLeft, {24 * s, 20 * s}, {.size = 28 * s});
            const Rect bar = f.anchored(Anchor::TopLeft, {260 * s, 22 * s}, {24 * s, 64 * s});
            f.progress_bar(health, bar, health > 0.3f ? rgb(0.35f, 0.85f, 0.5f) : rgb(0.95f, 0.3f, 0.3f));
            f.text("score " + std::to_string(score), Anchor::TopRight, {-24 * s, 20 * s}, {.size = 32 * s});

            f.text("Space: score    H: hurt    ESC: back", Anchor::Bottom, {0, -24 * s}, {.size = 22 * s});
            if (f.key_pressed(Key::Space)) score += 10;
            if (f.key_pressed(Key::H)) health = std::max(0.0f, health - 0.15f);
            if (f.key_pressed(Key::Escape)) go(Screen::Title, f);

            // A pause button: a fixed-size touch target in the corner.
            if (f.button("II", f.anchored(Anchor::TopRight, {56 * s, 56 * s}, {-24 * s, 70 * s}))) go(Screen::Title, f);
        }
    });

    THISTLE_RUN(app);
}
