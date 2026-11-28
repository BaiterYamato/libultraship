#pragma once

#include <algorithm>
#include <cmath>

// Weight of the flowing detail against the static one (FlowDetail in fog_volume_shader.h) for each cell of the
// nested fine grid. The target is the shader's speed ramp (0.05..0.4 u/s). A Stable Fluids pressure solve answers
// Link's first step with 0.1-1 u/s across the whole window within a few ticks; following that at once swapped the
// static noise for the flowing one far from Link in a frame or two. A weak flow may move the weight only by
// 1/FogFlowPresenceSlowSeconds per second; a strong one (the wake, 8+ u/s) keeps up within
// FogFlowPresenceFastSeconds. Header-only so the backend and the shader tests share it.
namespace Fast {

inline constexpr float FogFlowPresenceSlowSeconds = 3.0f;
inline constexpr float FogFlowPresenceFastSeconds = 0.15f;

inline float FogFlowSmoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Weight for a cell that has no history (new window, new stream, or a cell that just entered the window).
inline float FogFlowPresenceTarget(float vx, float vz) {
    return FogFlowSmoothstep(0.05f, 0.4f, std::hypot(vx, vz));
}

// Weight after `seconds` for a cell whose weight was `previous` and whose velocity is now (vx, vz).
inline float AdvanceFogFlowPresence(float previous, float vx, float vz, float seconds) {
    const float speed = std::hypot(vx, vz);
    const float target = FogFlowSmoothstep(0.05f, 0.4f, speed);
    const float time =
        std::lerp(FogFlowPresenceSlowSeconds, FogFlowPresenceFastSeconds, FogFlowSmoothstep(2.0f, 8.0f, speed));
    const float step = std::max(seconds, 0.0f) / time;
    return std::clamp(target, previous - step, previous + step);
}

} // namespace Fast
