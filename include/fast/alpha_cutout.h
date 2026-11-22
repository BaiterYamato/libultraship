#pragma once

#include <cstddef>
#include <cstdint>

namespace Fast::AlphaCutout {

// Statistics of the decoded RGBA base level, before filtering/mip generation.
// This describes coverage, never a game object or the meaning of its texture.
struct Statistics {
    uint64_t pixels = 0;
    uint64_t holes = 0;
    uint64_t solid = 0;

    bool IsCutout() const {
        return pixels >= 16 && holes * 100 >= pixels * 2 && solid * 100 >= pixels * 2 &&
               (pixels - holes - solid) * 100 <= pixels * 5;
    }
};

inline Statistics Analyze(const uint8_t* rgba, size_t pixels) {
    Statistics result{};
    if (!rgba)
        return result;
    result.pixels = pixels;
    for (size_t i = 0; i < pixels; ++i) {
        const auto alpha = rgba[i * 4 + 3];
        result.holes += alpha <= 8;
        result.solid += alpha >= 247;
    }
    return result;
}

enum class Source { Zero, One, Texture0, Texture1, Unknown };

// Deliberately conservative symbolic reduction of (a - b) * c + d.
// Unknown opacities/expressions are rejected instead of guessed from RGB usage.
inline Source Formula(Source a, Source b, Source c, Source d) {
    if (c == Source::Zero || (a != Source::Unknown && a == b))
        return d;
    if (b == Source::Zero && d == Source::Zero) {
        if (a == Source::One)
            return c;
        if (c == Source::One)
            return a;
    }
    return Source::Unknown;
}

} // namespace Fast::AlphaCutout
