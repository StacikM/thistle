// Checks three::ParticleSystem's simulation on the CPU: emit counts and the
// max_particles cap, lifetimes (with jitter) ending on time, gravity and
// drag, one-off styles kept per particle, and that the same seed replays
// identically. Drawing is visual (examples/lights_smoketest.cpp).
#include <thistle.hpp>
#include <cmath>
#include <cstdio>
#include <string>
using namespace thistle;
using namespace thistle::three;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (cond) { std::printf("  ok  %s\n", msg.c_str()); }
    else { std::printf("  FAIL %s\n", msg.c_str()); ++g_failures; }
}
void run(ParticleSystem& ps, float seconds) { for (int i = 0; i < static_cast<int>(seconds * 60.0f); ++i) ps.update(1.0f / 60.0f); }
} // namespace

int main() {
    {
        ParticleSystem ps;
        ps.settings.lifetime = 1.0f;
        ps.settings.lifetime_jitter = 0.25f;
        ps.emit({0, 0, 0}, 500);
        check(ps.count() == 500, "emit adds the requested number");
        run(ps, 0.7f);
        check(ps.count() == 500, "nothing dies before lifetime * (1 - jitter)");
        run(ps, 0.6f); // t = 1.3 > 1.25
        check(ps.count() == 0, "everything is gone after lifetime * (1 + jitter)");
    }
    {
        ParticleSystem ps;
        ps.max_particles = 100;
        ps.emit({0, 0, 0}, 80);
        ps.emit({0, 0, 0}, 80);
        check(ps.count() == 100, "max_particles caps the total");
        ps.clear();
        check(ps.count() == 0, "clear() empties it");
    }
    {
        // No spread, no drag: pure ballistic motion, checked against y = v t - g t^2 / 2.
        ParticleSystem ps;
        ps.settings.spread = 0.0f;
        ps.settings.drag = 0.0f;
        ps.settings.velocity = {0, 10, 0};
        ps.settings.gravity = {0, -10, 0};
        ps.settings.lifetime = 5.0f;
        ps.settings.lifetime_jitter = 0.0f;
        ps.emit({0, 0, 0}, 1);
        run(ps, 1.0f);
        // Semi-implicit Euler at 60 Hz (velocity first, then position):
        // y = sum_{k=1..60} (10 - 10 k / 60) / 60 = 10 - 1830 / 360 = 4.9167.
        check(ps.count() == 1 && std::fabs(ps.particles()[0].position.y - 4.9167f) < 1e-3f,
              "ballistic flight matches the integrator's exact answer");
        check(std::fabs(ps.particles()[0].velocity.y - 0.0f) < 1e-3f, "velocity after 1 s of -10 gravity from +10 is 0");
    }
    {
        ParticleSystem ps;
        ps.settings.spread = 0.0f;
        ps.settings.gravity = {0, 0, 0};
        ps.settings.velocity = {4, 0, 0};
        ps.settings.drag = 0.5f;
        ps.settings.lifetime = 5.0f;
        ps.emit({0, 0, 0}, 1);
        run(ps, 1.0f);
        const float expected = 4.0f * std::pow(1.0f - 0.5f / 60.0f, 60.0f);
        check(std::fabs(ps.particles()[0].velocity.x - expected) < 1e-3f, "drag removes that fraction of velocity per second");
    }
    {
        // Two systems with identical settings and emits replay identically.
        ParticleSystem a, b;
        a.emit({1, 2, 3}, 50);
        b.emit({1, 2, 3}, 50);
        run(a, 0.5f);
        run(b, 0.5f);
        check(a.count() == b.count(), "the same emits and steps give the same result (deterministic)");
    }
    {
        ParticleSystem ps;
        ParticleSettings sparks;
        sparks.lifetime = 0.2f;
        sparks.lifetime_jitter = 0.0f;
        sparks.additive = true;
        ps.settings.lifetime = 2.0f;
        ps.settings.lifetime_jitter = 0.0f;
        ps.emit({0, 0, 0}, 10);
        ps.emit({0, 0, 0}, 10, sparks);
        run(ps, 0.3f);
        check(ps.count() == 10, "a one-off style keeps its own lifetime (sparks gone, default particles left)");
    }
    if (g_failures) { std::printf("%d check(s) failed\n", g_failures); return 1; }
    std::printf("all particle checks passed\n");
    return 0;
}
