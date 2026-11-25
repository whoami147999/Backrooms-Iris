// Backrooms Iris - deterministic integer hashing. The whole world is a pure function of
// (seed, coordinates), so every machine (client, Vita, server) generates the same level.
#pragma once
#include <cstdint>

namespace iris {

inline uint32_t mix32(uint32_t x) {
    // lowbias32 by Chris Wellons
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline uint32_t hash3(uint32_t seed, int a, int b, uint32_t salt) {
    uint32_t h = mix32(seed ^ 0x9e3779b9U);
    h = mix32(h ^ (uint32_t)a);
    h = mix32(h ^ (uint32_t)b * 0x85ebca6bU);
    h = mix32(h ^ salt * 0xc2b2ae35U);
    return h;
}

// uniform float in [0, 1)
inline float hash01(uint32_t seed, int a, int b, uint32_t salt) {
    return (hash3(seed, a, b, salt) >> 8) * (1.0f / 16777216.0f);
}

// Small fast PRNG for things that don't need to be coordinate-addressable.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float f01() { return (next() >> 8) * (1.0f / 16777216.0f); }
    float range(float a, float b) { return a + (b - a) * f01(); }
};

} // namespace iris
