#pragma once

#include <cmath>

namespace Fast {

// Row-vector projection. The scene shader addresses the entire depth texture.
inline void MapProjectionToTarget(float (&projection)[4][4], float scaleX, float scaleY, float offsetX,
                                  float offsetY) {
    for (auto& row : projection) {
        row[0] = scaleX * row[0] + offsetX * row[3];
        row[1] = scaleY * row[1] + offsetY * row[3];
    }
}

// Applies Fast3D's horizontal clip-space aspect scale to a 4:3 row-vector projection matrix.
inline void ScaleProjectionAspectX(float (&projection)[4][4], float aspectScale) {
    if (!std::isfinite(aspectScale) || std::abs(aspectScale) <= 0.0001f)
        return;
    for (auto& row : projection)
        row[0] *= aspectScale;
}

// A ray inverts the same horizontal clip-space scale used to draw the scene.
inline float MapFogRayScaleX(float rayScaleX, float aspectScale) {
    if (!std::isfinite(aspectScale) || std::abs(aspectScale) <= 0.0001f)
        return rayScaleX;
    return rayScaleX / aspectScale;
}

} // namespace Fast
