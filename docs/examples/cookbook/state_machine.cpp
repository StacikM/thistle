// Recipe: enemy AI as a state machine. The enemy patrols between two points;
// if you (the mouse) come close it chases; close enough it attacks; if you get
// far it gives up and returns to patrolling. Each state is one case in a switch,
// and all the rules for changing state are in one place.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <string>
using namespace thistle;

enum class State { Patrol, Chase, Attack };

static const char* name_of(State s) {
    switch (s) {
        case State::Patrol: return "patrol";
        case State::Chase: return "chase";
        case State::Attack: return "attack";
    }
    return "?";
}

struct Enemy {
    vec2 pos{200, 360};
    State state = State::Patrol;
    float state_time = 0.0f;       // how long we've been in this state
    int patrol_target = 1;         // which end of the patrol we're heading to
    float attack_flash = 0.0f;

    void set_state(State next) { state = next; state_time = 0.0f; }

    void update(vec2 target, float dt) {
        state_time += dt;
        const float dist = pos.distance_to(target);
        const vec2 patrol[2] = {{200, 360}, {1000, 360}};

        switch (state) {
            case State::Patrol: {
                const vec2 goal = patrol[patrol_target];
                pos += (goal - pos).normalized() * (90.0f * dt);
                if (pos.distance_to(goal) < 6.0f) patrol_target = 1 - patrol_target;
                if (dist < 260.0f) set_state(State::Chase);
                break;
            }
            case State::Chase:
                pos += (target - pos).normalized() * (170.0f * dt);
                if (dist < 60.0f) set_state(State::Attack);
                else if (dist > 420.0f) set_state(State::Patrol);
                break;
            case State::Attack:
                attack_flash = 0.15f;
                if (state_time > 0.6f) set_state(dist < 60.0f ? State::Attack : State::Chase);   // swing again, or chase
                break;
        }
        attack_flash = std::max(0.0f, attack_flash - dt);
    }
};

THISTLE_MAIN {
    App app{{.title = "State machine", .width = 1200, .height = 720}};
    load_font("assets/font.ttf");

    Enemy enemy;

    app.update([&](Frame f) {
        enemy.update(f.mouse(), std::min(f.dt, 1.0f / 30.0f));

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        f.circle_outline(enemy.pos, 260, rgb(0.25f, 0.28f, 0.4f), 1.0f);   // notice range
        f.circle_outline(enemy.pos, 60, rgb(0.5f, 0.3f, 0.3f), 1.0f);      // attack range
        const rgba body = enemy.state == State::Patrol ? rgb(0.4f, 0.6f, 0.9f)
                        : enemy.state == State::Chase ? rgb(0.95f, 0.75f, 0.3f) : rgb(0.95f, 0.3f, 0.3f);
        f.circle(enemy.pos, 26, body);
        if (enemy.attack_flash > 0.0f) f.circle(enemy.pos, 60, rgba{1, 0.3f, 0.3f, 0.35f});
        f.circle(f.mouse(), 8, white);
        f.text(std::string("state: ") + name_of(enemy.state) + "   (" + std::to_string(enemy.state_time).substr(0, 3) + " s)",
               Anchor::TopLeft, {20, 16}, {.size = 28});
        f.text("move the mouse near the enemy", Anchor::BottomLeft, {20, -16}, {.size = 22});
    });

    THISTLE_RUN(app);
}
