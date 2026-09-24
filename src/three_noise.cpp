#include <thistle.hpp>

#include <cmath>

namespace thistle::three {

namespace {

// Gradients are picked by hashing the lattice point and the seed, rather
// than from Perlin's shuffled 256-entry table: any seed works with no setup
// and there's no table to share between threads. Integer math only, so the
// result is bit-identical on every compiler and CPU.
uint32_t hash(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = seed * 0x9E3779B9u;
    h ^= static_cast<uint32_t>(x) * 0x85EBCA6Bu;
    h ^= static_cast<uint32_t>(y) * 0xC2B2AE35u;
    h ^= static_cast<uint32_t>(z) * 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
float mixf(float a, float b, float t) { return a + (b - a) * t; }

float grad2(uint32_t h, float x, float y) {
    // 8 directions around the circle (unit axes and diagonals).
    constexpr float d = 0.70710678f;
    static constexpr float gx[8] = {1, -1, 0, 0, d, -d, d, -d};
    static constexpr float gy[8] = {0, 0, 1, -1, d, d, -d, -d};
    const uint32_t i = h & 7;
    return gx[i] * x + gy[i] * y;
}

float grad3(uint32_t h, float x, float y, float z) {
    // Perlin's 12 cube-edge directions (16 slots, 4 repeated).
    switch (h & 15) {
        case 0: return x + y;   case 1: return -x + y;  case 2: return x - y;   case 3: return -x - y;
        case 4: return x + z;   case 5: return -x + z;  case 6: return x - z;   case 7: return -x - z;
        case 8: return y + z;   case 9: return -y + z;  case 10: return y - z;  case 11: return -y - z;
        case 12: return x + y;  case 13: return -y + z; case 14: return -x + y; default: return -y - z;
    }
}

} // namespace

float perlin(float x, float y, uint32_t seed) {
    const float fx = std::floor(x), fy = std::floor(y);
    const int32_t ix = static_cast<int32_t>(fx), iy = static_cast<int32_t>(fy);
    const float dx = x - fx, dy = y - fy;
    const float n00 = grad2(hash(ix, iy, 0, seed), dx, dy);
    const float n10 = grad2(hash(ix + 1, iy, 0, seed), dx - 1.0f, dy);
    const float n01 = grad2(hash(ix, iy + 1, 0, seed), dx, dy - 1.0f);
    const float n11 = grad2(hash(ix + 1, iy + 1, 0, seed), dx - 1.0f, dy - 1.0f);
    const float u = fade(dx), v = fade(dy);
    // 2D Perlin peaks at +-sqrt(1/2); scale so the range is about -1..1.
    return 1.41421356f * mixf(mixf(n00, n10, u), mixf(n01, n11, u), v);
}

float perlin3(float x, float y, float z, uint32_t seed) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int32_t ix = static_cast<int32_t>(fx), iy = static_cast<int32_t>(fy), iz = static_cast<int32_t>(fz);
    const float dx = x - fx, dy = y - fy, dz = z - fz;
    float n[8];
    for (int i = 0; i < 8; ++i) {
        const int ox = i & 1, oy = (i >> 1) & 1, oz = (i >> 2) & 1;
        n[i] = grad3(hash(ix + ox, iy + oy, iz + oz, seed), dx - ox, dy - oy, dz - oz);
    }
    const float u = fade(dx), v = fade(dy), w = fade(dz);
    const float x00 = mixf(n[0], n[1], u), x10 = mixf(n[2], n[3], u);
    const float x01 = mixf(n[4], n[5], u), x11 = mixf(n[6], n[7], u);
    return mixf(mixf(x00, x10, v), mixf(x01, x11, v), w);
}

float fbm(float x, float y, int octaves, uint32_t seed, float lacunarity, float gain) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        // Each octave gets its own seed, so its features don't line up
        // with the octave below (which shows as grid-aligned artifacts).
        sum += amp * perlin(x * freq, y * freq, seed + static_cast<uint32_t>(i) * 1013u);
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

float fbm3(float x, float y, float z, int octaves, uint32_t seed, float lacunarity, float gain) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * perlin3(x * freq, y * freq, z * freq, seed + static_cast<uint32_t>(i) * 1013u);
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

float ridged(float x, float y, int octaves, uint32_t seed) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        const float r = 1.0f - std::fabs(perlin(x * freq, y * freq, seed + static_cast<uint32_t>(i) * 1013u));
        sum += amp * r * r;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

} // namespace thistle::three
