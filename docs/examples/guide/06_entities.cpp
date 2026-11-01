// Guide 6: lots of things. Blocks fall from the top; click one to pop it. This
// is the pattern for bullets, enemies, pickups: a struct, a std::vector of
// them, a loop that updates, and a loop that removes the dead ones.
#include <thistle.hpp>
#include <algorithm>
#include <random>
#include <string>
#include <vector>
using namespace thistle;

struct Block {
    Rect rect;
    float fall_speed;
    rgba color;
    bool dead = false;
};

THISTLE_MAIN {
    App app{{.title = "Entities", .width = 1280, .height = 720}};
    load_font("assets/font.ttf");

    std::mt19937 rng{12345};
    auto random_between = [&](float lo, float hi) {
        return std::uniform_real_distribution<float>(lo, hi)(rng);
    };

    std::vector<Block> blocks;
    float spawn_timer = 0.0f;   // counts down; spawn when it reaches 0
    int popped = 0;
    int missed = 0;

    app.update([&](Frame f) {
        // Spawn on a timer.
        spawn_timer -= f.dt;
        if (spawn_timer <= 0.0f) {
            spawn_timer += 0.45f;   // += not = 0.45 keeps the rhythm steady even on a slow frame
            const float w = random_between(40, 90);
            blocks.push_back({{{random_between(0, f.width - w), -w}, {w, w}},
                              random_between(120, 320),
                              rgb(random_between(0.4f, 1.0f), random_between(0.4f, 1.0f), random_between(0.4f, 1.0f))});
        }

        // Update.
        for (Block& b : blocks) {
            b.rect.pos.y += b.fall_speed * f.dt;
            if (b.rect.pos.y > f.height) { b.dead = true; ++missed; }
        }

        // Click pops the top-most block under the cursor (the last one drawn).
        if (f.mouse_pressed(Mouse::Left)) {
            for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
                if (!it->dead && it->rect.contains(f.mouse())) {
                    it->dead = true;
                    ++popped;
                    break;
                }
            }
        }

        // Remove everything marked dead, in one pass, AFTER the loops above.
        // Erasing from a vector while looping over it is how you get crashes.
        blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [](const Block& b) { return b.dead; }),
                     blocks.end());

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        for (const Block& b : blocks) f.rect(b.rect.pos, b.rect.size, b.color);
        f.text("popped " + std::to_string(popped) + "   missed " + std::to_string(missed) + "   alive " +
                   std::to_string(blocks.size()),
               Anchor::TopLeft, {20, 20}, {.size = 26});
    });

    THISTLE_RUN(app);
}
