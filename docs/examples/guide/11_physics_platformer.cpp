// Guide 11: a platformer on top of Box2D. Arrow keys / A D to move, Space or W
// to jump. Collect the coins, avoid the red spikes, reach the green flag. The
// platform in the pit carries you. Things worth studying:
//   - the game's rules live in Game::update(), separate from drawing, so a
//     test can drive them with scripted input (see docs/examples/tests/)
//   - how "am I standing on something?" is answered with raycasts
//   - coyote time and jump buffering (both make jumps feel fair)
//   - riding a moving platform by adding its speed to yours
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
using namespace thistle;

struct Input {
    float move = 0.0f;           // -1 left, 0 none, +1 right
    bool jump_pressed = false;   // true only on the frame the button went down
};

struct Solid { vec2 center, size; };
struct Coin { Body body; vec2 pos; };

class Game {
public:
    static constexpr float kRadius = 18.0f;
    static constexpr float kLiftLeft = 1180.0f, kLiftRight = 1420.0f;
    const vec2 start{120, 640};
    const vec2 lift_size{120, 20};

    Physics phys{vec2{0, 1400}};
    std::vector<Solid> solids;
    std::vector<Coin> coins;
    Body player, lift, spikes, flag;
    int collected = 0;
    bool won = false;
    int deaths = 0;
    bool grounded = false;

    Game() {
        solids = {
            {{550, 700}, {1100, 40}},     // ground, before the pit
            {{1850, 700}, {700, 40}},     // ground, after the pit
            {{450, 590}, {160, 20}},      // three steps up
            {{720, 500}, {160, 20}},
            {{1000, 430}, {160, 20}},
            {{-20, 360}, {40, 1200}},     // left wall
            {{2220, 560}, {40, 300}},     // wall behind the flag
        };
        for (const Solid& s : solids) phys.add_static_box(s.center, s.size);

        lift = phys.add_kinematic_box({kLiftLeft, 660}, lift_size, 1.0f);
        phys.set_velocity(lift, {90, 0});

        player = phys.add_dynamic_circle(start, kRadius, 1.0f, 0.0f, 0.0f);
        phys.set_fixed_rotation(player, true);

        spikes = phys.add_sensor_box({1750, 670}, {80, 20});
        flag = phys.add_sensor_box({2100, 640}, {30, 100});
        for (const vec2 p : {vec2{450, 540}, vec2{720, 450}, vec2{1000, 380}, vec2{1300, 590}, vec2{1950, 640}}) {
            coins.push_back({phys.add_sensor_box(p, {26, 26}), p});
        }

        // Runs inside step(), so only note what happened; change the world later.
        phys.on_collision([this](Body a, Body b) {
            const Body other = (a == player) ? b : (b == player ? a : Body{});
            if (!other.valid()) return;
            if (other == spikes) hit_spikes_ = true;
            else if (other == flag) won = true;
            else touched_.push_back(other);
        });
    }
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    void update(const Input& in, float dt) {
        // What's under the player? Rays start at the player's center, and a ray
        // ignores the shape it starts inside, so the player never blocks itself.
        const vec2 p = phys.position(player);
        Body ground{};
        for (const float dx : {-kRadius * 0.8f, 0.0f, kRadius * 0.8f}) {
            const RayHit hit = phys.raycast({p.x + dx, p.y}, {p.x + dx, p.y + kRadius + 5.0f});
            if (hit.hit) { ground = hit.body; break; }
        }
        grounded = ground.valid();

        coyote_ = grounded ? 0.1f : coyote_ - dt;
        jump_buffer_ -= dt;
        if (in.jump_pressed) jump_buffer_ = 0.12f;

        vec2 v = phys.velocity(player);
        // Standing on something that moves? Add its speed, or you slide off it.
        const float ground_speed = grounded ? phys.velocity(ground).x : 0.0f;
        v.x = in.move * 260.0f + ground_speed;
        if (jump_buffer_ > 0.0f && coyote_ > 0.0f) {
            v.y = -640.0f;
            jump_buffer_ = 0.0f;
            coyote_ = 0.0f;
        }
        phys.set_velocity(player, v);

        const vec2 lp = phys.position(lift);
        if (lp.x > kLiftRight) phys.set_velocity(lift, {-90, 0});
        if (lp.x < kLiftLeft) phys.set_velocity(lift, {90, 0});

        phys.step(dt);

        // Now that the step is over it's safe to change the world.
        for (const Body& b : touched_) {
            for (auto it = coins.begin(); it != coins.end(); ++it) {
                if (it->body == b) { phys.remove(b); coins.erase(it); ++collected; break; }
            }
        }
        touched_.clear();

        if (hit_spikes_ || phys.position(player).y > 1000.0f) {
            phys.reset_body(player, start);
            hit_spikes_ = false;
            ++deaths;
        }
    }

    void draw(Frame& f, float camera_x) const {
        f.camera({-camera_x, 0});
        for (const Solid& s : solids) {
            f.rect({s.center.x - s.size.x / 2, s.center.y - s.size.y / 2}, s.size, rgb(0.32f, 0.36f, 0.5f));
        }
        const vec2 lc = phys.position(lift);
        f.rect({lc.x - lift_size.x / 2, lc.y - lift_size.y / 2}, lift_size, rgb(0.5f, 0.42f, 0.25f));
        f.triangle({1710, 680}, {1750, 640}, {1790, 680}, rgb(0.9f, 0.25f, 0.25f));
        f.rect({2085, 590}, {6, 100}, white);
        f.triangle({2091, 590}, {2091, 620}, {2125, 605}, rgb(0.35f, 0.9f, 0.5f));
        for (const Coin& c : coins) f.circle(c.pos, 11, rgb(1.0f, 0.82f, 0.25f));
        f.circle(phys.position(player), kRadius, rgb(0.35f, 0.75f, 0.95f));
        f.camera({0, 0});
    }

private:
    float coyote_ = 0.0f;        // time left in which a jump still counts after walking off a ledge
    float jump_buffer_ = 0.0f;   // time left in which an early jump press still counts
    bool hit_spikes_ = false;
    std::vector<Body> touched_;
};

THISTLE_MAIN {
    App app{{.title = "Physics platformer", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    Game game;
    float camera_x = 0.0f;

    app.update([&](Frame f) {
        const float dt = std::min(f.dt, 1.0f / 30.0f);

        Input in;
        if (f.key_down(Key::Left)  || f.key_down(Key::A)) in.move -= 1.0f;
        if (f.key_down(Key::Right) || f.key_down(Key::D)) in.move += 1.0f;
        in.jump_pressed = f.key_pressed(Key::Space) || f.key_pressed(Key::W) || f.key_pressed(Key::Up);
        game.update(in, dt);

        // A camera that eases toward the player instead of snapping to it.
        const float target = game.phys.position(game.player).x - f.width * 0.4f;
        camera_x += (target - camera_x) * std::min(1.0f, 6.0f * dt);
        camera_x = std::clamp(camera_x, 0.0f, std::max(0.0f, 2240.0f - f.width));

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        game.draw(f, camera_x);
        f.text("coins " + std::to_string(game.collected) + " / 5   deaths " + std::to_string(game.deaths) +
                   (game.won ? "     you reached the flag!" : ""),
               Anchor::TopLeft, {20, 16}, {.size = 26});
    });

    THISTLE_RUN(app);
}
