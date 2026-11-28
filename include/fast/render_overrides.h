#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

// Session overrides for generic renderer integer parameters. A provider (for example a mod) sets one while it is
// active; the value lives only in memory and never enters the console-variable map, so saving the config cannot
// persist it and the base value the user saved stays untouched. Only the parameters listed below accept an override,
// and only renderer code that reads through GetRenderIntOverride sees it. Value 0 means "no override".
// Header-only on purpose: the host test targets compile this without linking libultraship.
namespace Fast {
namespace RenderOverrides {
inline constexpr const char* kIntParams[] = { "gDeveloperTools.FogFlowDetailMaxSpeed" };

inline std::atomic<int32_t>* Values() {
    static std::atomic<int32_t> values[std::size(kIntParams)]{};
    return values;
}

inline int IndexOf(const char* name) {
    if (name == nullptr) {
        return -1;
    }
    for (size_t i = 0; i < std::size(kIntParams); ++i) {
        if (std::strcmp(name, kIntParams[i]) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
} // namespace RenderOverrides

inline bool IsRenderIntOverridable(const char* name) {
    return RenderOverrides::IndexOf(name) >= 0;
}

// Returns false, storing nothing, for parameters without override support or negative values. 0 clears.
inline bool SetRenderIntOverride(const char* name, int32_t value) {
    const int index = RenderOverrides::IndexOf(name);
    if (index < 0 || value < 0) {
        return false;
    }
    RenderOverrides::Values()[index].store(value, std::memory_order_relaxed);
    return true;
}

// Writes the active override into value; false when the parameter has none.
inline bool GetRenderIntOverride(const char* name, int32_t& value) {
    const int index = RenderOverrides::IndexOf(name);
    if (index < 0) {
        return false;
    }
    const int32_t current = RenderOverrides::Values()[index].load(std::memory_order_relaxed);
    if (current == 0) {
        return false;
    }
    value = current;
    return true;
}

inline void ClearRenderIntOverrides() {
    for (size_t i = 0; i < std::size(RenderOverrides::kIntParams); ++i) {
        RenderOverrides::Values()[i].store(0, std::memory_order_relaxed);
    }
}
} // namespace Fast
