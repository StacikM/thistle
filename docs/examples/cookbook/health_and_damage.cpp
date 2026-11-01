// Recipe: health, damage, invincibility frames, knockback, and healing. Press
// SPACE to get hit, H to heal. After a hit you blink and can't be hurt again
// for a moment (otherwise one touch would drain your whole bar in 6 frames).
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
using namespace thistle;

struct Health {
    float hp = 100.0f, max_hp = 100.0f;
    float invincible = 0.0f;     // seconds left
    bool hurt(float amount) {
        if (invincible > 0.0f || hp <= 0.0f) return false;
        hp = std::max(0.0f, hp - amount);
        invincible = 1.0f;
        return true;
    }
    void heal(float amount) { hp = std::min(max_hp, hp + amount); }
    bool dead() const { return hp <= 0.0f; }
};

THISTLE_MAIN {
    App app{{.title = "Health and damage", .width = 900, .height = 500}};
    load_font("assets/font.ttf");

    Health health;
    vec2 pos{450, 280}, knock{0, 0};
    float shown_hp = 100.0f;   // a second bar that chases the real one, so damage is visible

    app.update([&](Frame f) {
        const float dt = std::min(f.dt, 1.0f / 30.0f);
        health.invincible = std::max(0.0f, health.invincible - dt);

        if (f.key_pressed(Key::Space) && health.hurt(20.0f)) knock = {(pos.x > 450 ? 1.0f : -1.0f) * 600.0f, -200.0f};
        if (f.key_pressed(Key::H)) health.heal(25.0f);
        if (f.key_pressed(Key::R)) health = Health{};

        // Knockback is just a velocity: gravity pulls it down, the ground stops it and slows it.
        knock.y += 1600.0f * dt;
        pos += knock * dt;
        if (pos.y > 280.0f) {
            pos.y = 280.0f;
            knock.y = 0.0f;
            knock.x *= std::max(0.0f, 1.0f - 10.0f * dt);
        }
        pos.x = std::clamp(pos.x, 40.0f, 860.0f);
        shown_hp += (health.hp - shown_hp) * std::min(1.0f, 4.0f * dt);

        f.clear(rgb(0.08f, 0.09f, 0.14f));

        // Blink while invincible: visible on even 0.1 s slices.
        const bool visible = health.invincible <= 0.0f || static_cast<int>(health.invincible * 10.0f) % 2 == 0;
        if (!health.dead() && visible) f.rect(pos - vec2{20, 20}, {40, 40}, rgb(0.35f, 0.75f, 0.95f));
        if (health.dead()) f.text("dead - press R", Anchor::Center, {0, 40}, {.size = 40});

        const Rect bar = f.anchored(Anchor::Top, {500, 28}, {0, 30});
        f.progress_bar(shown_hp / health.max_hp, bar, rgb(0.95f, 0.8f, 0.3f));          // the "damage taken" trail
        f.progress_bar(health.hp / health.max_hp, bar, rgb(0.35f, 0.85f, 0.5f), rgba{0, 0, 0, 0});
        f.text(std::to_string(static_cast<int>(health.hp)) + " / 100", Anchor::Top, {0, 62}, {.size = 22});
        f.text("Space: hit   H: heal   R: reset", Anchor::Bottom, {0, -16}, {.size = 22});
    });

    THISTLE_RUN(app);
}
