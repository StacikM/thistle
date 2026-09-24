#include <thistle.hpp>

#include <algorithm>
#include <cmath>

namespace thistle::three {

namespace {

bool same(const rgba& a, const rgba& b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

bool same_look(const ParticleSettings& a, const ParticleSettings& b) {
    return a.texture.id == b.texture.id && same(a.start_color, b.start_color) && same(a.end_color, b.end_color) &&
           a.start_size == b.start_size && a.end_size == b.end_size && a.lifetime == b.lifetime &&
           a.lifetime_jitter == b.lifetime_jitter && a.velocity == b.velocity && a.spread == b.spread &&
           a.gravity == b.gravity && a.drag == b.drag && a.spin == b.spin && a.additive == b.additive;
}

float next_unit(uint32_t& state) {
    // xorshift32: cheap, and deterministic per system (replays look the same).
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state & 0xFFFFFF) / static_cast<float>(0x1000000);
}

} // namespace

void ParticleSystem::emit(vec3 position, int count, const ParticleSettings& look) {
    if (count <= 0) return;
    // Particles keep pointing at their look for their whole life, so each
    // distinct style emitted gets its own stored copy (shared by equal ones).
    const ParticleSettings* stored = nullptr;
    for (const auto& l : looks_) {
        if (same_look(*l, look)) { stored = l.get(); break; }
    }
    if (!stored) {
        looks_.push_back(std::make_unique<ParticleSettings>(look));
        stored = looks_.back().get();
    }
    for (int i = 0; i < count && particles_.size() < max_particles; ++i) {
        // Uniform direction on a sphere, uniform speed up to `spread`.
        const float z = next_unit(seed_) * 2.0f - 1.0f;
        const float a = next_unit(seed_) * 2.0f * pi;
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const vec3 dir{r * std::cos(a), z, r * std::sin(a)};
        Particle p;
        p.position = position;
        p.velocity = look.velocity + dir * (look.spread * next_unit(seed_));
        p.age = 0.0f;
        p.lifetime = std::max(0.01f, look.lifetime * (1.0f + look.lifetime_jitter * (next_unit(seed_) * 2.0f - 1.0f)));
        p.rotation = next_unit(seed_) * 2.0f * pi;
        p.spin = look.spin * (next_unit(seed_) * 2.0f - 1.0f);
        p.look = stored;
        particles_.push_back(p);
    }
}

void ParticleSystem::update(float dt) {
    if (dt <= 0.0f) return;
    for (size_t i = 0; i < particles_.size();) {
        Particle& p = particles_[i];
        p.age += dt;
        if (p.age >= p.lifetime) {
            p = particles_.back(); // order doesn't matter: drawing sorts anyway
            particles_.pop_back();
            continue;
        }
        p.velocity += p.look->gravity * dt;
        p.velocity = p.velocity * std::max(0.0f, 1.0f - p.look->drag * dt);
        p.position += p.velocity * dt;
        p.rotation += p.spin * dt;
        ++i;
    }
    // Forget styles nothing uses any more, so a system that emits many
    // one-off styles over a long game doesn't keep them all forever.
    if (looks_.size() > 8) {
        looks_.erase(std::remove_if(looks_.begin(), looks_.end(),
                                    [&](const std::unique_ptr<ParticleSettings>& l) {
                                        return std::none_of(particles_.begin(), particles_.end(),
                                                            [&](const Particle& p) { return p.look == l.get(); });
                                    }),
                     looks_.end());
    }
}

} // namespace thistle::three
