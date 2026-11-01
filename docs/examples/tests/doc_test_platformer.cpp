// Drives guide/11_physics_platformer.cpp's Game with scripted input and checks
// what happens: standing, running, jumping, riding the lift, coins, spikes,
// the flag, and the pit. No window.
#define main example_main_platformer
#include "../guide/11_physics_platformer.cpp"
#undef main

#include <cstdio>

namespace {
int g_failures = 0;
void check(bool cond, const char* msg) {
    if (cond) { std::printf("  ok  %s\n", msg); }
    else { std::printf("  FAIL %s\n", msg); ++g_failures; }
}
const float kDt = 1.0f / 60.0f;
void run(Game& g, Input in, int frames) {
    for (int i = 0; i < frames; ++i) { g.update(in, kDt); in.jump_pressed = false; }
}
}

int main() {
    const float ground_y = 680.0f - Game::kRadius;   // ground top is y = 680

    {   // Standing
        Game g;
        run(g, {}, 90);
        check(g.grounded, "after settling, the player is grounded");
        check(std::fabs(g.phys.position(g.player).y - ground_y) < 2.0f, "and rests on the ground");
        check(std::fabs(g.phys.position(g.player).x - g.start.x) < 1.0f, "without drifting sideways");
    }
    {   // Running
        Game g;
        run(g, {}, 30);
        const float x0 = g.phys.position(g.player).x;
        run(g, {1.0f, false}, 60);
        const float moved = g.phys.position(g.player).x - x0;
        check(moved > 230.0f && moved < 290.0f, "holding right for a second moves about 260 px");
        run(g, {-1.0f, false}, 60);
        check(g.phys.position(g.player).x < x0 + 40.0f, "holding left comes back");
        check(g.deaths == 0, "running along the ground isn't deadly");
    }
    {   // Jumping
        Game g;
        run(g, {}, 30);
        const float y0 = g.phys.position(g.player).y;
        float highest = y0;
        g.update({0, true}, kDt);
        for (int i = 0; i < 90; ++i) { g.update({}, kDt); highest = std::min(highest, g.phys.position(g.player).y); }
        check(y0 - highest > 120.0f && y0 - highest < 160.0f, "a jump rises about 146 px (v^2 / 2g)");
        check(g.grounded && std::fabs(g.phys.position(g.player).y - y0) < 2.0f, "and lands on the same spot");
    }
    {   // Can't jump in mid-air
        Game g;
        run(g, {}, 30);
        g.update({0, true}, kDt);
        run(g, {}, 20);   // in the air now
        const float vy_before = g.phys.velocity(g.player).y;
        g.update({0, true}, kDt);
        check(g.phys.velocity(g.player).y >= vy_before - 1.0f, "pressing jump in mid-air doesn't jump again");
    }
    {   // Coyote time: walking off the edge, a jump pressed just afterwards still works
        Game g;
        g.phys.reset_body(g.lift, {Game::kLiftRight, 660});   // out of the way
        g.phys.set_velocity(g.lift, {0, 0});
        g.phys.reset_body(g.player, {1060, ground_y});
        run(g, {}, 20);
        int guard = 0;
        while (g.grounded && guard++ < 120) g.update({1.0f, false}, kDt);   // walk right until the ground ends
        check(!g.grounded, "(setup) walked off the end of the ground");
        g.update({1.0f, true}, kDt);   // one frame after leaving it
        check(g.phys.velocity(g.player).y < -300.0f, "a jump pressed a moment after walking off the edge still jumps");
    }
    {   // ...but not forever
        Game g;
        g.phys.reset_body(g.lift, {Game::kLiftRight, 660});
        g.phys.set_velocity(g.lift, {0, 0});
        g.phys.reset_body(g.player, {1060, ground_y});
        run(g, {}, 20);
        int guard = 0;
        while (g.grounded && guard++ < 120) g.update({1.0f, false}, kDt);
        run(g, {1.0f, false}, 12);   // 0.2 s in the air: coyote time (0.1 s) is over
        g.update({1.0f, true}, kDt);
        check(g.phys.velocity(g.player).y > 0.0f, "but a jump pressed 0.2 s after leaving the ground doesn't");
    }
    {   // Jump buffering: pressed a few frames BEFORE landing, used on landing
        Game g;
        g.phys.reset_body(g.player, {300, 600});   // about 0.3 s above the ground
        for (int i = 0; i < 14; ++i) g.update({}, kDt);
        g.update({0, true}, kDt);                  // press while still falling
        float highest = g.phys.position(g.player).y;
        for (int i = 0; i < 60; ++i) { g.update({}, kDt); highest = std::min(highest, g.phys.position(g.player).y); }
        check(ground_y - highest > 100.0f, "a jump pressed just before landing is remembered and happens on landing");
    }
    {   // ...and a press long before landing is forgotten
        Game g;
        g.phys.reset_body(g.player, {300, 300});   // a long fall
        for (int i = 0; i < 4; ++i) g.update({}, kDt);
        g.update({0, true}, kDt);                  // press way too early
        bool landed = false;
        float most_upward = 0.0f;
        for (int i = 0; i < 120; ++i) {
            g.update({}, kDt);
            if (g.grounded) landed = true;
            if (landed) most_upward = std::min(most_upward, g.phys.velocity(g.player).y);
        }
        check(landed && most_upward > -5.0f, "a jump pressed way too early is forgotten, so landing doesn't bounce you up");
    }
    {   // Reachability: a running jump from the ground can land on the first shelf
        int successes = 0;
        for (int start_x = 180; start_x <= 340; start_x += 10) {
            Game g;
            g.phys.reset_body(g.player, {static_cast<float>(start_x), ground_y});
            run(g, {}, 20);
            g.update({1.0f, true}, kDt);
            run(g, {1.0f, false}, 38);
            run(g, {}, 40);
            const vec2 p = g.phys.position(g.player);
            if (p.y < 600.0f && p.x > 370.0f && p.x < 530.0f && g.grounded) ++successes;
        }
        check(successes >= 3, "the first shelf can be reached with a running jump from several starting spots");
    }
    {   // The bot: cross the pit by riding the lift, to prove the level can be finished.
        Game g;
        run(g, {}, 30);
        enum Phase { ToEdge, WaitLift, Riding, Done } phase = ToEdge;
        int frames = 0;
        for (; frames < 60 * 40 && phase != Done; ++frames) {
            Input in;
            const vec2 p = g.phys.position(g.player);
            const vec2 lp = g.phys.position(g.lift);
            const vec2 lv = g.phys.velocity(g.lift);
            if (phase == ToEdge) {
                if (p.x < 1070.0f) in.move = 1.0f; else phase = WaitLift;
            }
            if (phase == WaitLift) {
                // Jump when the lift is coming back toward us and close.
                if (lv.x > 0 && lp.x > 1185.0f && lp.x < 1215.0f && g.grounded) { in.move = 1.0f; in.jump_pressed = true; phase = Riding; }
            } else if (phase == Riding) {
                const bool lift_is_right = lp.x > 1395.0f && lv.x > 0;
                if (g.grounded && lift_is_right) { in.move = 1.0f; in.jump_pressed = true; }
                else if (!g.grounded && (p.x < lp.x - 10.0f || p.x > 1300.0f)) in.move = 1.0f;   // steer through the air
                else if (g.grounded && p.x > 1480.0f) in.move = 1.0f;
                if (p.x > 1520.0f && g.grounded && p.y < 700.0f) phase = Done;
            }
            g.update(in, kDt);
        }
        check(phase == Done, "a simple bot rides the lift across the pit and reaches the far side");
        check(g.deaths == 0, "without falling");
    }
    {   // The lift carries an idle player
        Game g;
        g.phys.reset_body(g.player, {g.phys.position(g.lift).x, 600.0f});
        run(g, {}, 20);   // lands on the lift
        const float lx0 = g.phys.position(g.lift).x;
        const float px0 = g.phys.position(g.player).x;
        run(g, {}, 90);   // 1.5 s with no input
        const float lift_moved = g.phys.position(g.lift).x - lx0;
        const float player_moved = g.phys.position(g.player).x - px0;
        check(std::fabs(lift_moved) > 100.0f, "(setup) the lift moves");
        check(std::fabs(player_moved - lift_moved) < 12.0f, "an idle player on the lift moves along with it");
        check(g.deaths == 0, "and doesn't fall off");
    }
    {   // Falling into the pit respawns
        Game g;
        g.phys.reset_body(g.player, {1130, 500});   // above the middle of the pit, away from the lift's start
        // Park the lift out of the way by sending it to the far side.
        g.phys.reset_body(g.lift, {Game::kLiftRight, 660});
        g.phys.set_velocity(g.lift, {-90, 0});
        run(g, {}, 120);
        const int deaths_a = g.deaths;
        run(g, {}, 120);
        check(g.deaths >= 1 || deaths_a >= 1 || g.phys.position(g.player).y < 1000.0f, "(setup) fell or landed");
    }
    {   // Pit: player placed directly in the pit with the lift away
        Game g;
        g.phys.reset_body(g.lift, {Game::kLiftRight, 660});
        g.phys.reset_body(g.player, {1120, 900});
        run(g, {}, 200);
        check(g.deaths == 1, "falling out of the world counts a death");
        check(std::fabs(g.phys.position(g.player).x - g.start.x) < 30.0f && g.phys.position(g.player).y < 700.0f, "and puts the player back at the start");
    }
    {   // Spikes
        Game g;
        g.phys.reset_body(g.player, {1750, 640});
        run(g, {}, 60);
        check(g.deaths == 1, "touching the spikes kills the player once");
        check(std::fabs(g.phys.position(g.player).x - g.start.x) < 30.0f, "and respawns at the start");
    }
    {   // Coins
        Game g;
        check(g.coins.size() == 5, "five coins to begin with");
        g.phys.reset_body(g.player, g.coins[0].pos);
        run(g, {}, 5);
        check(g.collected == 1 && g.coins.size() == 4, "touching a coin collects it and removes it");
        g.phys.reset_body(g.player, g.coins[0].pos);
        run(g, {}, 5);
        g.phys.reset_body(g.player, {450, 540});   // where coin 0 used to be
        run(g, {}, 5);
        check(g.collected == 2, "a coin that's gone can't be collected twice");
    }
    {   // Flag
        Game g;
        g.phys.reset_body(g.player, {2100, 640});
        run(g, {}, 5);
        check(g.won, "touching the flag wins");
    }

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
