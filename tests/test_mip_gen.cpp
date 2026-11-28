#include "mip_gen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

static void Check(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

static size_t Count(const std::vector<uint8_t>& rgba, unsigned threshold) {
    size_t n = 0;
    for (size_t i = 3; i < rgba.size(); i += 4) {
        n += rgba[i] >= threshold;
    }
    return n;
}

static void Shape(uint32_t w, uint32_t h, int pattern) {
    std::vector<uint8_t> pixels(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t p = (size_t(y) * w + x) * 4;
            bool solid = pattern == 0 || (pattern == 1 && (x + y) % 2 == 0) ||
                         (pattern == 2 && (x % 7 == 0 || y % 9 == 0)) ||
                         (pattern == 3 && (x * 23 + y * 17) % 101 < 37);
            pixels[p] = 32;
            pixels[p + 1] = 191;
            pixels[p + 2] = 73;
            pixels[p + 3] = solid ? 255 : 0;
        }
    }
    const auto levels = Fast::MipGen::Generate(pixels.data(), w, h);
    const auto repeat = Fast::MipGen::Generate(pixels.data(), w, h);
    Check(levels.front().rgba == pixels, "level zero changed");
    uint32_t ew = w, eh = h;
    for (size_t i = 0; i < levels.size(); ++i) {
        const auto& level = levels[i];
        Check(level.width == ew && level.height == eh, "wrong mip dimensions");
        Check(level.rgba.size() == size_t(ew) * eh * 4, "wrong byte count");
        Check(level.rgba == repeat[i].rgba, "nondeterministic ties");
        const double n = double(ew) * eh;
        for (unsigned cutoff : { 8u, 49u, 128u }) {
            const double base = double(Count(pixels, cutoff)) / (double(w) * h);
            const double coverage = double(Count(level.rgba, cutoff)) / n;
            Check(std::abs(coverage - base) <= 0.5 / n + 1e-12, "cutout coverage drift");
        }
        for (size_t p = 0; p < level.rgba.size(); p += 4) {
            Check(level.rgba[p] == 32 && level.rgba[p + 1] == 191 && level.rgba[p + 2] == 73,
                  "constant RGB changed");
        }
        ew = std::max(1u, ew / 2);
        eh = std::max(1u, eh / 2);
    }
    Check(levels.back().width == 1 && levels.back().height == 1, "incomplete chain");
}

int main() {
    try {
        for (const auto dims : { std::pair<uint32_t, uint32_t>{ 1, 1 }, { 2, 1 }, { 64, 64 }, { 33, 17 },
                                 { 1, 33 }, { 3, 3 }, { 7, 2 } }) {
            for (int pattern = 0; pattern < 5; ++pattern) {
                Shape(dims.first, dims.second, pattern);
            }
        }
        // Opaque box average; no cutout remapping for smooth alpha.
        const uint8_t opaque[] = { 0, 20, 100, 255, 200, 60, 0, 255 };
        auto levels = Fast::MipGen::Generate(opaque, 2, 1);
        Check(levels[1].rgba == std::vector<uint8_t>({ 100, 40, 50, 255 }), "box average");
        const uint8_t smooth[] = { 100, 80, 60, 32, 100, 80, 60, 160 };
        levels = Fast::MipGen::Generate(smooth, 2, 1);
        Check(levels[1].rgba == std::vector<uint8_t>({ 100, 80, 60, 96 }), "smooth alpha remapped");
        const uint8_t fringe[] = { 255, 0, 0, 0, 0, 255, 0, 255 };
        levels = Fast::MipGen::Generate(fringe, 2, 1);
        Check(levels[1].rgba[0] == 0 && levels[1].rgba[1] == 255, "transparent RGB contaminated color");
        // Last NPOT row/column must contribute: 3x3 -> 1x1, not a cropped 2x2.
        std::vector<uint8_t> odd(3 * 3 * 4, 0);
        for (int i = 0; i < 9; ++i) {
            odd[i * 4 + 3] = 255;
            if (i % 3 == 2 || i / 3 == 2) {
                odd[i * 4] = 255;
            }
        }
        levels = Fast::MipGen::Generate(odd.data(), 3, 3);
        Check(levels[1].rgba[0] == 142, "NPOT edge omitted");
        for (int bad = 0; bad < 4; ++bad) {
            bool threw = false;
            try {
                if (bad == 0) Fast::MipGen::Generate(nullptr, 1, 1);
                if (bad == 1) Fast::MipGen::Generate(opaque, 0, 1);
                if (bad == 2) Fast::MipGen::Generate(opaque, 1, 0);
                if (bad == 3) Fast::MipGen::Generate(opaque, UINT32_MAX, UINT32_MAX);
            } catch (const std::invalid_argument&) {
                threw = true;
            } catch (const std::length_error&) {
                threw = true;
            }
            Check(threw, "invalid input accepted");
        }
        std::cout << "PASS: 35 shape/pattern cases; coverage at 8/49/128, deterministic ties, "
                     "RGBA box, smooth alpha, fringes, NPOT edges, invalid inputs\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
