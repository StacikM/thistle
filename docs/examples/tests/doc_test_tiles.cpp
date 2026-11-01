// Asserts on the plain functions inside guide/09_tiles.cpp and
// guide/08_collision.cpp. Their main() is renamed so they can be included
// here; nothing opens a window.
#define main example_main_tiles
#include "../guide/09_tiles.cpp"
#undef main

namespace collision_example {
#define main example_main_collision
#include "../guide/08_collision.cpp"
#undef main
}

#include <cstdio>

namespace {
int g_failures = 0;
void check(bool cond, const char* msg) {
    if (cond) { std::printf("  ok  %s\n", msg); }
    else { std::printf("  FAIL %s\n", msg); ++g_failures; }
}
}

int main() {
    // --- guide/09: tile movement --------------------------------------------
    {
        World w;   // the level from the guide: floor at row 9, y = 360
        Player p;
        p.body.pos = {100, 100};

        // Falling lands on the floor and stops there.
        for (int i = 0; i < 240; ++i) { p.velocity.y += 1500.0f * (1.0f / 60.0f); move_player(w, p, 1.0f / 60.0f); }
        check(p.on_ground, "player falls and ends up on the ground");
        check(std::fabs(p.body.pos.y + p.body.size.y - 9 * kTile) < 0.01f, "feet rest exactly on top of the floor tile");
        check(p.velocity.y == 0.0f, "vertical speed is zero on the ground");

        // Standing still on the floor stays on the floor (no sinking, no jitter).
        const float resting_y = p.body.pos.y;
        for (int i = 0; i < 120; ++i) { p.velocity.y += 1500.0f * (1.0f / 60.0f); move_player(w, p, 1.0f / 60.0f); }
        check(std::fabs(p.body.pos.y - resting_y) < 0.01f && p.on_ground, "standing still doesn't sink or float");

        // Jumping goes up, then comes back down to the same floor.
        p.velocity.y = -560.0f;
        float highest = p.body.pos.y;
        for (int i = 0; i < 180; ++i) {
            p.velocity.y += 1500.0f * (1.0f / 60.0f);
            move_player(w, p, 1.0f / 60.0f);
            highest = std::min(highest, p.body.pos.y);
        }
        check(highest < resting_y - 60.0f, "a jump rises well above the floor");
        check(p.on_ground && std::fabs(p.body.pos.y - resting_y) < 0.01f, "and lands back on the same spot");

        // Walking into the left edge of the level stops at the edge.
        p.velocity.x = -240.0f;
        for (int i = 0; i < 300; ++i) { p.velocity.x = -240.0f; p.velocity.y += 25.0f; move_player(w, p, 1.0f / 60.0f); }
        check(p.body.pos.x >= 0.0f, "the left edge of the level is a wall");

        // A wall tile blocks sideways movement. Row 8 has '######' at columns 12..17 (x = 480..720, y = 320..360).
        Player q;
        q.body.pos = {300, 322};   // level with that block, no gravity applied here
        q.velocity = {0, 0};
        for (int i = 0; i < 120; ++i) { q.velocity.x = 240.0f; move_player(w, q, 1.0f / 60.0f); }
        check(q.body.pos.x + q.body.size.x <= 480.0f + 0.01f, "walking right into a wall tile stops at the wall");
        check(q.body.pos.x + q.body.size.x > 479.0f, "and gets all the way up to it");

        // Hitting a ceiling stops upward movement. Row 3 has '#####' at columns 10..14 (x = 400..600, bottom edge y = 160).
        Player r;
        r.body.pos = {450, 170};
        for (int i = 0; i < 30; ++i) { r.velocity.y = -300.0f; move_player(w, r, 1.0f / 60.0f); }
        check(std::fabs(r.body.pos.y - 160.0f) < 0.01f, "head stops against the underside of a block");
        check(r.velocity.y == 0.0f, "and the upward speed is cancelled");

        // The guide caps the fall speed at 900 px/s and the frame time at 1/30 s:
        // the most you can move in one step is then 30 px, less than a 40 px tile.
        Player s;
        s.body.pos = {100, 330};
        s.velocity.y = 900.0f;
        move_player(w, s, 1.0f / 30.0f);
        check(s.body.pos.y + s.body.size.y <= 9 * kTile + 0.01f, "at the capped speed, a fall still stops at the floor");

        // Without the cap, the same code tunnels. This is the limit of "move, then fix the overlap":
        // it only sees where you ended up, not what you passed through.
        Player t;
        t.body.pos = {100, 330};
        t.velocity.y = 3000.0f;
        move_player(w, t, 1.0f / 30.0f);
        check(t.body.pos.y > 9 * kTile, "an uncapped 3000 px/s fall skips straight through the floor (why the cap exists)");
    }

    // --- guide/08: collision helpers ---------------------------------------
    {
        using namespace collision_example;
        check(circles_overlap({0, 0}, 5, {8, 0}, 5), "circles overlap when closer than the radii added");
        check(!circles_overlap({0, 0}, 5, {10, 0}, 5), "circles exactly touching do not overlap");
        check(!circles_overlap({0, 0}, 5, {30, 0}, 5), "far circles do not overlap");

        const Rect r{{10, 10}, {20, 20}};
        check(circle_hits_rect({20, 20}, 3, r), "circle with its center inside the rect hits it");
        check(circle_hits_rect({5, 20}, 6, r), "circle poking in from the side hits it");
        check(!circle_hits_rect({5, 20}, 4, r), "circle just short of the side does not");
        check(!circle_hits_rect({2, 2}, 5, r), "circle near a corner is measured to the corner, not the box");
        check(circle_hits_rect({6, 6}, 6, r), "circle reaching the corner point hits it");

        const Rect wall{{100, 100}, {20, 100}};
        Rect m{{85, 120}, {20, 20}};   // overlaps the wall's left side by 5
        push_out(m, wall);
        check(!m.overlaps(wall) && std::fabs(m.pos.x - 80.0f) < 0.01f, "pushed out to the left, the shortest way");
        Rect m2{{115, 120}, {20, 20}}; // overlaps the right side by 5
        push_out(m2, wall);
        check(!m2.overlaps(wall) && std::fabs(m2.pos.x - 120.0f) < 0.01f, "pushed out to the right");
        Rect m3{{100, 85}, {20, 20}};  // overlaps the top by 5
        push_out(m3, wall);
        check(!m3.overlaps(wall) && std::fabs(m3.pos.y - 80.0f) < 0.01f, "pushed out upward");
        Rect far{{300, 300}, {20, 20}};
        push_out(far, wall);
        check(far.pos.x == 300 && far.pos.y == 300, "a rect that isn't touching doesn't move");
    }

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
