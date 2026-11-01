// Recipe: Asteroids-style flight. Left / Right rotate, Up thrusts, Space
// fires. The ship has momentum (it keeps drifting), and everything wraps
// around the screen edges.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>
using namespace thistle;

struct Rock { vec2 pos, vel; float radius; bool dead = false; };
struct Shot { vec2 pos, vel; float life = 1.0f; };

static vec2 wrap(vec2 p, float w, float h) {
    if (p.x < 0) p.x += w; else if (p.x > w) p.x -= w;
    if (p.y < 0) p.y += h; else if (p.y > h) p.y -= h;
    return p;
}

THISTLE_MAIN {
    App app{{.title = "Asteroids flight", .width = 1100, .height = 700}};
    load_font("assets/font.ttf");

    std::mt19937 rng{21};
    auto rnd = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };

    vec2 ship{550, 350}, vel{0, 0};
    float angle = -1.5708f;   // pointing up (angles are radians; 0 points right)
    float cooldown = 0.0f;
    std::vector<Rock> rocks;
    std::vector<Shot> shots;
    for (int i = 0; i < 6; ++i) rocks.push_back({{rnd(0, 1100), rnd(0, 700)}, {rnd(-60, 60), rnd(-60, 60)}, rnd(30, 50)});

    app.update([&](Frame f) {
        const float dt = std::min(f.dt, 1.0f / 30.0f);
        const float W = static_cast<float>(f.width), H = static_cast<float>(f.height);

        if (f.key_down(Key::Left)) angle -= 3.5f * dt;
        if (f.key_down(Key::Right)) angle += 3.5f * dt;
        const vec2 forward{std::cos(angle), std::sin(angle)};
        const bool thrusting = f.key_down(Key::Up);
        if (thrusting) vel += forward * (320.0f * dt);
        vel = vel * (1.0f - 0.3f * dt);                        // a little drag, so it isn't forever
        if (vel.length() > 500.0f) vel = vel.normalized() * 500.0f;
        ship = wrap(ship + vel * dt, W, H);

        cooldown -= dt;
        if (f.key_down(Key::Space) && cooldown <= 0.0f) {
            cooldown = 0.2f;
            shots.push_back({ship + forward * 20.0f, vel + forward * 600.0f});
        }
        for (Shot& s : shots) { s.pos = wrap(s.pos + s.vel * dt, W, H); s.life -= dt; }
        shots.erase(std::remove_if(shots.begin(), shots.end(), [](const Shot& s) { return s.life <= 0.0f; }), shots.end());

        std::vector<Rock> spawned;
        for (Rock& r : rocks) {
            r.pos = wrap(r.pos + r.vel * dt, W, H);
            for (auto it = shots.begin(); it != shots.end(); ++it) {
                if (it->pos.distance_to(r.pos) < r.radius) {
                    shots.erase(it);
                    r.dead = true;
                    if (r.radius > 20.0f) {     // big rocks split in two
                        for (int k = 0; k < 2; ++k) spawned.push_back({r.pos, {rnd(-120, 120), rnd(-120, 120)}, r.radius * 0.55f});
                    }
                    break;
                }
            }
        }
        rocks.erase(std::remove_if(rocks.begin(), rocks.end(), [](const Rock& r) { return r.dead; }), rocks.end());
        rocks.insert(rocks.end(), spawned.begin(), spawned.end());

        f.clear(rgb(0.04f, 0.05f, 0.09f));
        for (const Rock& r : rocks) f.circle_outline(r.pos, r.radius, rgb(0.7f, 0.72f, 0.8f), 3.0f, 14);
        for (const Shot& s : shots) f.circle(s.pos, 3, rgb(1.0f, 0.9f, 0.4f));
        // The ship: a triangle pointing along `angle`.
        const vec2 nose = ship + forward * 18.0f;
        const vec2 left = ship + vec2{std::cos(angle + 2.5f), std::sin(angle + 2.5f)} * 14.0f;
        const vec2 right = ship + vec2{std::cos(angle - 2.5f), std::sin(angle - 2.5f)} * 14.0f;
        f.triangle(nose, left, right, rgb(0.35f, 0.75f, 0.95f));
        if (thrusting) f.triangle(ship - forward * 20.0f, left * 0.5f + ship * 0.5f, right * 0.5f + ship * 0.5f, rgb(1.0f, 0.6f, 0.2f));
        f.text("left/right: turn   up: thrust   space: fire   rocks " + std::to_string(rocks.size()), Anchor::TopLeft, {16, 12}, {.size = 22});
    });

    THISTLE_RUN(app);
}
