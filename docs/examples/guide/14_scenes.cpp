// Guide 14: screens, with App::scene. A title, the game, and a game-over
// screen, with a fade between them. In the game, move with the arrows / A D and
// dodge the falling blocks; P pauses. Each scene has enter / update / exit, and
// set_scene() switches at the start of the next frame.
#include <thistle.hpp>
#include <algorithm>
#include <random>
#include <string>
#include <vector>
using namespace thistle;

struct Faller { Rect rect; float speed; };

THISTLE_MAIN {
    App app{{.title = "Scenes", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    std::mt19937 rng{99};
    auto between = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };

    // State the scenes share. A scene is just three functions; where the data
    // lives is up to you.
    Rect player{{600, 640}, {60, 40}};
    std::vector<Faller> fallers;
    float spawn_timer = 0.0f;
    float survived = 0.0f;
    bool paused = false;
    float fade = 1.0f;             // 1 = black, 0 = clear
    float fade_target = 0.0f;
    std::string next_scene;        // while fading out to this scene
    int best = save::get_int("scenes_best", 0);

    // Fade out, switch, fade back in. The switch happens when the screen is black.
    auto change_scene = [&](const std::string& name) { next_scene = name; fade_target = 1.0f; };

    auto draw_fade = [&](Frame& f, float dt) {
        const float speed = 3.0f;
        if (fade < fade_target) fade = std::min(fade_target, fade + speed * dt);
        else if (fade > fade_target) fade = std::max(fade_target, fade - speed * dt);
        if (!next_scene.empty() && fade >= 1.0f) {
            set_scene(next_scene);
            next_scene.clear();
            fade_target = 0.0f;
        }
        if (fade > 0.0f) f.rect({0, 0}, {static_cast<float>(f.width), static_cast<float>(f.height)}, rgba{0, 0, 0, fade});
    };

    app.scene("title", {
        .update = [&](Frame f) {
            f.clear(rgb(0.08f, 0.09f, 0.14f));
            f.text("DODGE", Anchor::Center, {0, -80}, {.size = 96});
            f.text("press Enter", Anchor::Center, {0, 20}, {.size = 32});
            f.text("best: " + std::to_string(best) + " s", Anchor::Center, {0, 70}, {.size = 26, .color = rgb(0.6f, 0.65f, 0.8f)});
            if (f.key_pressed(Key::Enter) && next_scene.empty()) change_scene("play");
            draw_fade(f, f.dt);
        },
    });

    app.scene("play", {
        .enter = [&] {
            fallers.clear();
            player.pos = {600, 640};
            spawn_timer = 0.0f;
            survived = 0.0f;
            paused = false;
        },
        .update = [&](Frame f) {
            const float dt = std::min(f.dt, 1.0f / 30.0f);
            if (f.key_pressed(Key::P)) paused = !paused;

            if (!paused) {
                float dir = 0.0f;
                if (f.key_down(Key::Left)  || f.key_down(Key::A)) dir -= 1;
                if (f.key_down(Key::Right) || f.key_down(Key::D)) dir += 1;
                player.pos.x = std::clamp(player.pos.x + dir * 520.0f * dt, 0.0f, f.width - player.size.x);

                survived += dt;
                spawn_timer -= dt;
                if (spawn_timer <= 0.0f) {
                    spawn_timer += std::max(0.15f, 0.7f - survived * 0.01f);   // gets harder
                    const float w = between(30, 90);
                    fallers.push_back({{{between(0, f.width - w), -50}, {w, 40}}, between(250, 450) + survived * 6});
                }
                for (Faller& b : fallers) b.rect.pos.y += b.speed * dt;
                fallers.erase(std::remove_if(fallers.begin(), fallers.end(),
                                             [&](const Faller& b) { return b.rect.pos.y > f.height; }),
                              fallers.end());

                for (const Faller& b : fallers) {
                    if (b.rect.overlaps(player) && next_scene.empty()) {
                        const int seconds = static_cast<int>(survived);
                        if (seconds > best) { best = seconds; save::set_int("scenes_best", best); }
                        change_scene("gameover");
                    }
                }
            }

            f.clear(rgb(0.08f, 0.09f, 0.14f));
            for (const Faller& b : fallers) f.rect(b.rect.pos, b.rect.size, rgb(0.9f, 0.45f, 0.3f));
            f.rect(player.pos, player.size, rgb(0.35f, 0.75f, 0.95f));
            f.text(std::to_string(static_cast<int>(survived)) + " s", Anchor::TopLeft, {20, 16}, {.size = 32});
            if (paused) {
                f.rect({0, 0}, {static_cast<float>(f.width), static_cast<float>(f.height)}, rgba{0, 0, 0, 0.55f});
                f.text("paused", Anchor::Center, {0, 0}, {.size = 64});
            }
            draw_fade(f, f.dt);
        },
    });

    app.scene("gameover", {
        .update = [&](Frame f) {
            f.clear(rgb(0.12f, 0.06f, 0.08f));
            f.text("GAME OVER", Anchor::Center, {0, -70}, {.size = 80});
            f.text("you lasted " + std::to_string(static_cast<int>(survived)) + " s   (best " + std::to_string(best) + ")",
                   Anchor::Center, {0, 10}, {.size = 32});
            f.text("Enter: again    Esc: title", Anchor::Center, {0, 70}, {.size = 26});
            if (next_scene.empty()) {
                if (f.key_pressed(Key::Enter)) change_scene("play");
                if (f.key_pressed(Key::Escape)) change_scene("title");
            }
            draw_fade(f, f.dt);
        },
    });

    THISTLE_RUN(app);
}
