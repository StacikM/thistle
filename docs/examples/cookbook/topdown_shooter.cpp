// Recipe: a top-down shooter. WASD moves, the mouse aims, hold the left
// button to fire. Enemies chase you; touching one ends the run (press R).
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>
using namespace thistle;

struct Bullet { vec2 pos, vel; float life = 1.2f; bool dead = false; };
struct Enemy  { vec2 pos; float hp = 2.0f; bool dead = false; };

THISTLE_MAIN {
    App app{{.title = "Top-down shooter", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    std::mt19937 rng{5};
    auto rnd = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };

    vec2 player{640, 360};
    std::vector<Bullet> bullets;
    std::vector<Enemy> enemies;
    float fire_cooldown = 0.0f, spawn_timer = 1.0f;
    int score = 0;
    bool alive = true;

    app.update([&](Frame f) {
        const float dt = std::min(f.dt, 1.0f / 30.0f);
        const vec2 W{static_cast<float>(f.width), static_cast<float>(f.height)};

        if (!alive && f.key_pressed(Key::R)) {
            bullets.clear(); enemies.clear(); player = W * 0.5f; score = 0; alive = true;
        }

        if (alive) {
            vec2 dir{0, 0};
            if (f.key_down(Key::A)) dir.x -= 1;
            if (f.key_down(Key::D)) dir.x += 1;
            if (f.key_down(Key::W)) dir.y -= 1;
            if (f.key_down(Key::S)) dir.y += 1;
            player += dir.normalized() * (300.0f * dt);
            player.x = std::clamp(player.x, 16.0f, W.x - 16.0f);
            player.y = std::clamp(player.y, 16.0f, W.y - 16.0f);

            // Shooting: a cooldown stops one click from firing 60 bullets a second.
            fire_cooldown -= dt;
            if (f.mouse_down(Mouse::Left) && fire_cooldown <= 0.0f) {
                fire_cooldown = 0.12f;
                const vec2 aim = (f.mouse() - player).normalized();
                bullets.push_back({player + aim * 20.0f, aim * 900.0f});
            }

            // Spawn enemies just outside the screen edge.
            spawn_timer -= dt;
            if (spawn_timer <= 0.0f) {
                spawn_timer += std::max(0.25f, 1.2f - score * 0.02f);
                const bool horizontal = rnd(0, 1) < 0.5f;
                enemies.push_back({horizontal ? vec2{rnd(0, W.x), rnd(0, 1) < 0.5f ? -30.0f : W.y + 30.0f}
                                              : vec2{rnd(0, 1) < 0.5f ? -30.0f : W.x + 30.0f, rnd(0, W.y)}});
            }

            for (Bullet& b : bullets) {
                b.pos += b.vel * dt;
                b.life -= dt;
                if (b.life <= 0.0f) b.dead = true;
            }
            for (Enemy& e : enemies) {
                e.pos += (player - e.pos).normalized() * (110.0f * dt);
                if (e.pos.distance_to(player) < 28.0f) alive = false;
                for (Bullet& b : bullets) {
                    if (!b.dead && b.pos.distance_to(e.pos) < 18.0f) {
                        b.dead = true;
                        e.hp -= 1.0f;
                        if (e.hp <= 0.0f) { e.dead = true; ++score; }
                    }
                }
            }
            bullets.erase(std::remove_if(bullets.begin(), bullets.end(), [](const Bullet& b) { return b.dead; }), bullets.end());
            enemies.erase(std::remove_if(enemies.begin(), enemies.end(), [](const Enemy& e) { return e.dead; }), enemies.end());
        }

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        for (const Enemy& e : enemies) f.circle(e.pos, 16, rgb(0.9f, 0.35f, 0.3f));
        for (const Bullet& b : bullets) f.circle(b.pos, 4, rgb(1.0f, 0.9f, 0.4f));
        if (alive) {
            f.circle(player, 14, rgb(0.35f, 0.75f, 0.95f));
            const vec2 aim = (f.mouse() - player).normalized();
            f.line(player, player + aim * 26.0f, white, 4.0f);
        }
        f.circle_outline(f.mouse(), 10, white, 1.5f);
        f.text("score " + std::to_string(score), Anchor::TopLeft, {20, 16}, {.size = 28});
        if (!alive) f.text("you died - press R", Anchor::Center, {0, 0}, {.size = 56});
    });

    THISTLE_RUN(app);
}
