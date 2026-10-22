// Random.h - the bot's random source (jitter for gates, clicks, re-aims).
#pragma once

#include "Geometry.h"

#include <cmath>
#include <cstdint>
#include <random>

namespace fb {

class Rng {
public:
    explicit Rng(uint64_t seed = 0) : m_gen(seed ? seed : std::random_device{}()) {}

    void Seed(uint64_t seed) { m_gen.seed(seed); }

    // Uniform integer in [lo, hi] (hi < lo returns lo).
    int UniformInt(int lo, int hi) {
        if (hi <= lo) return lo;
        return std::uniform_int_distribution<int>(lo, hi)(m_gen);
    }

    // Uniform float in [lo, hi).
    float UniformFloat(float lo, float hi) {
        if (!(hi > lo)) return lo;
        return std::uniform_real_distribution<float>(lo, hi)(m_gen);
    }

    bool Chance(float p) { return UniformFloat(0.f, 1.f) < p; }

    // Centre-biased 2D offset inside a circle of `radius`: the distance follows a
    // triangular distribution peaking at 0, so most clicks land near the centre.
    Vec2 CenterBiased(float radius) {
        if (radius <= 0.f) return {0.f, 0.f};
        const float r = radius * (1.f - std::sqrt(1.f - UniformFloat(0.f, 1.f)));
        const float a = UniformFloat(0.f, 2.f * kPi);
        return {std::cos(a) * r, std::sin(a) * r};
    }

    std::mt19937_64& Engine() { return m_gen; }

private:
    std::mt19937_64 m_gen;
};

}  // namespace fb
