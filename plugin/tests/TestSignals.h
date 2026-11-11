#pragma once

#include <cmath>
#include <random>
#include <vector>

namespace griddy::testing
{

/** Deterministic music like test signal: random notes with harmonics and a little noise.
    Different seeds give different "performances", so their fingerprints differ. */
inline std::vector<float> makeMusic (double sr, double seconds, unsigned seed, float level = 1.0f)
{
    std::mt19937 rng (seed);
    std::vector<float> v ((size_t) (sr * seconds), 0.0f);
    size_t pos = 0;

    while (pos < v.size())
    {
        const auto f0 = 110.0 * std::pow (2.0, (double) (rng() % 36) / 12.0);
        const auto len = (size_t) (sr * (0.12 + 0.3 * (double) (rng() % 100) / 100.0));

        for (size_t i = 0; i < len && pos + i < v.size(); ++i)
        {
            const auto t = (double) i / sr;
            const auto env = std::exp (-3.0 * t / ((double) len / sr));
            float s = 0.0f;
            for (int h = 1; h <= 5; ++h)
                s += (float) std::sin (6.283185307 * f0 * h * t) / (float) h;
            v[pos + i] += 0.2f * s * (float) env;
        }

        pos += (size_t) ((double) len * 0.6);
    }

    std::normal_distribution<float> nd (0.0f, 1.0f);
    for (auto& x : v)
        x = level * (x + 0.002f * nd (rng));

    return v;
}

} // namespace griddy::testing
