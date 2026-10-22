// Checks Physics::remove() on the 2D (Box2D) wrapper: a removed body stops
// moving and colliding, its handle goes stale without touching another
// body's, and removing from inside on_collision (while Box2D's world is
// locked) doesn't crash and frees it after step(). No App/window/GPU.
#include <thistle.hpp>
#include <cstdio>
using namespace thistle;

namespace {
int g_failures = 0;
void check(bool cond, const char* msg) {
    if (cond) { std::printf("  ok  %s\n", msg); }
    else { std::printf("  FAIL %s\n", msg); ++g_failures; }
}
} // namespace

int main() {
    {
        Physics phys({0, 980});
        Body ground = phys.add_static_box({0, 200}, {1000, 20});
        Body a = phys.add_dynamic_box({0, 0}, {20, 20});
        Body b = phys.add_dynamic_box({100, 0}, {20, 20});
        phys.remove(a);
        for (int i = 0; i < 60; ++i) phys.step(1.0f / 60.0f);
        check(phys.position(a).x == 0 && phys.position(a).y == 0, "removed body reads as empty");
        check(phys.velocity(a).y == 0, "removed body has no velocity");
        check(phys.position(b).y > 150, "other body still falls onto the ground");
        check(!phys.touching(a, ground), "removed body touches nothing");
        phys.remove(a);
        Body c = phys.add_dynamic_circle({300, 0}, 10);
        check(c.id != a.id, "a removed body's id isn't reused");
        phys.set_velocity(a, {50, 0});
        phys.apply_impulse(a, {50, 0});
        check(phys.position(a).x == 0, "calls on a removed body do nothing");
        RayHit hit = phys.raycast({0, -100}, {0, 300});
        check(hit.hit && hit.body == ground, "raycast passes where a removed body was");
    }
    {
        Physics phys({0, 980});
        Body ground = phys.add_static_box({0, 200}, {1000, 20});
        Body coin = phys.add_sensor_box({0, 100}, {40, 40});
        Body ball = phys.add_dynamic_circle({0, 0}, 10);
        int coin_hits = 0;
        phys.on_collision([&](Body x, Body y) {
            if (x == coin || y == coin) { ++coin_hits; phys.remove(coin); }
        });
        for (int i = 0; i < 120; ++i) phys.step(1.0f / 60.0f);
        check(coin_hits == 1, "removing inside on_collision reports the pickup once");
        check(phys.position(ball).y > 150, "ball falls through where the coin was and lands");
        check(phys.touching(ball, ground), "ball rests on the ground");
    }
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("all passed\n");
    return 0;
}
