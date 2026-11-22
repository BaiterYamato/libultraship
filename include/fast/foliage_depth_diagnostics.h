#pragma once

// Estruturas fixas, sem D3D nem alocacao: tambem usadas pelos testes isolados.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Fast::FoliageDepth {
constexpr std::size_t RingSize = 8;
constexpr std::size_t MaxIntervals = 256;
constexpr std::size_t BatchSize = 300;

struct Counts {
    uint64_t draws = 0, triangles = 0;
    void Draw(std::size_t tris) { ++draws; triangles += tris; }
};

struct CountBatch {
    std::size_t frames = 0;
    Counts total{}, maximum{};
    void Add(const Counts& value) {
        ++frames;
        total.draws += value.draws;
        total.triangles += value.triangles;
        maximum.draws = (std::max)(maximum.draws, value.draws);
        maximum.triangles = (std::max)(maximum.triangles, value.triangles);
    }
    void Reset() { *this = {}; }
};

// Uma amostra e a SOMA dos intervalos, nunca o envelope entre primeiro/ultimo draw.
struct TickSum {
    uint64_t ticks = 0;
    bool valid = true;
    void Add(uint64_t begin, uint64_t end) {
        if (end < begin || end - begin > (std::numeric_limits<uint64_t>::max)() - ticks)
            valid = false;
        else
            ticks += end - begin;
    }
};

struct Sample {
    uint64_t frame = UINT64_MAX;
    Counts counts{};
    double milliseconds = 0;
    uint32_t width = 0, height = 0, msaa = 0, quality = 0, format = 0;
    unsigned mode = 0;
    bool valid = false, overflow = false;
};
} // namespace Fast::FoliageDepth
