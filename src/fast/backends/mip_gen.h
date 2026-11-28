#pragma once

#include <cstdint>
#include <vector>

namespace Fast {
namespace MipGen {

struct Level {
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> rgba;
};

// RGBA8 linear UNORM. Level zero is byte-identical to the input.
// Binary cutouts preserve base-level coverage to the nearest whole destination
// texel at alpha thresholds 8/256, 0.19 and 0.5. Continuous alpha is not remapped.
// Throws invalid_argument/length_error for invalid dimensions or null input.
std::vector<Level> Generate(const uint8_t* rgba, uint32_t width, uint32_t height);

} // namespace MipGen
} // namespace Fast
