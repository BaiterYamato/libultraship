#include "mip_gen.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace Fast {
namespace MipGen {
namespace {

size_t ByteCount(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        throw std::invalid_argument("zero mip dimensions");
    }
    if (size_t(width) > std::numeric_limits<size_t>::max() / 4 / height) {
        throw std::length_error("mip dimensions overflow");
    }
    return size_t(width) * height * 4;
}

uint8_t Byte(double value) {
    return static_cast<uint8_t>(std::max(0.0, std::min(255.0, std::floor(value + 0.5))));
}

Level Box(const Level& src) {
    Level dst{ std::max(1u, src.width / 2), std::max(1u, src.height / 2), {} };
    dst.rgba.resize(ByteCount(dst.width, dst.height));
    // Area-weighted boxes cover the entire source, including odd NPOT edges.
    for (uint32_t y = 0; y < dst.height; ++y) {
        const double y0 = double(y) * src.height / dst.height;
        const double y1 = double(y + 1) * src.height / dst.height;
        for (uint32_t x = 0; x < dst.width; ++x) {
            const double x0 = double(x) * src.width / dst.width;
            const double x1 = double(x + 1) * src.width / dst.width;
            double rgb[3] = {}, plain[3] = {}, alpha = 0.0, area = 0.0;
            for (uint32_t sy = uint32_t(y0); sy < uint32_t(std::ceil(y1)); ++sy) {
                for (uint32_t sx = uint32_t(x0); sx < uint32_t(std::ceil(x1)); ++sx) {
                    const double weight = (std::min(x1, double(sx + 1)) - std::max(x0, double(sx))) *
                                          (std::min(y1, double(sy + 1)) - std::max(y0, double(sy)));
                    const size_t p = (size_t(sy) * src.width + sx) * 4;
                    const double a = src.rgba[p + 3];
                    alpha += a * weight;
                    area += weight;
                    for (int c = 0; c < 3; ++c) {
                        rgb[c] += src.rgba[p + c] * a * weight;
                        plain[c] += src.rgba[p + c] * weight;
                    }
                }
            }
            const size_t p = (size_t(y) * dst.width + x) * 4;
            for (int c = 0; c < 3; ++c) {
                dst.rgba[p + c] = Byte(alpha > 0.0 ? rgb[c] / alpha : plain[c] / area);
            }
            dst.rgba[p + 3] = Byte(alpha / area);
        }
    }
    return dst;
}

uint32_t TieKey(size_t index) {
    // Deterministic spatial tie-break: uniform checkerboards must not become
    // entirely solid/empty, nor choose a single contiguous row when alpha ties.
    uint32_t h = static_cast<uint32_t>(index);
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    return h ^ (h >> 16);
}

void PreserveCoverage(Level& level, double baseCoverage) {
    const size_t pixels = level.rgba.size() / 4;
    const size_t target = static_cast<size_t>(std::floor(baseCoverage * pixels + 0.5));
    std::vector<size_t> order(pixels);
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const uint8_t aa = level.rgba[a * 4 + 3], ab = level.rgba[b * 4 + 3];
        if (aa != ab) {
            return aa > ab;
        }
        const uint32_t ha = TieKey(a), hb = TieKey(b);
        return ha != hb ? ha < hb : a < b;
    });
    // Minimal projection onto two disjoint alpha intervals. This handles equal
    // alphas (a global scale cannot preserve 50% of a uniform checkerboard).
    // The intervals straddle both Fast3D discard thresholds and the usual 0.5.
    for (size_t i = 0; i < pixels; ++i) {
        uint8_t& a = level.rgba[order[i] * 4 + 3];
        a = i < target ? std::max<uint8_t>(128, a) : std::min<uint8_t>(7, a);
    }
}

} // namespace

std::vector<Level> Generate(const uint8_t* rgba, uint32_t width, uint32_t height) {
    const size_t bytes = ByteCount(width, height);
    if (rgba == nullptr) {
        throw std::invalid_argument("null mip pixels");
    }
    bool binary = true;
    size_t covered = 0;
    for (size_t i = 3; i < bytes; i += 4) {
        binary = binary && (rgba[i] == 0 || rgba[i] == 255);
        covered += rgba[i] == 255;
    }
    const bool cutout = binary && covered != 0 && covered != bytes / 4;
    const double coverage = double(covered) / double(bytes / 4);
    std::vector<Level> result;
    result.push_back({ width, height, std::vector<uint8_t>(rgba, rgba + bytes) });
    Level raw = result.front();
    while (raw.width > 1 || raw.height > 1) {
        raw = Box(raw);
        result.push_back(raw);
        if (cutout) {
            PreserveCoverage(result.back(), coverage);
        }
        // Feed uncorrected alpha into the next box; corrections must not drift
        // the coverage target or the area-weighted color/alpha averages.
    }
    return result;
}

} // namespace MipGen
} // namespace Fast
