#include "fast/screen_projection.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void ExpectNear(float actual, float expected, const char* quantity) {
    if (std::abs(actual - expected) <= 0.0002f)
        return;
    std::cerr << quantity << ": expected " << expected << ", got " << actual << '\n';
    std::exit(EXIT_FAILURE);
}

void CheckProjection(float width, float height, float left, float top, float targetWidth, float targetHeight) {
    const float clipXScale = (4.0f / 3.0f) / (width / height);
    // Row-vector projection, including an off-axis term in clip X to check the whole column.
    constexpr float projectionX = 1.25f, projectionY = 1.75f, offAxisX = 0.125f, depth = 100.0f;
    float projection[4][4] = {
        { projectionX, 0, 0, 0 },
        { 0, projectionY, 0, 0 },
        { offAxisX, 0, -1.0f, -1.0f },
        { 0, 0, -20.0f, 0 },
    };
    Fast::MapProjectionToTarget(projection, clipXScale * width / targetWidth, height / targetHeight,
                                (2 * left + width) / targetWidth - 1, 1 - (2 * top + height) / targetHeight);
    ExpectNear(projection[2][2], -1.0f, "clip Z");
    ExpectNear(projection[2][3], -1.0f, "clip W");
    ExpectNear(projection[3][2], -20.0f, "depth offset");

    // Full-target Policroma camera projection uses ScaleProjectionAspectX with the same clipXScale.
    float policromaProj[4][4] = {
        { projectionX, 0, 0, 0 },
        { 0, projectionY, 0, 0 },
        { offAxisX, 0, -1.0f, -1.0f },
        { 0, 0, -20.0f, 0 },
    };
    Fast::ScaleProjectionAspectX(policromaProj, clipXScale);
    ExpectNear(policromaProj[0][0], projectionX * clipXScale, "policroma proj[0][0]");
    ExpectNear(policromaProj[2][0], offAxisX * clipXScale, "policroma proj[2][0]");
    ExpectNear(policromaProj[1][1], projectionY, "policroma proj[1][1]");
    ExpectNear(policromaProj[2][2], -1.0f, "policroma clip Z");
    ExpectNear(policromaProj[2][3], -1.0f, "policroma clip W");
    ExpectNear(policromaProj[3][2], -20.0f, "policroma depth offset");

    constexpr float viewY = 12.0f;
    for (const float viewX : { -30.0f, 10.0f, 50.0f }) {
        const float spriteX = left + (0.5f + 0.5f * clipXScale * (projectionX * viewX / depth - offAxisX)) * width;
        const float spriteY = top + (0.5f - 0.5f * projectionY * viewY / depth) * height;
        const float clipX = viewX * projection[0][0] - depth * projection[2][0] + projection[3][0];
        const float clipY = viewY * projection[1][1] - depth * projection[2][1] + projection[3][1];
        ExpectNear((0.5f + 0.5f * clipX / depth) * targetWidth, spriteX, "emissive halo X");
        ExpectNear((0.5f - 0.5f * clipY / depth) * targetHeight, spriteY, "emissive halo Y");

        if (left == 0.0f && top == 0.0f && width == targetWidth && height == targetHeight) {
            const float polyClipX = viewX * policromaProj[0][0] - depth * policromaProj[2][0] + policromaProj[3][0];
            ExpectNear((0.5f + 0.5f * polyClipX / depth) * targetWidth, spriteX, "policroma halo X");
        }

        // FogVolume uses a centered perspective. Its UV is relative to the viewport.
        const float fogNdcX = clipXScale * projectionX * viewX / depth;
        const float fogX = left + (0.5f + 0.5f * fogNdcX) * width;
        const float rayScaleX = Fast::MapFogRayScaleX(1.0f / projectionX, clipXScale);
        const float fogWorldX = depth * rayScaleX * (2.0f * (fogX - left) / width - 1.0f);
        ExpectNear(fogWorldX, viewX, "fog world X");
    }
}

void CheckDegenerateGuards() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const float bad : { 0.0f, -0.0f, 1e-6f, -1e-6f, nan, inf, -inf }) {
        ExpectNear(Fast::MapFogRayScaleX(0.8f, bad), 0.8f, "MapFogRayScaleX degenerate guard");
        float proj[4][4] = {
            { 1.25f, 0, 0, 0 },
            { 0, 1.75f, 0, 0 },
            { 0.125f, 0, -1.0f, -1.0f },
            { 0, 0, -20.0f, 0 },
        };
        Fast::ScaleProjectionAspectX(proj, bad);
        ExpectNear(proj[0][0], 1.25f, "ScaleProjectionAspectX degenerate guard [0][0]");
        ExpectNear(proj[2][0], 0.125f, "ScaleProjectionAspectX degenerate guard [2][0]");
    }
}

} // namespace

int main() {
    CheckProjection(640, 480, 0, 0, 640, 480);      // 4:3
    CheckProjection(1920, 1080, 0, 0, 1920, 1080);  // 16:9
    CheckProjection(1920, 1009, 0, 0, 1920, 1009);  // 1920x1009 windowed (R ~ 1.4272)
    CheckProjection(1920, 1009, 18, 42, 1980, 1080);
    CheckDegenerateGuards();
}
