// Recipe: waves of enemies from a data table. Each wave says how many of
// each kind to spawn and how fast; between waves there's a short breather.
// Click an enemy to kill it. Easy to extend: change the table, not the code.
#include <thistle.hpp>
#include <algorithm>
#include <random>
#include <string>
#include <vector>
using namespace thistle;

struct Kind { const char* name; float radius, speed; int hp; rgba color; };
static const Kind kKinds[] = {
    {"grunt", 16, 70, 1, {0.9f, 0.45f, 0.3f, 1}},
    {"runner", 11, 170, 1, {0.95f, 0.8f, 0.3f, 1}},
    {"brute", 28, 45, 4, {0.7f, 0.3f, 0.8f, 1}},
};
struct WaveEntry { int kind, count; };
struct Wave { std::vector<WaveEntry> entries; float spawn_interval; };

static const std::vector<Wave> kWaves = {
    {{{0, 5}}, 0.8f},
    {{{0, 6}, {1, 3}}, 0.6f},
    {{{1, 8}, {2, 1}}, 0.5f},
    {{{0, 8}, {1, 6}, {2, 3}}, 0.4f},
};

struct Enemy { vec2 pos; int kind, hp; bool dead = false; };

THISTLE_MAIN {
    App app{{.title = "Waves", .width = 1100, .height = 640}};
    load_font("assets/font.ttf");

    std::mt19937 rng{11};
    std::vector<Enemy> enemies;
    std::vector<int> queue;       // kinds still to spawn in this wave
    size_t wave = 0;
    float spawn_timer = 0.0f, break_timer = 2.0f;
    int escaped = 0, killed = 0;
    bool waiting_for_next = true;

    auto start_wave = [&](size_t index) {
        queue.clear();
        for (const WaveEntry& e : kWaves[index].entries) queue.insert(queue.end(), e.count, e.kind);
        std::shuffle(queue.begin(), queue.end(), rng);
        waiting_for_next = false;
    };

    app.update([&](Frame f) {
        const float dt = std::min(f.dt, 1.0f / 30.0f);

        if (waiting_for_next) {
            break_timer -= dt;
            if (break_timer <= 0.0f && wave < kWaves.size()) start_wave(wave);
        } else {
            spawn_timer -= dt;
            if (spawn_timer <= 0.0f && !queue.empty()) {
                spawn_timer = kWaves[wave].spawn_interval;
                const int kind = queue.back();
                queue.pop_back();
                enemies.push_back({{-30.0f, std::uniform_real_distribution<float>(80, f.height - 80.0f)(rng)}, kind, kKinds[kind].hp});
            }
            if (queue.empty() && enemies.empty()) {      // wave cleared
                ++wave;
                waiting_for_next = true;
                break_timer = 2.5f;
            }
        }

        for (Enemy& e : enemies) {
            e.pos.x += kKinds[e.kind].speed * dt;
            if (e.pos.x > f.width + 30.0f) { e.dead = true; ++escaped; }
        }
        if (f.mouse_pressed(Mouse::Left)) {
            for (auto it = enemies.rbegin(); it != enemies.rend(); ++it) {
                if (!it->dead && it->pos.distance_to(f.mouse()) < kKinds[it->kind].radius + 6.0f) {
                    if (--it->hp <= 0) { it->dead = true; ++killed; }
                    break;
                }
            }
        }
        enemies.erase(std::remove_if(enemies.begin(), enemies.end(), [](const Enemy& e) { return e.dead; }), enemies.end());

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        for (const Enemy& e : enemies) {
            f.circle(e.pos, kKinds[e.kind].radius, kKinds[e.kind].color);
            if (kKinds[e.kind].hp > 1) f.text(std::to_string(e.hp), {e.pos.x - 6, e.pos.y - 12}, {.size = 22, .color = black});
        }
        std::string title = wave < kWaves.size() ? "wave " + std::to_string(wave + 1) + " / " + std::to_string(kWaves.size()) : "all waves cleared!";
        f.text(title + "     killed " + std::to_string(killed) + "     escaped " + std::to_string(escaped),
               Anchor::TopLeft, {20, 16}, {.size = 28});
        if (waiting_for_next && wave < kWaves.size()) f.text("next wave in " + std::to_string(std::max(0, static_cast<int>(break_timer) + 1)), Anchor::Center, {0, 0}, {.size = 48});
    });

    THISTLE_RUN(app);
}
