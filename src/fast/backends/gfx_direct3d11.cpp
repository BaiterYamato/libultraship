#ifdef ENABLE_DX11

#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>

#include <map>
#include <unordered_map>

#include <windows.h>
#include <versionhelpers.h>
#include <wrl/client.h>

#include <dxgi1_3.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif

#include "fast/backends/gfx_window_manager_api.h"
#include "fast/backends/gfx_direct3d_common.h"

#define DECLARE_GFX_DXGI_FUNCTIONS
#include "fast/backends/gfx_dxgi.h"

#include "fast/backends/gfx_screen_config.h"
#include "ship/window/gui/Gui.h"
#include "fast/Fast3dGui.h"
#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/window/Window.h"

#include "fast/backends/gfx_rendering_api.h"
#include "fast/interpreter.h"

#include <prism/processor.h>
#include "ship/config/ConsoleVariable.h"
#include <ship/resource/factory/ShaderFactory.h>
#include <ship/resource/ResourceManager.h>
#include "spdlog/spdlog.h"
#include "nlohmann/json.hpp"

#include "fast/Fast3dWindow.h"

#define DEBUG_D3D 0

using namespace Microsoft::WRL; // For ComPtr

namespace Fast {
namespace {
constexpr int32_t kAoFallbackNone = 0;
constexpr int32_t kAoFallbackBackendUnsupportedMetal = 1;
constexpr int32_t kAoFallbackShaderCompileFailed = 2;
constexpr int32_t kAoFallbackRuntimeUnsupported = 3;
constexpr int32_t kVolumetricsFallbackNone = 0;
constexpr int32_t kVolumetricsFallbackBackendUnsupportedMetal = 1;
constexpr int32_t kVolumetricsFallbackShaderCompileFailed = 2;
constexpr int32_t kVolumetricsFallbackRuntimeUnsupported = 3;

struct ShadowFloat3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ShadowFloat4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

static ShadowFloat3 ShadowFloat3Add(const ShadowFloat3& lhs, const ShadowFloat3& rhs) {
    return { lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z };
}

static ShadowFloat3 ShadowFloat3Sub(const ShadowFloat3& lhs, const ShadowFloat3& rhs) {
    return { lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z };
}

static ShadowFloat3 ShadowFloat3Scale(const ShadowFloat3& value, float scalar) {
    return { value.x * scalar, value.y * scalar, value.z * scalar };
}

static float ShadowFloat3Dot(const ShadowFloat3& lhs, const ShadowFloat3& rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

static ShadowFloat3 ShadowFloat3Cross(const ShadowFloat3& lhs, const ShadowFloat3& rhs) {
    return { lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z, lhs.x * rhs.y - lhs.y * rhs.x };
}

static float ShadowFloat3Length(const ShadowFloat3& value) {
    return std::sqrt(ShadowFloat3Dot(value, value));
}

static ShadowFloat3 ShadowFloat3Normalize(const ShadowFloat3& value) {
    const float length = ShadowFloat3Length(value);
    if (length <= 0.0001f) {
        return { 0.0f, 0.0f, -1.0f };
    }
    return ShadowFloat3Scale(value, 1.0f / length);
}

static ShadowFloat4 MultiplyColumnMajorMatrixVec4(const float* matrix, const ShadowFloat4& vector) {
    ShadowFloat4 result{};
    result.x = matrix[0] * vector.x + matrix[4] * vector.y + matrix[8] * vector.z + matrix[12] * vector.w;
    result.y = matrix[1] * vector.x + matrix[5] * vector.y + matrix[9] * vector.z + matrix[13] * vector.w;
    result.z = matrix[2] * vector.x + matrix[6] * vector.y + matrix[10] * vector.z + matrix[14] * vector.w;
    result.w = matrix[3] * vector.x + matrix[7] * vector.y + matrix[11] * vector.z + matrix[15] * vector.w;
    return result;
}

static void MultiplyColumnMajorMatrices(const float* lhs, const float* rhs, float* outMatrix) {
    float result[16] = {};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            result[column * 4 + row] = lhs[0 * 4 + row] * rhs[column * 4 + 0] +
                                       lhs[1 * 4 + row] * rhs[column * 4 + 1] +
                                       lhs[2 * 4 + row] * rhs[column * 4 + 2] +
                                       lhs[3 * 4 + row] * rhs[column * 4 + 3];
        }
    }
    memcpy(outMatrix, result, sizeof(result));
}

static void BuildLookAtColumnMajor(const ShadowFloat3& eye, const ShadowFloat3& target, const ShadowFloat3& up, float* outMatrix) {
    const ShadowFloat3 forward = ShadowFloat3Normalize(ShadowFloat3Sub(target, eye));
    ShadowFloat3 right = ShadowFloat3Cross(up, forward);
    if (ShadowFloat3Length(right) <= 0.0001f) {
        right = ShadowFloat3Cross({ 0.0f, 0.0f, 1.0f }, forward);
    }
    right = ShadowFloat3Normalize(right);
    const ShadowFloat3 correctedUp = ShadowFloat3Cross(forward, right);
    const ShadowFloat3 normalizedUp = ShadowFloat3Normalize(correctedUp);

    outMatrix[0] = right.x;
    outMatrix[1] = right.y;
    outMatrix[2] = right.z;
    outMatrix[3] = 0.0f;

    outMatrix[4] = normalizedUp.x;
    outMatrix[5] = normalizedUp.y;
    outMatrix[6] = normalizedUp.z;
    outMatrix[7] = 0.0f;

    outMatrix[8] = forward.x;
    outMatrix[9] = forward.y;
    outMatrix[10] = forward.z;
    outMatrix[11] = 0.0f;

    outMatrix[12] = -ShadowFloat3Dot(right, eye);
    outMatrix[13] = -ShadowFloat3Dot(normalizedUp, eye);
    outMatrix[14] = -ShadowFloat3Dot(forward, eye);
    outMatrix[15] = 1.0f;
}

static void BuildOrthoOffCenterColumnMajor(float left, float right, float bottom, float top, float nearPlane,
                                           float farPlane, float* outMatrix) {
    memset(outMatrix, 0, sizeof(float) * 16);
    const float invWidth = 1.0f / std::max(right - left, 0.0001f);
    const float invHeight = 1.0f / std::max(top - bottom, 0.0001f);
    const float invDepth = 1.0f / std::max(farPlane - nearPlane, 0.0001f);

    outMatrix[0] = 2.0f * invWidth;
    outMatrix[5] = 2.0f * invHeight;
    outMatrix[10] = 2.0f * invDepth;
    outMatrix[12] = -(right + left) * invWidth;
    outMatrix[13] = -(top + bottom) * invHeight;
    outMatrix[14] = -(farPlane + nearPlane) * invDepth;
    outMatrix[15] = 1.0f;
}

static bool BuildDirectionalShadowViewProjection(const GfxVolumetricsConfig& config, const GfxVolumetricLight& light,
                                                 float* outMatrix) {
    const ShadowFloat3 lightDirection = ShadowFloat3Normalize(
        { light.direction[0], light.direction[1], light.direction[2] });
    if (ShadowFloat3Length(lightDirection) <= 0.0001f) {
        return false;
    }

    const ShadowFloat4 clipCorners[8] = {
        { -1.0f, -1.0f, -1.0f, 1.0f }, { 1.0f, -1.0f, -1.0f, 1.0f }, { -1.0f, 1.0f, -1.0f, 1.0f },
        { 1.0f, 1.0f, -1.0f, 1.0f },   { -1.0f, -1.0f, 1.0f, 1.0f }, { 1.0f, -1.0f, 1.0f, 1.0f },
        { -1.0f, 1.0f, 1.0f, 1.0f },   { 1.0f, 1.0f, 1.0f, 1.0f },
    };

    ShadowFloat3 worldCorners[8] = {};
    ShadowFloat3 center{};
    for (size_t i = 0; i < 8; ++i) {
        ShadowFloat4 world = MultiplyColumnMajorMatrixVec4(config.inverseViewProjection, clipCorners[i]);
        const float invW = 1.0f / std::max(world.w, 0.0001f);
        worldCorners[i] = { world.x * invW, world.y * invW, world.z * invW };
        center = ShadowFloat3Add(center, worldCorners[i]);
    }
    center = ShadowFloat3Scale(center, 1.0f / 8.0f);

    float maxRadius = 0.0f;
    for (const auto& corner : worldCorners) {
        maxRadius = std::max(maxRadius, ShadowFloat3Length(ShadowFloat3Sub(corner, center)));
    }

    ShadowFloat3 up = std::abs(ShadowFloat3Dot(lightDirection, { 0.0f, 1.0f, 0.0f })) > 0.95f ? ShadowFloat3{ 0.0f, 0.0f, 1.0f }
                                                                                                  : ShadowFloat3{ 0.0f, 1.0f, 0.0f };
    const ShadowFloat3 eye = ShadowFloat3Sub(center, ShadowFloat3Scale(lightDirection, maxRadius + 1024.0f));

    float viewMatrix[16];
    BuildLookAtColumnMajor(eye, center, up, viewMatrix);

    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float minZ = std::numeric_limits<float>::max();
    float maxX = -std::numeric_limits<float>::max();
    float maxY = -std::numeric_limits<float>::max();
    float maxZ = -std::numeric_limits<float>::max();
    for (const auto& corner : worldCorners) {
        const ShadowFloat4 lightSpace = MultiplyColumnMajorMatrixVec4(viewMatrix, { corner.x, corner.y, corner.z, 1.0f });
        minX = std::min(minX, lightSpace.x);
        minY = std::min(minY, lightSpace.y);
        minZ = std::min(minZ, lightSpace.z);
        maxX = std::max(maxX, lightSpace.x);
        maxY = std::max(maxY, lightSpace.y);
        maxZ = std::max(maxZ, lightSpace.z);
    }

    const float paddingXY = std::max(maxRadius * 0.2f, 128.0f);
    const float paddingZ = std::max(maxRadius * 0.5f, 256.0f);
    float orthoMatrix[16];
    BuildOrthoOffCenterColumnMajor(minX - paddingXY, maxX + paddingXY, minY - paddingXY, maxY + paddingXY,
                                   minZ - paddingZ, maxZ + paddingZ, orthoMatrix);
    MultiplyColumnMajorMatrices(orthoMatrix, viewMatrix, outMatrix);
    return true;
}

const char* kAoFullscreenVs = R"(
struct VsOut {
    float4 pos : SV_POSITION;
};
VsOut VSMain(uint vertexId : SV_VertexID) {
    VsOut outv;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    outv.pos = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return outv;
}
)";

const char* kAoGeneratePs = R"(
cbuffer AoParams : register(b0) {
    float2 invResolution;
    float radiusPixels;
    float intensity;
    float bias;
    float power;
    float maxDistance;
    uint sampleCount;
    uint debugView;
    float2 blurDirection;
}
Texture2D depthTex : register(t0);
SamplerState depthSampler : register(s0);

float ReadDepth(float2 uv) {
    return depthTex.SampleLevel(depthSampler, saturate(uv), 0).r;
}

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    float2 uv = position.xy * invResolution;
    float centerDepth = ReadDepth(uv);
    if (centerDepth >= 0.9999f) {
        return float4(1.0f, 0.0f, 0.0f, 1.0f);
    }
    float2 kernel[16] = {
        float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0),
        float2(0.707, 0.707), float2(-0.707, 0.707), float2(0.707, -0.707), float2(-0.707, -0.707),
        float2(2.0, 0.0), float2(-2.0, 0.0), float2(0.0, 2.0), float2(0.0, -2.0),
        float2(1.5, 0.5), float2(-1.5, 0.5), float2(1.5, -0.5), float2(-1.5, -0.5)
    };
    float occlusion = 0.0f;
    float maxDistanceDepth = maxDistance * 0.0001f;
    uint samples = clamp(sampleCount, 4u, 16u);
    [loop]
    for (uint i = 0; i < samples; i++) {
        float2 offset = kernel[i] * radiusPixels * invResolution;
        float sampleDepth = ReadDepth(uv + offset);
        float delta = sampleDepth - centerDepth;
        if (delta > bias && delta < maxDistanceDepth) {
            occlusion += 1.0f;
        }
    }
    float ao = 1.0f - (occlusion / samples) * intensity;
    ao = pow(saturate(ao), max(power, 0.0001f));
    return float4(ao, 0.0f, 0.0f, 1.0f);
}
)";

const char* kAoGeneratePsMsaa = R"(
cbuffer AoParams : register(b0) {
    float2 invResolution;
    float radiusPixels;
    float intensity;
    float bias;
    float power;
    float maxDistance;
    uint sampleCount;
    uint debugView;
    float2 blurDirection;
}
Texture2DMS<float, 2> depthTexMsaa : register(t0);

float ReadDepthMsaa(int2 pixelCoord) {
    return depthTexMsaa.Load(pixelCoord, 0);
}

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    int2 coord = int2(position.xy);
    float centerDepth = ReadDepthMsaa(coord);
    if (centerDepth >= 0.9999f) {
        return float4(1.0f, 0.0f, 0.0f, 1.0f);
    }
    float2 kernel[16] = {
        float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0),
        float2(1.0, 1.0), float2(-1.0, 1.0), float2(1.0, -1.0), float2(-1.0, -1.0),
        float2(2.0, 0.0), float2(-2.0, 0.0), float2(0.0, 2.0), float2(0.0, -2.0),
        float2(2.0, 1.0), float2(-2.0, 1.0), float2(2.0, -1.0), float2(-2.0, -1.0)
    };
    float occlusion = 0.0f;
    float maxDistanceDepth = maxDistance * 0.0001f;
    uint samples = clamp(sampleCount, 4u, 16u);
    [loop]
    for (uint i = 0; i < samples; i++) {
        int2 sampleCoord = coord + int2(kernel[i] * radiusPixels);
        float sampleDepth = ReadDepthMsaa(sampleCoord);
        float delta = sampleDepth - centerDepth;
        if (delta > bias && delta < maxDistanceDepth) {
            occlusion += 1.0f;
        }
    }
    float ao = 1.0f - (occlusion / samples) * intensity;
    ao = pow(saturate(ao), max(power, 0.0001f));
    return float4(ao, 0.0f, 0.0f, 1.0f);
}
)";

const char* kAoBlurPs = R"(
cbuffer AoParams : register(b0) {
    float2 invResolution;
    float radiusPixels;
    float intensity;
    float bias;
    float power;
    float maxDistance;
    uint sampleCount;
    uint debugView;
    float2 blurDirection;
}
Texture2D aoTex : register(t0);
SamplerState aoSampler : register(s0);

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    float2 uv = position.xy * invResolution;
    float w0 = 0.227027f;
    float w1 = 0.316216f;
    float w2 = 0.070270f;
    float result = aoTex.SampleLevel(aoSampler, saturate(uv), 0).r * w0;
    result += aoTex.SampleLevel(aoSampler, saturate(uv + blurDirection * 1.384615f * invResolution), 0).r * w1;
    result += aoTex.SampleLevel(aoSampler, saturate(uv - blurDirection * 1.384615f * invResolution), 0).r * w1;
    result += aoTex.SampleLevel(aoSampler, saturate(uv + blurDirection * 3.230769f * invResolution), 0).r * w2;
    result += aoTex.SampleLevel(aoSampler, saturate(uv - blurDirection * 3.230769f * invResolution), 0).r * w2;
    return float4(saturate(result), 0.0f, 0.0f, 1.0f);
}
)";

const char* kAoCompositePs = R"(
cbuffer AoParams : register(b0) {
    float2 invResolution;
    float radiusPixels;
    float intensity;
    float bias;
    float power;
    float maxDistance;
    uint sampleCount;
    uint debugView;
    float2 blurDirection;
}
Texture2D aoTex : register(t0);
SamplerState aoSampler : register(s0);

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    float2 uv = position.xy * invResolution;
    float ao = aoTex.SampleLevel(aoSampler, saturate(uv), 0).r;
    if (debugView == 1u) {
        return float4(ao, ao, ao, 1.0f);
    }
    return float4(ao, ao, ao, 1.0f);
}
)";

const char* kVolumetricsRaymarchPs =
R"(
#define MAX_LIGHTS 8
#define DEBUG_OFF 0u
#define DEBUG_FOG_DENSITY 1u
#define DEBUG_LIGHT_ENERGY 2u
#define DEBUG_FINAL_VOLUME 3u
#define DEBUG_TRANSMITTANCE 4u
#define DEBUG_SHADOW_OCCLUSION 5u
#define DEBUG_TRANSMITTANCE 4u
#define DEBUG_SHADOW_OCCLUSION 5u
cbuffer VolumetricsParams : register(b0) {
    float2 invFullResolution;
    float2 invLowResolution;
    float3 cameraWorldPosition;
    float density;
    float3 fogColor;
    float ambientIntensity;
    float anisotropy;
    float startDistance;
    float maxDistance;
    float baseHeight;
    float heightFalloff;
    float lightShaftIntensity;
    float shadowIntensity;
    float temporalBlend;
    float jitterScale;
    float nearPlane;
    float farPlane;
    float paddingAfterFarPlane;
    float2 depthNdcParams;
    float2 ndcDepthParams;
    float debugDensityScale;
    float debugLightScale;
    float debugFinalScale;
    float debugPreviewBlend;
    float debugDensityBias;
    float debugLightBias;
    float debugFinalBias;
    float debugMediumBias;
    float aerialPerspectiveStrength;
    float macroNoiseStrength;
    float valleyFogStrength;
    float edgeHazeStrength;
    float depthExtinctionStrength;
    float depthExtinctionExponent;
    float horizonFogStrength;
    float skyFallbackStrength;
    float directionalShadowBias;
    float directionalShadowNormalBias;
    float directionalShadowSoftness;
    float padding3;
    uint stepCount;
    uint lightCount;
    uint shadowQuality;
    uint historyValid;
    uint heightFogEnabled;
    uint debugMode;
    uint resetHistory;
    uint performanceMode;
    uint raymarchEnabled;
    uint depthFogPathEnabled;
    uint shadowMode;
    uint padding1;
    column_major float4x4 currViewProj;
    column_major float4x4 invViewProj;
    column_major float4x4 prevViewProj;
    uint directionalShadowEnabled;
    uint directionalShadowLightIndex;
    uint directionalShadowMapSize;
    uint padding2;
    column_major float4x4 directionalShadowViewProj;
    float4 lightPositionRadius[MAX_LIGHTS];
    float4 lightDirectionIntensity[MAX_LIGHTS];
    float4 lightColorVolumetric[MAX_LIGHTS];
    float4 lightConeShadow[MAX_LIGHTS];
    float4 lightShadowParams[MAX_LIGHTS];
};
Texture2D depthTex : register(t0);
Texture2D prevVolumeTex : register(t1);
Texture2D<uint> directionalShadowTex : register(t2);
SamplerState linearSampler : register(s0);

struct PsOut {
    float4 color : SV_Target0;
    float lowDepth : SV_Target1;
    float4 metrics : SV_Target2;
};

float ReadDepth(float2 uv) {
    return depthTex.SampleLevel(linearSampler, saturate(uv), 0).r;
}

float Noise(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float Noise3(float3 p) {
    return frac(sin(dot(p, float3(12.9898, 78.233, 37.719))) * 43758.5453);
}

float Fbm3(float3 p) {
    float value = 0.0;
    float amplitude = 0.5;
    float3 samplePos = p;
    [loop]
    for (int octave = 0; octave < 4; ++octave) {
        if (octave >= (performanceMode >= 2 ? 3 : (performanceMode == 1 ? 2 : 1))) {
            break;
        }
        value += Noise3(samplePos) * amplitude;
        samplePos = samplePos * 2.03 + float3(17.0, 31.0, 47.0);
        amplitude *= 0.5;
    }
    return saturate(value / (performanceMode >= 2 ? 0.875 : (performanceMode == 1 ? 0.75 : 0.5)));
}

float3 ReconstructWorldPosition(float2 uv, float depth) {
    float ndcZ = depth * depthNdcParams.x + depthNdcParams.y;
    float4 clip = float4(uv * 2.0 - 1.0, ndcZ, 1.0);
    float4 world = mul(invViewProj, clip);
    return world.xyz / max(world.w, 0.0001);
}

float3 ReconstructViewRayDirection(float2 uv) {
    float4 clip = float4(uv * 2.0 - 1.0, 1.0, 1.0);
    float4 world = mul(invViewProj, clip);
    float3 worldPos = world.xyz / max(world.w, 0.0001);
    return normalize(worldPos - cameraWorldPosition);
}

float EstimateSkyProxyDistance(float2 uv, float fallbackDistance, out float confidence) {
    if (performanceMode < 2) {
        confidence = 0.0;
        return fallbackDistance;
    }

    float accumulatedDistance = 0.0;
    float accumulatedWeight = 0.0;
    float closestDistance = fallbackDistance;
    float validSampleCount = 0.0;

    [unroll]
    for (int ring = 1; ring <= 4; ++ring) {
        float offsetY = invFullResolution.y * (10.0 * ring);
        float offsetX = invFullResolution.x * (12.0 * ring);

        [unroll]
        for (int sampleX = -2; sampleX <= 2; ++sampleX) {
            [unroll]
            for (int sampleDirection = 0; sampleDirection < 3; ++sampleDirection) {
                float2 sampleOffset = float2(offsetX * sampleX * 0.55, offsetY);
                if (sampleDirection == 1) {
                    sampleOffset = float2(offsetX * sampleX * 0.9, offsetY * 0.55);
                } else if (sampleDirection == 2) {
                    sampleOffset = float2(offsetX * sampleX * 0.35, offsetY * 1.35);
                }

                float2 sampleUv = saturate(uv + sampleOffset);
                float sampleDepth = ReadDepth(sampleUv);
                if (sampleDepth >= 0.9999) {
                    continue;
                }

                float3 sampleWorldPos = ReconstructWorldPosition(sampleUv, sampleDepth);
                float sampleDistance = length(sampleWorldPos - cameraWorldPosition);
                float weight = 1.0 / (1.0 + abs((float)sampleX) + (ring - 1) * 0.75 + sampleDirection * 0.35);
                accumulatedDistance += sampleDistance * weight;
                accumulatedWeight += weight;
                closestDistance = min(closestDistance, sampleDistance);
                validSampleCount += 1.0;
            }
        }
    }

    if (accumulatedWeight <= 0.0001) {
        confidence = 0.0;
        return fallbackDistance;
    }

    float averagedDistance = accumulatedDistance / accumulatedWeight;
    float blendedDistance = lerp(closestDistance, averagedDistance, 0.65);
    float reliability = saturate(validSampleCount / 10.0);
    float conservativeDistance = lerp(closestDistance, blendedDistance, reliability * 0.55);
    confidence = reliability;
    return lerp(fallbackDistance, conservativeDistance, reliability);
}

bool ProjectWorld(column_major float4x4 viewProj, float3 worldPos, out float2 uv, out float deviceDepth) {
    float4 clip = mul(viewProj, float4(worldPos, 1.0));
    if (clip.w <= 0.0001) {
        uv = float2(-1.0, -1.0);
        deviceDepth = 1.0;
        return false;
    }
    float3 ndc = clip.xyz / clip.w;
    uv = ndc.xy * 0.5 + 0.5;
    deviceDepth = ndc.z * ndcDepthParams.x + ndcDepthParams.y;
    return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
}

float PhaseHG(float cosTheta, float g) {
    float g2 = g * g;
    float denom = pow(max(1.0 + g2 - 2.0 * g * cosTheta, 0.001), 1.5);
    return (1.0 - g2) / max(12.5663706 * denom, 0.001);
}

float ComputeShadow(int lightIndex, float3 sampleWorldPos) {
    if (shadowQuality == 0 || lightShadowParams[lightIndex].w < 0.5) {
        return 1.0;
    }

    float3 traceDirection = float3(0.0, 0.0, 0.0);
    float maxTraceDistance = max(lightConeShadow[lightIndex].w, 0.0);
    int lightType = (int)(lightShadowParams[lightIndex].z + 0.5);
    if (lightType == 0) {
        if (directionalShadowEnabled != 0 && directionalShadowMapSize > 0 &&
            directionalShadowLightIndex == (uint)lightIndex) {
            float4 lightClip = mul(directionalShadowViewProj, float4(sampleWorldPos, 1.0));
            float invLightW = 1.0 / max(abs(lightClip.w), 0.0001);
            float3 lightNdc = lightClip.xyz * invLightW;
            float2 shadowUv = lightNdc.xy * 0.5 + 0.5;
            if (shadowUv.x < 0.0 || shadowUv.x > 1.0 || shadowUv.y < 0.0 || shadowUv.y > 1.0) {
                return 1.0;
            }

            float lightDepth = lightNdc.z * ndcDepthParams.x + ndcDepthParams.y;
            int shadowSize = max((int)directionalShadowMapSize, 1);
            int2 baseCoord = int2(clamp(shadowUv * shadowSize, float2(0.0, 0.0), float2(shadowSize - 1, shadowSize - 1)));
            int pcfRadius = (int)clamp(round(directionalShadowSoftness * (shadowQuality >= 3 ? 1.0 : 0.5)), 0.0, 2.0);
            float directionalNormalFactor = 1.0 - abs(normalize(lightDirectionIntensity[lightIndex].xyz).y);
            float bias = max(0.0, lightConeShadow[lightIndex].z + directionalShadowBias +
                                      directionalShadowNormalBias * directionalNormalFactor +
                                      lightShadowParams[lightIndex].x * 0.01);
            float visibility = 0.0;
            int sampleCount = 0;
            [loop]
            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                [loop]
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    if (abs(offsetX) > pcfRadius || abs(offsetY) > pcfRadius) {
                        continue;
                    }
                    int2 sampleCoord = clamp(baseCoord + int2(offsetX, offsetY), int2(0, 0), int2(shadowSize - 1, shadowSize - 1));
                    uint packedDepth = directionalShadowTex.Load(int3(sampleCoord, 0));
                    if (packedDepth == 0xFFFFFFFFu) {
                        visibility += 1.0;
                    } else {
                        float shadowDepth = (float)packedDepth / 4294967295.0;
                        visibility += lightDepth <= shadowDepth + bias ? 1.0 : (1.0 - shadowIntensity);
                    }
                    sampleCount++;
                }
            }
            return visibility / max((float)sampleCount, 1.0);
        }

        float3 directionalVector = -lightDirectionIntensity[lightIndex].xyz;
        float directionalLength = length(directionalVector);
        if (directionalLength <= 0.001) {
            return 1.0;
        }
        traceDirection = directionalVector / directionalLength;
        maxTraceDistance = min(max(maxTraceDistance, startDistance + 64.0), maxDistance);
    } else {
        float3 toLight = lightPositionRadius[lightIndex].xyz - sampleWorldPos;
        float lengthToLight = length(toLight);
        if (lengthToLight <= 0.001) {
            return 1.0;
        }
        traceDirection = toLight / lengthToLight;
        maxTraceDistance = min(maxTraceDistance, lengthToLight);
    }

    if (maxTraceDistance <= 0.001) {
        return 1.0;
    }

    float occlusion = 0.0;
    int shadowSamples = shadowQuality == 1 ? 2 : (shadowQuality == 2 ? 3 : 4);
    float shadowBias = lightConeShadow[lightIndex].z + lightShadowParams[lightIndex].x * 0.01;
    [loop]
    for (int sampleIndex = 1; sampleIndex <= 4; ++sampleIndex) {
        if (sampleIndex > shadowSamples) {
            break;
        }
        float sampleT = (float)sampleIndex / max((float)shadowSamples, 1.0);
        float travel = maxTraceDistance * sampleT;
        float3 probeWorldPos = sampleWorldPos + traceDirection * travel;
        float2 probeUv;
        float probeDepth;
        if (!ProjectWorld(currViewProj, probeWorldPos, probeUv, probeDepth)) {
            continue;
        }
        float sceneDepth = ReadDepth(probeUv);
        if (sceneDepth + shadowBias < probeDepth) {
            float weight = (lightType == 0) ? (1.0 - sampleT * 0.35) : 1.0;
            occlusion += max(weight, 0.1);
        }
    }

    return saturate(1.0 - (occlusion / max((float)shadowSamples, 1.0)) * shadowIntensity);
}

float ComputeLightAttenuation(int lightIndex, float3 sampleWorldPos, out float3 lightDir) {
    int lightType = (int)(lightShadowParams[lightIndex].z + 0.5);
    lightDir = normalize(-lightDirectionIntensity[lightIndex].xyz);
    if (lightType == 0) {
        return 1.0;
    }

    float3 toLight = lightPositionRadius[lightIndex].xyz - sampleWorldPos;
    float distanceToLight = length(toLight);
    if (distanceToLight <= 0.001) {
        return 0.0;
    }

    lightDir = toLight / distanceToLight;
    float radius = max(lightPositionRadius[lightIndex].w, 0.001);
    float attenuation = saturate(1.0 - distanceToLight / radius);
    attenuation *= attenuation;

    if (lightType == 2) {
        float coneDot = dot(normalize(sampleWorldPos - lightPositionRadius[lightIndex].xyz),
                            normalize(lightDirectionIntensity[lightIndex].xyz));
        float innerCos = lightConeShadow[lightIndex].x;
        float outerCos = lightConeShadow[lightIndex].y;
        float coneAttenuation = smoothstep(outerCos, innerCos, coneDot);
        attenuation *= coneAttenuation;
    }

    return attenuation;
}

)"
R"(
PsOut PSMain(float4 position : SV_POSITION) {
    PsOut output;
    float2 uv = position.xy * invLowResolution;
    float fullDepth = ReadDepth(uv);
    float3 baseRayDirection = ReconstructViewRayDirection(uv);
    bool hasSurfaceDepth = fullDepth < 0.9999;
    float3 surfaceWorldPosFromDepth = cameraWorldPosition;
    float surfaceDistance = maxDistance;
    float skyProxyConfidence = 1.0;
    if (hasSurfaceDepth) {
        surfaceWorldPosFromDepth = ReconstructWorldPosition(uv, fullDepth);
        surfaceDistance = min(length(surfaceWorldPosFromDepth - cameraWorldPosition), maxDistance);
        bool skyLikeDepth = fullDepth > 0.995 &&
                            surfaceDistance > max(startDistance + 96.0, maxDistance * 0.22) &&
                            baseRayDirection.y > -0.18;
        if (skyLikeDepth) {
            hasSurfaceDepth = false;
            surfaceDistance = maxDistance;
        }
    }
    output.lowDepth = hasSurfaceDepth ? fullDepth : 1.0;
    output.metrics = float4(0.0, 0.0, 0.0, 0.0);

    float3 rayDirection = hasSurfaceDepth ? normalize(surfaceWorldPosFromDepth - cameraWorldPosition)
                                         : baseRayDirection;
    float horizonFactor = saturate(1.0 - abs(rayDirection.y));
    if (!hasSurfaceDepth) {
        float skyProxyDistance = lerp(max(startDistance + 24.0, maxDistance * 0.0125),
                                      max(startDistance + 120.0, maxDistance * 0.12), saturate(pow(horizonFactor, 2.2)));
        float sampledSkyDistance = EstimateSkyProxyDistance(uv, skyProxyDistance, skyProxyConfidence);
        float noDepthMaxDistance = lerp(max(startDistance + 72.0, maxDistance * 0.06),
                                        max(startDistance + 144.0, maxDistance * 0.12),
                                        saturate(pow(horizonFactor, 2.1)));
        surfaceDistance = min(max(sampledSkyDistance, startDistance + 48.0), noDepthMaxDistance);
    }
    if (surfaceDistance <= startDistance + 0.001) {
        output.color = float4(0.0, 0.0, 0.0, 1.0);
        return output;
    }

    float distanceSpan = max(maxDistance - startDistance, 1.0);
    float totalDistanceNorm = saturate((surfaceDistance - startDistance) / distanceSpan);
    float surfaceDepthFactor = hasSurfaceDepth ? saturate(pow(totalDistanceNorm, 1.08)) : 0.0;
    float skyDistanceFactor = saturate(surfaceDistance / max(maxDistance, 1.0));
    float noDepthConfidenceMask = hasSurfaceDepth ? 1.0 : smoothstep(0.35, 0.88, skyProxyConfidence);
    float noDepthHorizonMask = hasSurfaceDepth ? 1.0 : smoothstep(0.982, 0.9996, horizonFactor);
    float noDepthDistanceMask = hasSurfaceDepth ? 1.0 : smoothstep(0.32, 0.95, skyDistanceFactor);
    float noDepthCoverage = hasSurfaceDepth
                                ? 1.0
                                : noDepthConfidenceMask * noDepthHorizonMask * noDepthDistanceMask;
    float skyRayFade = hasSurfaceDepth
                           ? 1.0
                           : noDepthCoverage * smoothstep(0.94, 1.0, horizonFactor);

    float3 surfaceWorldPos = cameraWorldPosition + rayDirection * surfaceDistance;
    int clampedStepCount = clamp((int)stepCount, 8, 64);
    float rayRange = max(surfaceDistance - startDistance, 0.001);
    float stepSize = rayRange / max((float)clampedStepCount, 1.0);
    float jitter = (Noise(position.xy) - 0.5) * jitterScale;
    float t = startDistance + stepSize * (0.5 + jitter);
    float3 volumeAccum = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;
    float densityIntegral = 0.0;
    float mediumLuma = 0.0;
    float lightingLuma = 0.0;
    float opticalDepth = 0.0;

    [loop]
    for (int stepIndex = 0; stepIndex < 64; ++stepIndex) {
        if (stepIndex >= clampedStepCount || t >= surfaceDistance || t >= maxDistance) {
            break;
        }

        float3 sampleWorldPos = cameraWorldPosition + rayDirection * t;
        float sampleDensity = density;
        if (heightFogEnabled != 0) {
            sampleDensity *= exp(-max(sampleWorldPos.y - baseHeight, 0.0) * heightFalloff);
        }
        float rayProgress = saturate((t - startDistance) / rayRange);
        float absoluteDistanceNorm = saturate((t - startDistance) / distanceSpan);
        float densityNoise = Fbm3(sampleWorldPos * float3(0.0035, 0.0021, 0.0035));
        float detailNoise = performanceMode >= 1
                                ? Fbm3(sampleWorldPos * float3(0.0095, 0.0065, 0.0095) + float3(19.0, 37.0, 53.0))
                                : 0.5;
        float macroNoise = performanceMode >= 2
                               ? Fbm3(sampleWorldPos * float3(0.00115, 0.0007, 0.00115) + float3(71.0, 17.0, 43.0))
                               : densityNoise;
        float pocketNoise = performanceMode >= 2
                                ? Fbm3(sampleWorldPos * float3(0.0058, 0.0032, 0.0058) + float3(13.0, 97.0, 29.0))
                                : 0.5;
        float verticalLayer = 0.72 + 0.28 * (0.5 + 0.5 * sin(sampleWorldPos.y * 0.012 + densityNoise * 4.8));
        float billowLayer = 0.82 + 0.18 * (0.5 + 0.5 * sin(sampleWorldPos.x * 0.0042 + sampleWorldPos.z * 0.0026 + macroNoise * 6.0));
        float macroNoiseWeight = saturate(macroNoiseStrength);
        float macroNoiseBoost = 1.0 + max(macroNoiseStrength - 1.0, 0.0) * 0.35;
        float valleyFogWeight = saturate(valleyFogStrength);
        float valleyFogBoost = 1.0 + max(valleyFogStrength - 1.0, 0.0) * 0.25;
        float groundPocket = 1.0;
        if (heightFogEnabled != 0) {
            float groundMask = saturate(1.0 - max(sampleWorldPos.y - baseHeight, 0.0) / 180.0);
            groundPocket = lerp(0.78, 1.34, groundMask);
        }
        float macroCluster = lerp(0.82, 1.24, macroNoise) * lerp(0.9, 1.16, pocketNoise) * billowLayer;
        float densityModulation =
            lerp(0.72, 1.28, densityNoise) * lerp(0.86, 1.18, detailNoise) * verticalLayer *
            lerp(1.0, macroCluster, macroNoiseWeight) * macroNoiseBoost *
            lerp(1.0, groundPocket, valleyFogWeight) * valleyFogBoost;
        float distanceBoost = hasSurfaceDepth ? lerp(0.58, 4.35, pow(rayProgress, 1.12))
                                              : lerp(0.72, 1.7, pow(rayProgress, 1.55));
        float horizonDepthBoost = (hasSurfaceDepth ? lerp(0.86, 1.52, horizonFactor)
                                                   : lerp(0.72, 1.34, horizonFactor)) *
                                  lerp(1.0, 1.0 + max(horizonFogStrength - 1.0, 0.0), horizonFactor);
        sampleDensity *= densityModulation * distanceBoost * horizonDepthBoost;
        if (!hasSurfaceDepth) {
            float noDepthDensityScale =
                noDepthCoverage * smoothstep(0.975, 1.0, rayProgress) * smoothstep(0.78, 1.0, skyDistanceFactor) *
                smoothstep(0.9, 0.985, skyProxyConfidence) * skyFallbackStrength;
            sampleDensity *= noDepthDensityScale;
        } else {
            sampleDensity *= lerp(0.85, 1.85, surfaceDepthFactor);
        }
        sampleDensity = max(sampleDensity, 0.0);
        float mediumStep = sampleDensity * stepSize * 0.0135;
        if (mediumStep > 0.0) {
            densityIntegral += mediumStep;
            float3 stepLighting = float3(0.0, 0.0, 0.0);
            [loop]
            for (int lightIndex = 0; lightIndex < MAX_LIGHTS; ++lightIndex) {
                if (lightIndex >= (int)lightCount) {
                    break;
                }
                float3 lightDir;
                float attenuation = ComputeLightAttenuation(lightIndex, sampleWorldPos, lightDir);
                if (attenuation <= 0.0001) {
                    continue;
                }
                float visibility = ComputeShadow(lightIndex, sampleWorldPos);
                float phase = PhaseHG(dot(lightDir, -rayDirection), anisotropy);
                float3 lightColor = lightColorVolumetric[lightIndex].rgb;
                float lightIntensity = lightDirectionIntensity[lightIndex].w * lightColorVolumetric[lightIndex].a;
                stepLighting += lightColor * (lightIntensity * attenuation * visibility * phase);
            }

            float fogStepAlpha = saturate(1.0 - exp(-mediumStep * (hasSurfaceDepth ? 1.75 : 1.2)));
            float nearFade = hasSurfaceDepth ? smoothstep(0.0, 0.3, rayProgress) : smoothstep(0.08, 0.42, rayProgress);
            float farDepthWeight = hasSurfaceDepth ? lerp(0.9, 4.8, pow(absoluteDistanceNorm, 0.98))
                                                   : lerp(0.72, 1.42, absoluteDistanceNorm);
            float noDepthDistanceWindow = hasSurfaceDepth ? 1.0 : smoothstep(0.985, 1.0, rayProgress);
            float distanceVisibilityFade = nearFade * skyRayFade * farDepthWeight * noDepthDistanceWindow;
            float mediumDepthWeight = hasSurfaceDepth
                                          ? lerp(0.96, 5.8, surfaceDepthFactor) * lerp(0.95, 1.85, rayProgress)
                                          : lerp(0.72, 1.34, rayProgress) * lerp(0.85, 1.18, totalDistanceNorm);
            float skyAmbientFactor = hasSurfaceDepth
                                         ? 1.0
                                         : noDepthCoverage * smoothstep(0.985, 1.0, horizonFactor) *
                                               smoothstep(0.8, 1.0, skyDistanceFactor) *
                                               smoothstep(0.9, 0.985, skyProxyConfidence);
            float noLightSkySuppression = (!hasSurfaceDepth && lightCount == 0) ? 0.0 : 1.0;
            float3 mediumFog =
                fogColor * ambientIntensity * fogStepAlpha * mediumDepthWeight * distanceVisibilityFade *
                skyAmbientFactor * noLightSkySuppression;
            float scatteringBoost =
                (0.85 + densityNoise * 0.45) * lerp(0.78, 1.6, rayProgress) * lerp(0.74, 1.28, horizonFactor);
            float3 scattering =
                stepLighting *
                (mediumStep * lightShaftIntensity * scatteringBoost * distanceVisibilityFade * skyAmbientFactor *
                 noLightSkySuppression);
            mediumLuma += dot(mediumFog, float3(0.2126, 0.7152, 0.0722));
            lightingLuma += dot(scattering, float3(0.2126, 0.7152, 0.0722));
            volumeAccum += (mediumFog + scattering) * transmittance;
            float extinctionTerm = mediumStep * (hasSurfaceDepth ? lerp(2.4, 10.5, surfaceDepthFactor) *
                                                                      max(distanceVisibilityFade, 0.22)
                                                                  : lerp(1.0, 2.35, rayProgress) *
                                                                        max(noDepthCoverage, 0.05));
            opticalDepth += max(extinctionTerm, 0.0);
            transmittance *= exp(-max(extinctionTerm, 0.0));
            if (transmittance <= 0.01) {
                break;
            }
        }
        t += stepSize;
    }

    if (hasSurfaceDepth && totalDistanceNorm > 0.03) {
        float distanceFogAmount = saturate(1.0 - exp(-density * rayRange * 0.0085 *
                                                     lerp(0.55, 13.5, pow(totalDistanceNorm, 1.22))));
        float distanceExtinction = saturate(1.0 - exp(-density * rayRange * 0.0105 *
                                                      lerp(0.8, 16.0, pow(totalDistanceNorm, 1.28))));
        float horizonBoost = lerp(1.0, 1.9, horizonFactor);
        float heightMask = 1.0;
        if (heightFogEnabled != 0) {
            heightMask = exp(-max(surfaceWorldPos.y - baseHeight, 0.0) * heightFalloff * 0.28);
            heightMask = lerp(0.55, 1.0, heightMask);
        }
        float largeScaleNoise = Fbm3(surfaceWorldPos * float3(0.0016, 0.0, 0.0016) + float3(7.0, 19.0, 43.0));
        float mediumDetailNoise = performanceMode >= 1
                                      ? Fbm3(surfaceWorldPos * float3(0.0045, 0.0015, 0.0045) + float3(31.0, 11.0, 59.0))
                                      : 0.5;
        float depthVfxModulation = lerp(0.82, 1.18, largeScaleNoise) * lerp(0.9, 1.12, mediumDetailNoise);
        float3 aerialPerspectiveFog = fogColor * ambientIntensity * distanceFogAmount * horizonBoost *
                                      heightMask * depthVfxModulation * lerp(1.05, 2.1, surfaceDepthFactor);
        volumeAccum += aerialPerspectiveFog * max(transmittance, 0.08);
        mediumLuma += dot(aerialPerspectiveFog, float3(0.2126, 0.7152, 0.0722));
        densityIntegral += distanceFogAmount * 1.35;
        float distanceExtinctionTerm =
            distanceExtinction * max(depthExtinctionStrength, 0.0) *
            pow(max(totalDistanceNorm, 0.001), max(depthExtinctionExponent, 0.25)) * lerp(2.8, 6.8, surfaceDepthFactor);
        opticalDepth += max(distanceExtinctionTerm, 0.0);
        transmittance *= exp(-max(distanceExtinctionTerm, 0.0));
    }

    float3 finalVolume = max(volumeAccum, float3(0.0, 0.0, 0.0));
    if (debugMode == DEBUG_OFF && historyValid != 0 && temporalBlend > 0.0) {
        float2 prevUv;
        float prevDepth;
        if (ProjectWorld(prevViewProj, surfaceWorldPos, prevUv, prevDepth)) {
            float3 prevColor = prevVolumeTex.SampleLevel(linearSampler, saturate(prevUv), 0).rgb;
            finalVolume = lerp(finalVolume, prevColor, temporalBlend);
        }
    }

    float fogAmount = saturate(1.0 - transmittance);
    if (hasSurfaceDepth) {
        float depthCompositeFog = saturate(1.0 - exp(-density * rayRange * 0.0098 *
                                                     lerp(0.7, 14.0, pow(totalDistanceNorm, 1.18))));
        fogAmount = max(fogAmount, depthCompositeFog);
    }
    float finalLuma = dot(finalVolume, float3(0.2126, 0.7152, 0.0722));
    output.color = float4(max(finalVolume, float3(0.0, 0.0, 0.0)), fogAmount);
    output.metrics = float4(max(densityIntegral, 0.0), max(lightingLuma, 0.0), max(finalLuma, 0.0), max(opticalDepth, 0.0));
    return output;
}
)";

const char* kVolumetricsDirectionalShadowCs = R"(
cbuffer VolumetricsParams : register(b0) {
    float2 invFullResolution;
    float2 invLowResolution;
    float3 cameraWorldPosition;
    float density;
    float3 fogColor;
    float ambientIntensity;
    float anisotropy;
    float startDistance;
    float maxDistance;
    float baseHeight;
    float heightFalloff;
    float lightShaftIntensity;
    float shadowIntensity;
    float temporalBlend;
    float jitterScale;
    float nearPlane;
    float farPlane;
    float paddingAfterFarPlane;
    float2 depthNdcParams;
    float2 ndcDepthParams;
    float debugDensityScale;
    float debugLightScale;
    float debugFinalScale;
    float debugPreviewBlend;
    float debugDensityBias;
    float debugLightBias;
    float debugFinalBias;
    float debugMediumBias;
    float aerialPerspectiveStrength;
    float macroNoiseStrength;
    float valleyFogStrength;
    float edgeHazeStrength;
    float depthExtinctionStrength;
    float depthExtinctionExponent;
    float horizonFogStrength;
    float skyFallbackStrength;
    float directionalShadowBias;
    float directionalShadowNormalBias;
    float directionalShadowSoftness;
    float padding3;
    uint stepCount;
    uint lightCount;
    uint shadowQuality;
    uint historyValid;
    uint heightFogEnabled;
    uint debugMode;
    uint resetHistory;
    uint performanceMode;
    uint raymarchEnabled;
    uint depthFogPathEnabled;
    uint shadowMode;
    uint padding1;
    column_major float4x4 currViewProj;
    column_major float4x4 invViewProj;
    column_major float4x4 prevViewProj;
    uint directionalShadowEnabled;
    uint directionalShadowLightIndex;
    uint directionalShadowMapSize;
    uint padding2;
    column_major float4x4 directionalShadowViewProj;
    float4 lightPositionRadius[8];
    float4 lightDirectionIntensity[8];
    float4 lightColorVolumetric[8];
    float4 lightConeShadow[8];
    float4 lightShadowParams[8];
};

Texture2D depthTex : register(t0);
RWTexture2D<uint> directionalShadowTex : register(u0);

float3 ReconstructWorldPosition(float2 uv, float depth) {
    float ndcZ = depth * depthNdcParams.x + depthNdcParams.y;
    float4 clip = float4(uv * 2.0 - 1.0, ndcZ, 1.0);
    float4 world = mul(invViewProj, clip);
    return world.xyz / max(world.w, 0.0001);
}

float3 ReconstructViewRayDirection(float2 uv) {
    float4 clip = float4(uv * 2.0 - 1.0, 1.0, 1.0);
    float4 world = mul(invViewProj, clip);
    float3 worldPos = world.xyz / max(world.w, 0.0001);
    return normalize(worldPos - cameraWorldPosition);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (directionalShadowEnabled == 0 || directionalShadowMapSize == 0) {
        return;
    }

    uint fullWidth;
    uint fullHeight;
    depthTex.GetDimensions(fullWidth, fullHeight);
    if (dispatchThreadId.x >= fullWidth || dispatchThreadId.y >= fullHeight) {
        return;
    }

    float depth = depthTex.Load(int3(dispatchThreadId.xy, 0)).r;
    if (depth >= 0.9999) {
        return;
    }

    float2 uv = (float2(dispatchThreadId.xy) + 0.5) * invFullResolution;
    float3 worldPos = ReconstructWorldPosition(uv, depth);
    float4 lightClip = mul(directionalShadowViewProj, float4(worldPos, 1.0));
    float invLightW = 1.0 / max(abs(lightClip.w), 0.0001);
    float3 lightNdc = lightClip.xyz * invLightW;
    float2 shadowUv = lightNdc.xy * 0.5 + 0.5;
    if (shadowUv.x < 0.0 || shadowUv.x > 1.0 || shadowUv.y < 0.0 || shadowUv.y > 1.0) {
        return;
    }

    float lightDepth = saturate(lightNdc.z * ndcDepthParams.x + ndcDepthParams.y);
    uint packedDepth = (uint)(lightDepth * 4294967295.0);
    uint2 shadowCoord = min(uint2(shadowUv * directionalShadowMapSize), uint2(directionalShadowMapSize - 1, directionalShadowMapSize - 1));
    InterlockedMin(directionalShadowTex[shadowCoord], packedDepth);
}
)";

const char* kVolumetricsCompositePs = R"(
#define DEBUG_OFF 0u
#define DEBUG_FOG_DENSITY 1u
#define DEBUG_LIGHT_ENERGY 2u
#define DEBUG_FINAL_VOLUME 3u
cbuffer VolumetricsParams : register(b0) {
    float2 invFullResolution;
    float2 invLowResolution;
    float3 cameraWorldPosition;
    float density;
    float3 fogColor;
    float ambientIntensity;
    float anisotropy;
    float startDistance;
    float maxDistance;
    float baseHeight;
    float heightFalloff;
    float lightShaftIntensity;
    float shadowIntensity;
    float temporalBlend;
    float jitterScale;
    float nearPlane;
    float farPlane;
    float paddingAfterFarPlane;
    float2 depthNdcParams;
    float2 ndcDepthParams;
    float debugDensityScale;
    float debugLightScale;
    float debugFinalScale;
    float debugPreviewBlend;
    float debugDensityBias;
    float debugLightBias;
    float debugFinalBias;
    float debugMediumBias;
    float aerialPerspectiveStrength;
    float macroNoiseStrength;
    float valleyFogStrength;
    float edgeHazeStrength;
    float depthExtinctionStrength;
    float depthExtinctionExponent;
    float horizonFogStrength;
    float skyFallbackStrength;
    float directionalShadowBias;
    float directionalShadowNormalBias;
    float directionalShadowSoftness;
    float padding3;
    uint stepCount;
    uint lightCount;
    uint shadowQuality;
    uint historyValid;
    uint heightFogEnabled;
    uint debugMode;
    uint resetHistory;
    uint performanceMode;
    uint raymarchEnabled;
    uint depthFogPathEnabled;
    uint shadowMode;
    uint padding1;
    column_major float4x4 currViewProj;
    column_major float4x4 invViewProj;
    column_major float4x4 prevViewProj;
    uint directionalShadowEnabled;
    uint directionalShadowLightIndex;
    uint directionalShadowMapSize;
    uint padding2;
    column_major float4x4 directionalShadowViewProj;
    float4 lightPositionRadius[8];
    float4 lightDirectionIntensity[8];
    float4 lightColorVolumetric[8];
    float4 lightConeShadow[8];
    float4 lightShadowParams[8];
};
Texture2D volumeTex : register(t0);
Texture2D lowDepthTex : register(t1);
Texture2D fullDepthTex : register(t2);
Texture2D metricsTex : register(t3);
Texture2D<uint> directionalShadowTex : register(t4);
SamplerState linearSampler : register(s0);

float3 BuildFogDensityDebugColor(float densityNorm) {
    float3 coldColor = float3(0.01, 0.03, 0.08);
    float3 midColor = float3(0.0, 0.62, 1.0);
    float3 hotColor = float3(0.95, 0.99, 1.0);
    float3 ramp = lerp(coldColor, midColor, saturate(densityNorm * 0.8));
    return lerp(ramp, hotColor, saturate((densityNorm - 0.45) * 1.8));
}

float3 BuildLightEnergyDebugColor(float lightNorm) {
    float3 lowColor = float3(0.0, 0.0, 0.0);
    float3 midColor = float3(0.95, 0.45, 0.08);
    float3 hotColor = float3(1.0, 0.98, 0.25);
    float3 ramp = lerp(lowColor, midColor, saturate(lightNorm * 0.75));
    return lerp(ramp, hotColor, saturate(lightNorm));
}

float3 BuildFinalVolumeDebugColor(float3 volume) {
    float3 neutralBg = float3(0.015, 0.02, 0.03);
    float3 safeVolume = max(volume, float3(0.0, 0.0, 0.0));
    float volumeLuma = dot(safeVolume, float3(0.2126, 0.7152, 0.0722));
    float contrast = saturate((volumeLuma - debugFinalBias) * max(debugFinalScale, 0.001));
    float3 scaledVolume = safeVolume * (0.65 + contrast * 2.6) * max(debugFinalScale, 0.001);
    float3 tonemapped = 1.0 - exp(-scaledVolume);
    tonemapped = pow(saturate(tonemapped), 1.0 / 2.2);
    float mixFactor = saturate(max(contrast, dot(tonemapped, float3(0.2126, 0.7152, 0.0722)) * debugPreviewBlend));
    return lerp(neutralBg, tonemapped, mixFactor);
}

float3 BuildTransmittanceDebugColor(float transmittance) {
    float fogCoverage = saturate(1.0 - transmittance);
    float3 clearColor = float3(0.02, 0.035, 0.08);
    float3 fogColorDebug = float3(0.65, 0.92, 1.0);
    return lerp(clearColor, fogColorDebug, pow(fogCoverage, 0.72));
}

float3 BuildShadowOcclusionDebugColor(float visibility) {
    float occlusion = saturate(1.0 - visibility);
    float3 litColor = float3(0.9, 0.95, 1.0);
    float3 shadowColor = float3(0.05, 0.08, 0.12);
    return lerp(litColor, shadowColor, occlusion);
}

float ReadFullDepth(float2 uv) {
    return fullDepthTex.SampleLevel(linearSampler, saturate(uv), 0).r;
}

float3 ReconstructCompositeWorldPosition(float2 uv, float depth) {
    float ndcZ = depth * depthNdcParams.x + depthNdcParams.y;
    float4 clip = float4(uv * 2.0 - 1.0, ndcZ, 1.0);
    float4 world = mul(invViewProj, clip);
    return world.xyz / max(world.w, 0.0001);
}

float SampleDirectionalShadowVisibility(float3 worldPos) {
    if (directionalShadowEnabled == 0 || directionalShadowMapSize == 0) {
        return 1.0;
    }

    float4 lightClip = mul(directionalShadowViewProj, float4(worldPos, 1.0));
    float invLightW = 1.0 / max(abs(lightClip.w), 0.0001);
    float3 lightNdc = lightClip.xyz * invLightW;
    float2 shadowUv = lightNdc.xy * 0.5 + 0.5;
    if (shadowUv.x < 0.0 || shadowUv.x > 1.0 || shadowUv.y < 0.0 || shadowUv.y > 1.0) {
        return 1.0;
    }

    float lightDepth = lightNdc.z * ndcDepthParams.x + ndcDepthParams.y;
    int shadowSize = max((int)directionalShadowMapSize, 1);
    int2 baseCoord = int2(clamp(shadowUv * shadowSize, float2(0.0, 0.0), float2(shadowSize - 1, shadowSize - 1)));
    int pcfRadius = (int)clamp(round(directionalShadowSoftness), 0.0, 2.0);
    float bias = max(0.0, directionalShadowBias + directionalShadowNormalBias * 0.5);
    float visibility = 0.0;
    int sampleCount = 0;
    [loop]
    for (int offsetY = -1; offsetY <= 1; ++offsetY) {
        [loop]
        for (int offsetX = -1; offsetX <= 1; ++offsetX) {
            if (abs(offsetX) > pcfRadius || abs(offsetY) > pcfRadius) {
                continue;
            }
            int2 coord = clamp(baseCoord + int2(offsetX, offsetY), int2(0, 0), int2(shadowSize - 1, shadowSize - 1));
            float shadowDepth = asfloat(directionalShadowTex[coord]);
            visibility += lightDepth <= (shadowDepth + bias) ? 1.0 : 0.0;
            sampleCount++;
        }
    }
    return sampleCount > 0 ? visibility / sampleCount : 1.0;
}
)"
R"(

float CompositeNoise3(float3 p) {
    return frac(sin(dot(p, float3(12.9898, 78.233, 37.719))) * 43758.5453);
}

float CompositeFbm3(float3 p) {
    float value = 0.0;
    float amplitude = 0.5;
    float3 samplePos = p;
    [loop]
    for (int octave = 0; octave < 4; ++octave) {
        if (octave >= (performanceMode >= 2 ? 3 : (performanceMode == 1 ? 2 : 1))) {
            break;
        }
        value += CompositeNoise3(samplePos) * amplitude;
        samplePos = samplePos * 2.01 + float3(13.0, 29.0, 47.0);
        amplitude *= 0.5;
    }
    return saturate(value / (performanceMode >= 2 ? 0.875 : (performanceMode == 1 ? 0.75 : 0.5)));
}

float EstimateCompositeDepthGradient(float2 uv, float centerDepth) {
    float2 dx = float2(invFullResolution.x, 0.0);
    float2 dy = float2(0.0, invFullResolution.y);
    float depthL = ReadFullDepth(saturate(uv - dx));
    float depthR = ReadFullDepth(saturate(uv + dx));
    float depthU = ReadFullDepth(saturate(uv - dy));
    float depthD = ReadFullDepth(saturate(uv + dy));
    float gradient = max(abs(depthL - centerDepth), abs(depthR - centerDepth));
    gradient = max(gradient, max(abs(depthU - centerDepth), abs(depthD - centerDepth)));
    return gradient;
}

float4 SampleUpscaled(float2 uv, float fullDepth) {
    if (performanceMode < 2) {
        return volumeTex.SampleLevel(linearSampler, saturate(uv), 0);
    }

    float2 lowSize = float2(1.0 / invLowResolution.x, 1.0 / invLowResolution.y);
    float2 pixel = uv * lowSize - 0.5;
    float2 baseCoord = floor(pixel);
    float2 fracCoord = frac(pixel);
    float4 accum = float4(0.0, 0.0, 0.0, 0.0);
    float weightSum = 0.0;
    float4 bestSample = float4(0.0, 0.0, 0.0, 0.0);
    float bestDepthDelta = 1e9;

    [unroll]
    for (int oy = -1; oy <= 1; ++oy) {
        [unroll]
        for (int ox = -1; ox <= 1; ++ox) {
            float2 coord = baseCoord + float2((float)ox, (float)oy) + 0.5;
            float2 sampleUv = saturate(coord * invLowResolution);
            float2 offsetDelta = abs(float2((float)ox, (float)oy) - fracCoord);
            float bilinear = max(1.0 - offsetDelta.x, 0.0) * max(1.0 - offsetDelta.y, 0.0);
            bilinear = max(bilinear, (ox == 0 && oy == 0) ? 1.0 : 0.15);
            float lowDepth = lowDepthTex.SampleLevel(linearSampler, sampleUv, 0).r;
            float depthDelta = abs(lowDepth - fullDepth);
            float4 sampleValue = volumeTex.SampleLevel(linearSampler, sampleUv, 0);
            if (depthDelta < bestDepthDelta) {
                bestDepthDelta = depthDelta;
                bestSample = sampleValue;
            }
            float depthWeight = exp(-depthDelta * 2048.0);
            float weight = bilinear * depthWeight;
            accum += sampleValue * weight;
            weightSum += weight;
        }
    }

    if (bestDepthDelta > 0.002 || weightSum <= 0.05) {
        return bestSample;
    }

    return accum / max(weightSum, 0.0001);
}

float4 SampleMetricsUpscaled(float2 uv, float fullDepth) {
    if (performanceMode < 2) {
        return metricsTex.SampleLevel(linearSampler, saturate(uv), 0);
    }

    float2 lowSize = float2(1.0 / invLowResolution.x, 1.0 / invLowResolution.y);
    float2 pixel = uv * lowSize - 0.5;
    float2 baseCoord = floor(pixel);
    float2 fracCoord = frac(pixel);
    float4 accum = float4(0.0, 0.0, 0.0, 0.0);
    float weightSum = 0.0;
    float4 bestSample = float4(0.0, 0.0, 0.0, 0.0);
    float bestDepthDelta = 1e9;

    [unroll]
    for (int oy = -1; oy <= 1; ++oy) {
        [unroll]
        for (int ox = -1; ox <= 1; ++ox) {
            float2 coord = baseCoord + float2((float)ox, (float)oy) + 0.5;
            float2 sampleUv = saturate(coord * invLowResolution);
            float2 offsetDelta = abs(float2((float)ox, (float)oy) - fracCoord);
            float bilinear = max(1.0 - offsetDelta.x, 0.0) * max(1.0 - offsetDelta.y, 0.0);
            bilinear = max(bilinear, (ox == 0 && oy == 0) ? 1.0 : 0.15);
            float lowDepth = lowDepthTex.SampleLevel(linearSampler, sampleUv, 0).r;
            float depthDelta = abs(lowDepth - fullDepth);
            float4 sampleValue = metricsTex.SampleLevel(linearSampler, sampleUv, 0);
            if (depthDelta < bestDepthDelta) {
                bestDepthDelta = depthDelta;
                bestSample = sampleValue;
            }
            float depthWeight = exp(-depthDelta * 2048.0);
            float weight = bilinear * depthWeight;
            accum += sampleValue * weight;
            weightSum += weight;
        }
    }

    if (bestDepthDelta > 0.002 || weightSum <= 0.05) {
        return bestSample;
    }

    return accum / max(weightSum, 0.0001);
}

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    float2 uv = position.xy * invFullResolution;
    float fullDepth = ReadFullDepth(uv);
    if (debugMode == DEBUG_FOG_DENSITY) {
        float4 metrics = SampleMetricsUpscaled(uv, fullDepth);
        float densityNorm = saturate((metrics.r - debugDensityBias) * max(debugDensityScale, 0.001));
        densityNorm = pow(densityNorm, 0.72);
        return float4(BuildFogDensityDebugColor(densityNorm), 1.0);
    }
    if (debugMode == DEBUG_LIGHT_ENERGY) {
        float4 metrics = SampleMetricsUpscaled(uv, fullDepth);
        float lightNorm = saturate((metrics.g - debugLightBias) * max(debugLightScale, 0.001));
        lightNorm = pow(lightNorm, 0.72);
        return float4(BuildLightEnergyDebugColor(lightNorm), 1.0);
    }
    if (debugMode == DEBUG_FINAL_VOLUME) {
        float4 volumeSample = SampleUpscaled(uv, fullDepth);
        return float4(BuildFinalVolumeDebugColor(volumeSample.rgb), 1.0);
    }
    if (debugMode == DEBUG_TRANSMITTANCE) {
        float4 metrics = SampleMetricsUpscaled(uv, fullDepth);
        float transmittance = exp(-max(metrics.a, 0.0));
        return float4(BuildTransmittanceDebugColor(transmittance), 1.0);
    }
    if (debugMode == DEBUG_SHADOW_OCCLUSION) {
        if (fullDepth >= 0.9999) {
            return float4(0.12, 0.16, 0.22, 1.0);
        }
        float3 worldPos = ReconstructCompositeWorldPosition(uv, fullDepth);
        float visibility = SampleDirectionalShadowVisibility(worldPos);
        return float4(BuildShadowOcclusionDebugColor(visibility), 1.0);
    }
    float4 volumeSample = SampleUpscaled(uv, fullDepth);
    float4 metricsSample = SampleMetricsUpscaled(uv, fullDepth);
    float3 volume = max(volumeSample.rgb, float3(0.0, 0.0, 0.0));
    float fogAmount = saturate(volumeSample.a);
    float metricsOpticalDepth = max(metricsSample.a, 0.0);
    float metricsTransmittance = exp(-metricsOpticalDepth);
    float metricsFogAmount = saturate(1.0 - metricsTransmittance);

    if (fullDepth < 0.9999) {
        float3 surfaceWorldPos = ReconstructCompositeWorldPosition(uv, fullDepth);
        float3 toSurface = surfaceWorldPos - cameraWorldPosition;
        float surfaceDistance = length(toSurface);
        float distanceSpan = max(maxDistance - startDistance, 1.0);
        float distanceNorm = saturate((surfaceDistance - startDistance) / distanceSpan);
        float sceneDistanceNorm = saturate(surfaceDistance / max(maxDistance + 1400.0, 1600.0));
        float3 viewDir = normalize(toSurface);
        float horizonFactor = saturate(1.0 - abs(viewDir.y));
        float depthGradient = EstimateCompositeDepthGradient(uv, fullDepth);
        float edgeFactor = smoothstep(0.00035, 0.01, depthGradient);
        float depthExtStrength = max(depthExtinctionStrength, 0.0);
        float depthExtExponentSafe = max(depthExtinctionExponent, 0.25);
        float heightMask = 1.0;
        if (heightFogEnabled != 0) {
            heightMask = exp(-max(surfaceWorldPos.y - baseHeight, 0.0) * heightFalloff * 0.28);
            heightMask = lerp(0.55, 1.0, heightMask);
        }
        float largeScaleNoise = CompositeFbm3(surfaceWorldPos * float3(0.0017, 0.0, 0.0017) + float3(5.0, 17.0, 31.0));
        float detailNoise = performanceMode >= 1
                                ? CompositeFbm3(surfaceWorldPos * float3(0.0048, 0.0014, 0.0048) + float3(23.0, 41.0, 59.0))
                                : 0.5;
        float billowNoise = performanceMode >= 2
                                ? CompositeFbm3(surfaceWorldPos * float3(0.00085, 0.00045, 0.00085) + float3(71.0, 13.0, 97.0))
                                : largeScaleNoise;
        float streakNoise = performanceMode >= 2
                                ? CompositeFbm3(surfaceWorldPos * float3(0.0032, 0.0075, 0.0032) + float3(11.0, 89.0, 37.0))
                                : 0.5;
        float macroNoiseWeight = saturate(macroNoiseStrength);
        float macroNoiseBoost = 1.0 + max(macroNoiseStrength - 1.0, 0.0) * 0.35;
        float valleyFogWeight = saturate(valleyFogStrength);
        float valleyFogBoost = 1.0 + max(valleyFogStrength - 1.0, 0.0) * 0.28;
        float aerialWeight = saturate(aerialPerspectiveStrength * 0.75);
        float aerialBoost = 1.0 + max(aerialPerspectiveStrength - 1.0, 0.0) * 0.55;
        float edgeWeight = saturate(edgeHazeStrength);
        float edgeBoost = 1.0 + max(edgeHazeStrength - 1.0, 0.0) * 0.25;
        float valleyPool = 1.0;
        if (heightFogEnabled != 0) {
            float valleyHeight = saturate(1.0 - max(surfaceWorldPos.y - baseHeight, 0.0) / 220.0);
            valleyPool = lerp(0.78, 1.32, valleyHeight);
        }
        float farBand = smoothstep(0.16, 0.96, distanceNorm) * lerp(0.85, 1.4, horizonFactor);
        float macroVfx = lerp(1.0, lerp(0.82, 1.22, largeScaleNoise) * lerp(0.82, 1.28, billowNoise), macroNoiseWeight) * macroNoiseBoost;
        float microVfx = lerp(0.9, 1.14, detailNoise) * lerp(0.9, 1.12, streakNoise);
        float valleyVfx = lerp(1.0, valleyPool, valleyFogWeight) * valleyFogBoost;
        float vfxModulation = macroVfx * microVfx * valleyVfx;
        float baseDistance = max(surfaceDistance - startDistance, 0.0);
        float distanceCoverage = smoothstep(0.04, 0.92, distanceNorm);
        float horizonCoverage = lerp(0.75, 1.65, horizonFactor);
        float extinctionScale = lerp(0.018, 0.11, distanceCoverage) * aerialBoost;
        float analyticalFog = 1.0 - exp(-density * baseDistance * extinctionScale * horizonCoverage);
        float distanceRampFog = distanceCoverage * (0.18 + 0.42 * aerialWeight) * heightMask * lerp(0.82, 1.18, edgeFactor);
        float opticalDepthFog = metricsFogAmount * lerp(0.92, 1.9, distanceNorm) * lerp(0.95, 1.4, horizonFactor);
        float extinctionFog = saturate(
            (1.0 - exp(-pow(max(distanceNorm, 0.001), depthExtExponentSafe) * depthExtStrength *
                        lerp(0.9, 4.8, horizonFactor))) *
            lerp(0.88, 1.35, edgeFactor));
        float sceneDistanceFog = saturate(
            (1.0 - exp(-pow(max(sceneDistanceNorm, 0.001), 1.22) * (0.85 + depthExtStrength * 0.9) *
                        lerp(1.0, 2.25, horizonFactor))) *
            heightMask * lerp(0.92, 1.28, edgeFactor));
        float depthFogAmount = saturate(max(max(analyticalFog, distanceRampFog * saturate(density * 1.8 + 0.08)),
                                            max(max(opticalDepthFog, extinctionFog), sceneDistanceFog)));
        float aerialDepthWeight = lerp(1.0, lerp(1.0, 2.1, horizonFactor) * lerp(0.9, 1.42, farBand), aerialWeight) * aerialBoost;
        float edgeHazeWeight = lerp(1.0, lerp(1.0, 1.2, edgeFactor), edgeWeight) * edgeBoost;
        depthFogAmount *= aerialDepthWeight * heightMask * edgeHazeWeight;
        depthFogAmount = saturate(pow(depthFogAmount, lerp(0.9, 0.68, saturate(depthExtStrength * 0.18))));
        float3 depthFogColor = fogColor * ambientIntensity * depthFogAmount * vfxModulation *
                               lerp(1.1, 2.45, distanceNorm) * lerp(1.0, 1.2, edgeFactor) *
                               lerp(1.0, 1.35, aerialWeight) * lerp(1.0, 1.45, horizonFactor) *
                               lerp(1.0, 1.55, sceneDistanceNorm);
        float remainingTransmittance = 1.0 - fogAmount;
        volume += depthFogColor * remainingTransmittance;
        fogAmount = saturate(fogAmount + depthFogAmount * remainingTransmittance);
        fogAmount = max(fogAmount, saturate(metricsFogAmount * lerp(0.92, 1.6, distanceNorm) * lerp(0.95, 1.25, heightMask)));
        fogAmount = max(fogAmount, saturate(sceneDistanceFog * lerp(0.65, 1.0, heightMask)));
    }

    return float4(volume, fogAmount);
}
)";
} // namespace

GfxRenderingAPIDX11::~GfxRenderingAPIDX11() {
}

GfxRenderingAPIDX11::GfxRenderingAPIDX11(GfxWindowBackendDXGI* backend) {
    mWindowBackend = backend;
}

void GfxRenderingAPIDX11::CreateDepthStencilObjects(uint32_t width, uint32_t height, uint32_t msaa_count,
                                                    ID3D11DepthStencilView** view, ID3D11ShaderResourceView** srv) {
    D3D11_TEXTURE2D_DESC texture_desc;
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format =
        mFeatureLevel >= D3D_FEATURE_LEVEL_10_0 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    texture_desc.SampleDesc.Count = msaa_count;
    texture_desc.SampleDesc.Quality = 0;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | (srv != nullptr ? D3D11_BIND_SHADER_RESOURCE : 0);
    texture_desc.CPUAccessFlags = 0;
    texture_desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> texture;
    ThrowIfFailed(mDevice->CreateTexture2D(&texture_desc, nullptr, texture.GetAddressOf()));

    D3D11_DEPTH_STENCIL_VIEW_DESC view_desc;
    view_desc.Format = mFeatureLevel >= D3D_FEATURE_LEVEL_10_0 ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D24_UNORM_S8_UINT;
    view_desc.Flags = 0;
    if (msaa_count > 1) {
        view_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        view_desc.Texture2DMS.UnusedField_NothingToDefine = 0;
    } else {
        view_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MipSlice = 0;
    }

    ThrowIfFailed(mDevice->CreateDepthStencilView(texture.Get(), &view_desc, view));

    if (srv != nullptr) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
        srv_desc.Format =
            mFeatureLevel >= D3D_FEATURE_LEVEL_10_0 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        srv_desc.ViewDimension = msaa_count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels = -1;

        ThrowIfFailed(mDevice->CreateShaderResourceView(texture.Get(), &srv_desc, srv));
    }
}
static bool CreateDeviceFunc(class GfxRenderingAPIDX11* self, bool SoftwareRenderer) {
#if DEBUG_D3D
    UINT device_creation_flags = D3D11_CREATE_DEVICE_DEBUG;
#else
    UINT device_creation_flags = 0;
#endif
    bool CreationFailed = false;
    const char HardwareText[320] = "\nUsing software renderer. Performance issues are to be expected.\n\n"
                                   "Please check your preferred GPU in Windows graphics or GPU driver settings and "
                                   "make sure you have the correct GPU drivers installed.\n\n"
                                   "You can also try to change the graphic backend of the port in its config file.\n"
                                   "Window->Backend->Id\n"
                                   "0 = DX11, 1 = OpenGL";
    const char SoftwareText[33] = "\nUsing software renderer failed.";

    if (SoftwareRenderer) {
        SPDLOG_INFO("Using software renderer.");
    }

    HRESULT res = self->mDX11CreateDevice(
        NULL, SoftwareRenderer ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, device_creation_flags, NULL,
        NULL, D3D11_SDK_VERSION, self->mDevice.GetAddressOf(), &self->mFeatureLevel, self->mContext.GetAddressOf());

    // Get and log name of adapter
    IDXGIDevice* DXGIDevice = nullptr;
    IDXGIAdapter* Adapter = nullptr;
    DXGI_ADAPTER_DESC adapterDesc;
    std::wstring adapterName;
    char adapterNameCStr[128] = "";
    char error_message[512];
    HRESULT res2;

    res2 = self->mDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&DXGIDevice);
    if (SUCCEEDED(res2)) {
        res2 = DXGIDevice->GetAdapter(&Adapter);
        if (SUCCEEDED(res2)) {
            res2 = Adapter->GetDesc(&adapterDesc);
            if (SUCCEEDED(res2)) {
                adapterName = adapterDesc.Description;
                wcstombs(adapterNameCStr, adapterName.c_str(), 128);
            }
            Adapter->Release();
        }
        DXGIDevice->Release();
    }
    SPDLOG_INFO("Using D3D adapter: {0}", adapterNameCStr);

    if (FAILED(res)) {
        CreationFailed = true;
        SPDLOG_WARN("Failed to create a D3D device. HRESULT: 0x{0:08x}", res);
        snprintf(error_message, sizeof(error_message), "Failed to create a D3D device on %s\nHRESULT: 0x%08X%s",
                 adapterNameCStr, res, SoftwareRenderer ? SoftwareText : HardwareText);
    }

    else if (self->mFeatureLevel < D3D_FEATURE_LEVEL_10_0) {
        CreationFailed = true;
        SPDLOG_WARN("D3D adapter doesn't support D3D feature level 10_0 or greater.");
        snprintf(error_message, sizeof(error_message), "%s doesn't support D3D feature level 10_0 or greater.%s",
                 adapterNameCStr, SoftwareRenderer ? SoftwareText : HardwareText);
    }

    else if (self->mFeatureLevel < D3D_FEATURE_LEVEL_10_1) {
        SPDLOG_WARN("D3D adapter doesn't support D3D feature level 10_1 or greater. MSAA setting will be ignored.");

    } else {
        // Check for Compute Shader support
        if (self->mDevice != NULL) {
            D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS features;
            self->mDevice->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &features,
                                               sizeof(D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS));
            if (features.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x == false) {
                CreationFailed = true;
                SPDLOG_WARN("D3D adapter doesn't support compute shaders.");
                snprintf(error_message, sizeof(error_message), "%s doesn't support compute shaders.%s", adapterNameCStr,
                         SoftwareRenderer ? SoftwareText : HardwareText);
            }
        }
    }

    if (CreationFailed) {
        if (self->mContext) {
            self->mContext->Release();
        }
        if (self->mDevice) {
            self->mDevice->Release();
        }
        MessageBoxA(self->mWindowBackend->GetWindowHandle(), error_message, "Warning", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
};

void GfxRenderingAPIDX11::Init() {
    // Load d3d11.dll
    mDX11Module = LoadLibraryW(L"d3d11.dll");
    if (mDX11Module == nullptr) {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), mWindowBackend->GetWindowHandle(), "d3d11.dll not found");
    }
    mDX11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(mDX11Module, "D3D11CreateDevice");

    // Load D3DCompiler_47.dll
    mCompilerModule = LoadLibraryW(L"D3DCompiler_47.dll");
    if (mCompilerModule == nullptr) {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), mWindowBackend->GetWindowHandle(),
                      "D3DCompiler_47.dll not found");
    }
    mD3dCompile = (pD3DCompile)GetProcAddress(mCompilerModule, "D3DCompile");

    // Create D3D11 mDevice

    mWindowBackend->CreateFactoryAndDevice(DEBUG_D3D, 11, this, CreateDeviceFunc);

    // Create the swap chain
    mWindowBackend->CreateSwapChain(mDevice.Get(), [this]() {
        mFrameBuffers[0].render_target_view.Reset();
        mTextures[mFrameBuffers[0].texture_id].texture.Reset();
        mContext->ClearState();
        mContext->Flush();

        mLastShaderProgram = nullptr;
        mLastVertexBufferStride = 0;
        mLastBlendState.Reset();
        for (int i = 0; i < SHADER_MAX_TEXTURES; i++) {
            mLastResourceViews[i].Reset();
            mLastSamplerStates[i].Reset();
        }
        mLastDepthTest = -1;
        mLastDepthMask = -1;
        mLastZmodeDecal = -1;
        mLastPrimitaveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    });

    // Create D3D Debug mDevice if in debug mode

#if DEBUG_D3D
    ThrowIfFailed(mDevice->QueryInterface(__uuidof(ID3D11Debug), (void**)debug.GetAddressOf()),
                  mWindowBackend->GetWindowHandle(), "Failed to get ID3D11Debug device.");
#endif

    // Create the default framebuffer which represents the window
    FramebufferDX11& fb = mFrameBuffers[CreateFramebuffer()];

    // Check the size of the window
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc;
    ThrowIfFailed(mWindowBackend->GetSwapChain()->GetDesc1(&swap_chain_desc));
    mTextures[fb.texture_id].width = swap_chain_desc.Width;
    mTextures[fb.texture_id].height = swap_chain_desc.Height;
    fb.msaa_level = 1;

    for (uint32_t sample_count = 1; sample_count <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; sample_count++) {
        ThrowIfFailed(mDevice->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, sample_count,
                                                             &mMsaaNumQualityLevels[sample_count - 1]));
    }

    // Create main vertex buffer

    D3D11_BUFFER_DESC vertex_buffer_desc;
    ZeroMemory(&vertex_buffer_desc, sizeof(D3D11_BUFFER_DESC));

    vertex_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    vertex_buffer_desc.ByteWidth = 256 * 32 * 3 * sizeof(float); // Same as buf_vbo size in gfx_pc
    vertex_buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertex_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    vertex_buffer_desc.MiscFlags = 0;

    ThrowIfFailed(mDevice->CreateBuffer(&vertex_buffer_desc, nullptr, mVertexBuffer.GetAddressOf()),
                  mWindowBackend->GetWindowHandle(), "Failed to create vertex buffer.");

    // Create per-frame constant buffer

    D3D11_BUFFER_DESC constant_buffer_desc;
    ZeroMemory(&constant_buffer_desc, sizeof(D3D11_BUFFER_DESC));

    constant_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    constant_buffer_desc.ByteWidth = sizeof(PerFrameCB);
    constant_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constant_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    constant_buffer_desc.MiscFlags = 0;

    ThrowIfFailed(mDevice->CreateBuffer(&constant_buffer_desc, nullptr, mPerFrameCb.GetAddressOf()),
                  mWindowBackend->GetWindowHandle(), "Failed to create per-frame constant buffer.");

    // Create per-draw constant buffer

    constant_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    constant_buffer_desc.ByteWidth = sizeof(PerDrawCB);
    constant_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constant_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    constant_buffer_desc.MiscFlags = 0;

    ThrowIfFailed(mDevice->CreateBuffer(&constant_buffer_desc, nullptr, mPerDrawCb.GetAddressOf()),
                  mWindowBackend->GetWindowHandle(), "Failed to create per-draw constant buffer.");

    // Create per-prim-depth constant buffer (G_ZS_PRIM), uploaded only when mPrimDepthDirty

    constant_buffer_desc.ByteWidth = sizeof(PerPrimDepthCB);
    ThrowIfFailed(mDevice->CreateBuffer(&constant_buffer_desc, nullptr, mPerPrimDepthCb.GetAddressOf()),
                  mWindowBackend->GetWindowHandle(), "Failed to create per-prim-depth constant buffer.");

    // Create compute shader that can be used to retrieve depth buffer values

    const char* shader_source = R"(
sampler my_sampler : register(s0);
Texture2D<float> tex : register(t0);
StructuredBuffer<int2> coord : register(t1);
RWStructuredBuffer<float> output : register(u0);

[numthreads(1, 1, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
    output[DTid.x] = tex.Load(int3(coord[DTid.x], 0));
}
)";

    const char* shader_source_msaa = R"(
sampler my_sampler : register(s0);
Texture2DMS<float, 2> tex : register(t0);
StructuredBuffer<int2> coord : register(t1);
RWStructuredBuffer<float> output : register(u0);

[numthreads(1, 1, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
    output[DTid.x] = tex.Load(coord[DTid.x], 0);
}
)";

#if DEBUG_D3D
    UINT compile_flags = D3DCOMPILE_DEBUG;
#else
    UINT compile_flags = D3DCOMPILE_OPTIMIZATION_LEVEL2;
#endif

    ComPtr<ID3DBlob> cs, error_blob;
    HRESULT hr;

    hr = mD3dCompile(shader_source, strlen(shader_source), nullptr, nullptr, nullptr, "CSMain", "cs_4_0", compile_flags,
                     0, cs.GetAddressOf(), error_blob.GetAddressOf());

    if (FAILED(hr)) {
        char* err = (char*)error_blob->GetBufferPointer();
        MessageBoxA(mWindowBackend->GetWindowHandle(), err, "Error", MB_OK | MB_ICONERROR);
        throw Ship::HResultException(hr, "Compute shader compilation failed");
    }

    ThrowIfFailed(mDevice->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr,
                                               mComputeShader.GetAddressOf()));

    hr = mD3dCompile(shader_source_msaa, strlen(shader_source_msaa), nullptr, nullptr, nullptr, "CSMain", "cs_4_1",
                     compile_flags, 0, mComputeShaderMsaaBlob.GetAddressOf(), error_blob.ReleaseAndGetAddressOf());

    if (FAILED(hr)) {
        char* err = (char*)error_blob->GetBufferPointer();
        MessageBoxA(mWindowBackend->GetWindowHandle(), err, "Error", MB_OK | MB_ICONERROR);
        throw Ship::HResultException(hr, "MSAA compute shader compilation failed");
    }

    // Create ImGui

    Fast::GuiWindowInitData window_impl;
    window_impl.Dx11 = { mWindowBackend->GetWindowHandle(), mContext.Get(), mDevice.Get() };
    window_impl.Backend = WindowBackend::FAST3D_DXGI_DX11;
    std::dynamic_pointer_cast<Fast::Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui())
        ->Init(window_impl);
}

int GfxRenderingAPIDX11::GetMaxTextureSize() {
    return D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}

const char* GfxRenderingAPIDX11::GetName() {
    return "DirectX 11";
}

struct GfxClipParameters GfxRenderingAPIDX11::GetClipParameters() {
    return { true, false };
}

void GfxRenderingAPIDX11::UnloadShader(struct ShaderProgram* old_prg) {
}

void GfxRenderingAPIDX11::LoadShader(struct ShaderProgram* new_prg) {
    mShaderProgram = (struct ShaderProgramD3D11*)new_prg;
}

void GfxRenderingAPIDX11::ClearShaderCache() {
    mShaderProgramPool.clear();
}

struct ShaderProgram* GfxRenderingAPIDX11::CreateAndLoadNewShader(uint64_t shader_id0, uint64_t shader_id1) {
    CCFeatures cc_features;
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    char* buf;
    size_t len, numFloats;

    auto shader = gfx_direct3d_common_build_shader(numFloats, cc_features, false,
                                                   mCurrentFilterMode == FILTER_THREE_POINT, mSrgbMode);

    buf = shader.data();
    len = shader.size();

    ComPtr<ID3DBlob> vs, ps;
    ComPtr<ID3DBlob> error_blob;

#if DEBUG_D3D
    UINT compile_flags = D3DCOMPILE_DEBUG;
#else
    UINT compile_flags = D3DCOMPILE_OPTIMIZATION_LEVEL2;
#endif

    HRESULT hr = mD3dCompile(buf, len, nullptr, nullptr, nullptr, "VSMain", "vs_4_0", compile_flags, 0,
                             vs.GetAddressOf(), error_blob.GetAddressOf());

    if (FAILED(hr)) {
        char* err = (char*)error_blob->GetBufferPointer();
        MessageBoxA(mWindowBackend->GetWindowHandle(), err, "Error", MB_OK | MB_ICONERROR);
        throw Ship::HResultException(hr, "Vertex shader compilation failed");
    }

    hr = mD3dCompile(buf, len, nullptr, nullptr, nullptr, "PSMain", "ps_4_0", compile_flags, 0, ps.GetAddressOf(),
                     error_blob.GetAddressOf());

    if (FAILED(hr)) {
        char* err = (char*)error_blob->GetBufferPointer();
        MessageBoxA(mWindowBackend->GetWindowHandle(), err, "Error", MB_OK | MB_ICONERROR);
        throw Ship::HResultException(hr, "Pixel shader compilation failed");
    }

    struct ShaderProgramD3D11* prg = &mShaderProgramPool[std::make_pair(shader_id0, shader_id1)];

    ThrowIfFailed(mDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                              prg->vertex_shader.GetAddressOf()));
    ThrowIfFailed(mDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr,
                                             prg->pixel_shader.GetAddressOf()));

    // Input Layout

    D3D11_INPUT_ELEMENT_DESC ied[16];
    uint8_t ied_index = 0;
    ied[ied_index++] = {
        "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0
    };
    for (UINT i = 0; i < 2; i++) {
        if (cc_features.usedTextures[i]) {
            ied[ied_index++] = {
                "TEXCOORD", i, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0
            };
            if (cc_features.clamp[i][0]) {
                ied[ied_index++] = { "TEXCLAMPS",
                                     i,
                                     DXGI_FORMAT_R32_FLOAT,
                                     0,
                                     D3D11_APPEND_ALIGNED_ELEMENT,
                                     D3D11_INPUT_PER_VERTEX_DATA,
                                     0 };
            }
            if (cc_features.clamp[i][1]) {
                ied[ied_index++] = { "TEXCLAMPT",
                                     i,
                                     DXGI_FORMAT_R32_FLOAT,
                                     0,
                                     D3D11_APPEND_ALIGNED_ELEMENT,
                                     D3D11_INPUT_PER_VERTEX_DATA,
                                     0 };
            }
        }
    }
    if (cc_features.opt_fog) {
        ied[ied_index++] = {
            "FOG", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0
        };
    }
    if (cc_features.opt_grayscale) {
        ied[ied_index++] = { "GRAYSCALE",
                             0,
                             DXGI_FORMAT_R32G32B32A32_FLOAT,
                             0,
                             D3D11_APPEND_ALIGNED_ELEMENT,
                             D3D11_INPUT_PER_VERTEX_DATA,
                             0 };
    }
    for (unsigned int i = 0; i < cc_features.numInputs; i++) {
        DXGI_FORMAT format = cc_features.opt_alpha ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32G32B32_FLOAT;
        ied[ied_index++] = { "INPUT", i, format, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    }

    ThrowIfFailed(mDevice->CreateInputLayout(ied, ied_index, vs->GetBufferPointer(), vs->GetBufferSize(),
                                             prg->input_layout.GetAddressOf()));

    // Blend state

    D3D11_BLEND_DESC blend_desc;
    ZeroMemory(&blend_desc, sizeof(D3D11_BLEND_DESC));

    if (cc_features.opt_alpha) {
        blend_desc.RenderTarget[0].BlendEnable = true;
        blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        blend_desc.RenderTarget[0].DestBlendAlpha =
            D3D11_BLEND_ONE; // We initially clear alpha to 1.0f and want to keep it at 1.0f
        blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    } else {
        blend_desc.RenderTarget[0].BlendEnable = false;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }

    ThrowIfFailed(mDevice->CreateBlendState(&blend_desc, prg->blend_state.GetAddressOf()));

    // Save some values

    prg->shader_id0 = shader_id0;
    prg->shader_id1 = shader_id1;
    prg->numInputs = cc_features.numInputs;
    prg->numFloats = numFloats;
    prg->usedTextures[0] = cc_features.usedTextures[0];
    prg->usedTextures[1] = cc_features.usedTextures[1];
    prg->usedTextures[2] = cc_features.used_masks[0];
    prg->usedTextures[3] = cc_features.used_masks[1];
    prg->usedTextures[4] = cc_features.used_blend[0];
    prg->usedTextures[5] = cc_features.used_blend[1];

    return (struct ShaderProgram*)(mShaderProgram = prg);
}

struct ShaderProgram* GfxRenderingAPIDX11::LookupShader(uint64_t shader_id0, uint64_t shader_id1) {
    auto it = mShaderProgramPool.find(std::make_pair(shader_id0, shader_id1));
    return it == mShaderProgramPool.end() ? nullptr : (struct ShaderProgram*)&it->second;
}

void GfxRenderingAPIDX11::ShaderGetInfo(struct ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    struct ShaderProgramD3D11* p = (struct ShaderProgramD3D11*)prg;

    *numInputs = p->numInputs;
    usedTextures[0] = p->usedTextures[0];
    usedTextures[1] = p->usedTextures[1];
}

uint32_t GfxRenderingAPIDX11::NewTexture() {
    mTextures.resize(mTextures.size() + 1);
    return (uint32_t)(mTextures.size() - 1);
}

void GfxRenderingAPIDX11::DeleteTexture(uint32_t texID) {
    // glDeleteTextures(1, &texID);
}

void GfxRenderingAPIDX11::SelectTexture(int tile, uint32_t texture_id) {
    mCurrentTile = tile;
    mCurrentTextureIds[tile] = texture_id;
}

static D3D11_TEXTURE_ADDRESS_MODE gfx_cm_to_d3d11(uint32_t val) {
    // TODO: handle G_TX_MIRROR | G_TX_CLAMP
    if (val & G_TX_CLAMP) {
        return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
    return (val & G_TX_MIRROR) ? D3D11_TEXTURE_ADDRESS_MIRROR : D3D11_TEXTURE_ADDRESS_WRAP;
}

void GfxRenderingAPIDX11::UploadTexture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }

    // Create texture

    TextureData* texture_data = &mTextures[mCurrentTextureIds[mCurrentTile]];
    texture_data->width = width;
    texture_data->height = height;

    D3D11_TEXTURE2D_DESC texture_desc;
    ZeroMemory(&texture_desc, sizeof(D3D11_TEXTURE2D_DESC));

    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.CPUAccessFlags = 0;
    texture_desc.MiscFlags = 0; // D3D11_RESOURCE_MISC_GENERATE_MIPS ?
    texture_desc.ArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.SampleDesc.Quality = 0;

    D3D11_SUBRESOURCE_DATA resource_data;
    resource_data.pSysMem = rgba32_buf;
    resource_data.SysMemPitch = width * 4;
    resource_data.SysMemSlicePitch = resource_data.SysMemPitch * height;

    ThrowIfFailed(
        mDevice->CreateTexture2D(&texture_desc, &resource_data, texture_data->texture.ReleaseAndGetAddressOf()));

    // Create shader resource view from texture

    ThrowIfFailed(mDevice->CreateShaderResourceView(texture_data->texture.Get(), nullptr,
                                                    texture_data->resource_view.ReleaseAndGetAddressOf()));
}

void GfxRenderingAPIDX11::SetSamplerParameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    D3D11_SAMPLER_DESC sampler_desc;
    ZeroMemory(&sampler_desc, sizeof(D3D11_SAMPLER_DESC));

    sampler_desc.Filter = linear_filter && mCurrentFilterMode == FILTER_LINEAR ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
                                                                               : D3D11_FILTER_MIN_MAG_MIP_POINT;

    sampler_desc.AddressU = gfx_cm_to_d3d11(cms);
    sampler_desc.AddressV = gfx_cm_to_d3d11(cmt);
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    TextureData* texture_data = &mTextures[mCurrentTextureIds[tile]];
    texture_data->linear_filtering = linear_filter;

    // This function is called twice per texture, the first one only to set default values.
    // Maybe that could be skipped? Anyway, make sure to release the first default sampler
    // state before setting the actual one.
    texture_data->sampler_state.Reset();

    ThrowIfFailed(mDevice->CreateSamplerState(&sampler_desc, texture_data->sampler_state.GetAddressOf()));
}

void GfxRenderingAPIDX11::SetDepthTestAndMask(bool depth_test, bool depth_mask) {
    mCurrentDepthTest = depth_test;
    mCurrentDepthMask = depth_mask;
}

void GfxRenderingAPIDX11::SetCurrentPrimDepth(float depth) {
    if (depth != mCurrentPrimDepth) {
        mCurrentPrimDepth = depth;
        mPrimDepthDirty = true;
    }
}

void GfxRenderingAPIDX11::SetZmodeDecal(bool zmode_decal) {
    mCurrentZmodeDecal = zmode_decal;
}

void GfxRenderingAPIDX11::SetViewport(int x, int y, int width, int height) {
    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = x;
    viewport.TopLeftY = mRenderTargetHeight - y - height;
    viewport.Width = width;
    viewport.Height = height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    mContext->RSSetViewports(1, &viewport);
}

void GfxRenderingAPIDX11::SetScissor(int x, int y, int width, int height) {
    D3D11_RECT rect;
    rect.left = x;
    rect.top = mRenderTargetHeight - y - height;
    rect.right = x + width;
    rect.bottom = mRenderTargetHeight - y;

    mContext->RSSetScissorRects(1, &rect);
}

void GfxRenderingAPIDX11::SetUseAlpha(bool use_alpha) {
    // Already part of the pipeline state from shader info
}

void GfxRenderingAPIDX11::DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {

    if (mLastDepthTest != mCurrentDepthTest || mLastDepthMask != mCurrentDepthMask) {
        mLastDepthTest = mCurrentDepthTest;
        mLastDepthMask = mCurrentDepthMask;

        mDepthStencilState.Reset();

        D3D11_DEPTH_STENCIL_DESC depth_stencil_desc;
        ZeroMemory(&depth_stencil_desc, sizeof(D3D11_DEPTH_STENCIL_DESC));

        depth_stencil_desc.DepthEnable = mCurrentDepthTest || mCurrentDepthMask;
        depth_stencil_desc.DepthWriteMask =
            mCurrentDepthMask ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        depth_stencil_desc.DepthFunc = mCurrentDepthTest
                                           ? (mCurrentZmodeDecal ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_LESS)
                                           : D3D11_COMPARISON_ALWAYS;
        depth_stencil_desc.StencilEnable = false;

        ThrowIfFailed(mDevice->CreateDepthStencilState(&depth_stencil_desc, mDepthStencilState.GetAddressOf()));
        mContext->OMSetDepthStencilState(mDepthStencilState.Get(), 0);
    }

    if (mLastZmodeDecal != mCurrentZmodeDecal) {
        mLastZmodeDecal = mCurrentZmodeDecal;

        mRasterizerState.Reset();

        D3D11_RASTERIZER_DESC rasterizer_desc;
        ZeroMemory(&rasterizer_desc, sizeof(D3D11_RASTERIZER_DESC));

        rasterizer_desc.FillMode = D3D11_FILL_SOLID;
        rasterizer_desc.CullMode = D3D11_CULL_NONE;
        rasterizer_desc.FrontCounterClockwise = true;
        rasterizer_desc.DepthBias = 0;
        // SSDB = SlopeScaledDepthBias 120 leads to -2 at 240p which is the same as N64 mode which has very little
        // fighting
        const int n64modeFactor = 120;
        const int noVanishFactor = 100;
        float SSDB = -2;

        switch (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_Z_FIGHTING_MODE, 0)) {
            case 1: // scaled z-fighting (N64 mode like)
                SSDB = -1.0f * (float)mRenderTargetHeight / n64modeFactor;
                break;
            case 2: // no vanishing paths
                SSDB = -1.0f * (float)mRenderTargetHeight / noVanishFactor;
                break;
            case 0: // disabled
            default:
                SSDB = -2;
        }
        rasterizer_desc.SlopeScaledDepthBias = mCurrentZmodeDecal ? SSDB : 0.0f;
        rasterizer_desc.DepthBiasClamp = 0.0f;
        rasterizer_desc.DepthClipEnable = false;
        rasterizer_desc.ScissorEnable = true;
        rasterizer_desc.MultisampleEnable = false;
        rasterizer_desc.AntialiasedLineEnable = false;

        ThrowIfFailed(mDevice->CreateRasterizerState(&rasterizer_desc, mRasterizerState.GetAddressOf()));
        mContext->RSSetState(mRasterizerState.Get());
    }

    bool textures_changed = false;

    for (int i = 0; i < SHADER_MAX_TEXTURES; i++) {
        if (mShaderProgram->usedTextures[i]) {
            // mTextures is append-only (NewTexture just resizes +1, DeleteTexture is a no-op),
            // so this is really just catching stale IDs left over from before we zero-initialized
            // mCurrentTextureIds. No entries are ever removed, so gaps aren't a concern.
            if (mCurrentTextureIds[i] >= mTextures.size()) {
                continue;
            }
            if (mLastResourceViews[i].Get() != mTextures[mCurrentTextureIds[i]].resource_view.Get()) {
                mLastResourceViews[i] = mTextures[mCurrentTextureIds[i]].resource_view.Get();
                mContext->PSSetShaderResources(i, 1, mTextures[mCurrentTextureIds[i]].resource_view.GetAddressOf());

                if (mCurrentFilterMode == FILTER_THREE_POINT) {
                    mPerDrawCbData.mTextures[i].width = mTextures[mCurrentTextureIds[i]].width;
                    mPerDrawCbData.mTextures[i].height = mTextures[mCurrentTextureIds[i]].height;
                    mPerDrawCbData.mTextures[i].linear_filtering = mTextures[mCurrentTextureIds[i]].linear_filtering;
                    textures_changed = true;
                }

                if (mLastSamplerStates[i].Get() != mTextures[mCurrentTextureIds[i]].sampler_state.Get()) {
                    mLastSamplerStates[i] = mTextures[mCurrentTextureIds[i]].sampler_state.Get();
                }
            }
            mContext->PSSetSamplers(i, 1, mTextures[mCurrentTextureIds[i]].sampler_state.GetAddressOf());
        }
    }

    // Set per-draw constant buffer
    if (textures_changed) {
        D3D11_MAPPED_SUBRESOURCE ms;
        ZeroMemory(&ms, sizeof(D3D11_MAPPED_SUBRESOURCE));
        mContext->Map(mPerDrawCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        memcpy(ms.pData, &mPerDrawCbData, sizeof(PerDrawCB));
        mContext->Unmap(mPerDrawCb.Get(), 0);
    }

    // G_ZS_PRIM: upload prim_depth cbuffer when it changed
    if (mPrimDepthDirty) {
        mPerPrimDepthCbData.prim_depth = mCurrentPrimDepth;
        D3D11_MAPPED_SUBRESOURCE ms;
        ZeroMemory(&ms, sizeof(D3D11_MAPPED_SUBRESOURCE));
        mContext->Map(mPerPrimDepthCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        memcpy(ms.pData, &mPerPrimDepthCbData, sizeof(PerPrimDepthCB));
        mContext->Unmap(mPerPrimDepthCb.Get(), 0);
        mPrimDepthDirty = false;
    }

    // Set vertex buffer data

    D3D11_MAPPED_SUBRESOURCE ms;
    ZeroMemory(&ms, sizeof(D3D11_MAPPED_SUBRESOURCE));
    mContext->Map(mVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, buf_vbo, buf_vbo_len * sizeof(float));
    mContext->Unmap(mVertexBuffer.Get(), 0);

    uint32_t stride = mShaderProgram->numFloats * sizeof(float);
    uint32_t offset = 0;

    if (mLastVertexBufferStride != stride) {
        mLastVertexBufferStride = stride;
        mContext->IASetVertexBuffers(0, 1, mVertexBuffer.GetAddressOf(), &stride, &offset);
    }

    if (mLastShaderProgram != mShaderProgram) {
        mLastShaderProgram = mShaderProgram;
        mContext->IASetInputLayout(mShaderProgram->input_layout.Get());
        mContext->VSSetShader(mShaderProgram->vertex_shader.Get(), 0, 0);
        mContext->PSSetShader(mShaderProgram->pixel_shader.Get(), 0, 0);

        if (mLastBlendState.Get() != mShaderProgram->blend_state.Get()) {
            mLastBlendState = mShaderProgram->blend_state.Get();
            mContext->OMSetBlendState(mShaderProgram->blend_state.Get(), 0, 0xFFFFFFFF);
        }
    }

    if (mLastPrimitaveTopology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        mLastPrimitaveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        mContext->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    mContext->Draw(buf_vbo_num_tris * 3, 0);
}

void GfxRenderingAPIDX11::OnResize() {
    // create_render_target_views(true);
}

void GfxRenderingAPIDX11::StartFrame() {
    // Set per-frame constant buffer
    ID3D11Buffer* buffers[3] = { mPerFrameCb.Get(), mPerDrawCb.Get(), mPerPrimDepthCb.Get() };
    mContext->PSSetConstantBuffers(0, 3, buffers);

    mPerFrameCbData.noise_frame++;
    if (mPerFrameCbData.noise_frame > 150) {
        // No high values, as noise starts to look ugly
        mPerFrameCbData.noise_frame = 0;
    }
}

bool GfxRenderingAPIDX11::EnsureAmbientOcclusionShaders() {
    if (mAoVertexShader.Get() != nullptr && mAoPixelShader.Get() != nullptr && mAoBlurPixelShader.Get() != nullptr &&
        mAoCompositePixelShader.Get() != nullptr && mAoPixelShaderMsaa.Get() != nullptr) {
        return true;
    }

    UINT compile_flags = D3DCOMPILE_OPTIMIZATION_LEVEL2;
#if DEBUG_D3D
    compile_flags = D3DCOMPILE_DEBUG;
#endif

    auto compileShader = [&](const char* source, const char* entryPoint, const char* target,
                             ComPtr<ID3DBlob>& outBlob) -> bool {
        ComPtr<ID3DBlob> errors;
        HRESULT hr = mD3dCompile(source, strlen(source), nullptr, nullptr, nullptr, entryPoint, target, compile_flags, 0,
                                 outBlob.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr)) {
            const char* errorText = errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown";
            SPDLOG_ERROR("[FAST][AO][DX11] Shader compile failed {} {}: {}", entryPoint, target, errorText);
            mAmbientOcclusionFallbackCode = kAoFallbackShaderCompileFailed;
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> aoPsBlob;
    ComPtr<ID3DBlob> aoPsMsaaBlob;
    ComPtr<ID3DBlob> blurPsBlob;
    ComPtr<ID3DBlob> compositePsBlob;
    if (!compileShader(kAoFullscreenVs, "VSMain", "vs_4_0", vsBlob) ||
        !compileShader(kAoGeneratePs, "PSMain", "ps_4_0", aoPsBlob) ||
        !compileShader(kAoGeneratePsMsaa, "PSMain", "ps_4_0", aoPsMsaaBlob) ||
        !compileShader(kAoBlurPs, "PSMain", "ps_4_0", blurPsBlob) ||
        !compileShader(kAoCompositePs, "PSMain", "ps_4_0", compositePsBlob)) {
        return false;
    }

    auto createOrFail = [&](HRESULT hr, const char* label) -> bool {
        if (FAILED(hr)) {
            SPDLOG_ERROR("[FAST][AO][DX11] Resource creation failed: {} (hr=0x{:08X})", label,
                         static_cast<uint32_t>(hr));
            mAmbientOcclusionFallbackCode = kAoFallbackRuntimeUnsupported;
            return false;
        }
        return true;
    };

    if (!createOrFail(mDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                                  mAoVertexShader.ReleaseAndGetAddressOf()),
                      "CreateVertexShader") ||
        !createOrFail(mDevice->CreatePixelShader(aoPsBlob->GetBufferPointer(), aoPsBlob->GetBufferSize(), nullptr,
                                                 mAoPixelShader.ReleaseAndGetAddressOf()),
                      "CreatePixelShader(AO)") ||
        !createOrFail(mDevice->CreatePixelShader(aoPsMsaaBlob->GetBufferPointer(), aoPsMsaaBlob->GetBufferSize(), nullptr,
                                                 mAoPixelShaderMsaa.ReleaseAndGetAddressOf()),
                      "CreatePixelShader(AO MSAA)") ||
        !createOrFail(mDevice->CreatePixelShader(blurPsBlob->GetBufferPointer(), blurPsBlob->GetBufferSize(), nullptr,
                                                 mAoBlurPixelShader.ReleaseAndGetAddressOf()),
                      "CreatePixelShader(AO Blur)") ||
        !createOrFail(mDevice->CreatePixelShader(compositePsBlob->GetBufferPointer(), compositePsBlob->GetBufferSize(),
                                                 nullptr, mAoCompositePixelShader.ReleaseAndGetAddressOf()),
                      "CreatePixelShader(AO Composite)")) {
        return false;
    }

    D3D11_BUFFER_DESC ao_cb_desc;
    ZeroMemory(&ao_cb_desc, sizeof(ao_cb_desc));
    ao_cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    ao_cb_desc.ByteWidth = (sizeof(AoParams) + 15u) & ~15u;
    ao_cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ao_cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!createOrFail(mDevice->CreateBuffer(&ao_cb_desc, nullptr, mAoParamsCb.ReleaseAndGetAddressOf()),
                      "CreateBuffer(AO Params)")) {
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc;
    ZeroMemory(&sampler_desc, sizeof(sampler_desc));
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (!createOrFail(mDevice->CreateSamplerState(&sampler_desc, mAoSamplerState.ReleaseAndGetAddressOf()),
                      "CreateSamplerState(AO)")) {
        return false;
    }

    D3D11_BLEND_DESC blend_desc;
    ZeroMemory(&blend_desc, sizeof(blend_desc));
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (!createOrFail(mDevice->CreateBlendState(&blend_desc, mAoMultiplyBlendState.ReleaseAndGetAddressOf()),
                      "CreateBlendState(AO Multiply)")) {
        return false;
    }

    return true;
}

bool GfxRenderingAPIDX11::EnsureAmbientOcclusionResources(uint32_t width, uint32_t height) {
    if (!EnsureAmbientOcclusionShaders()) {
        return false;
    }
    if (mAoWidth == width && mAoHeight == height && mAoPrimary.texture.Get() != nullptr && mAoBlur.texture.Get() != nullptr) {
        return true;
    }

    auto createTarget = [&](AoRenderTargets& target) {
        D3D11_TEXTURE2D_DESC texture_desc;
        ZeroMemory(&texture_desc, sizeof(texture_desc));
        texture_desc.Width = width;
        texture_desc.Height = height;
        texture_desc.MipLevels = 1;
        texture_desc.ArraySize = 1;
        texture_desc.Format = DXGI_FORMAT_R8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ThrowIfFailed(mDevice->CreateTexture2D(&texture_desc, nullptr, target.texture.ReleaseAndGetAddressOf()));
        ThrowIfFailed(mDevice->CreateRenderTargetView(target.texture.Get(), nullptr, target.rtv.ReleaseAndGetAddressOf()));
        ThrowIfFailed(mDevice->CreateShaderResourceView(target.texture.Get(), nullptr, target.srv.ReleaseAndGetAddressOf()));
    };

    createTarget(mAoPrimary);
    createTarget(mAoBlur);
    mAoWidth = width;
    mAoHeight = height;
    return true;
}

void GfxRenderingAPIDX11::ApplyAmbientOcclusion() {
    if (!mAmbientOcclusionConfig.enabled || mAmbientOcclusionConfig.quality == 0) {
        return;
    }
    mAmbientOcclusionFallbackCode = kAoFallbackNone;
    if (mCurrentFramebuffer < 0 || mCurrentFramebuffer >= static_cast<int32_t>(mFrameBuffers.size())) {
        mAmbientOcclusionFallbackCode = kAoFallbackRuntimeUnsupported;
        return;
    }

    FramebufferDX11& fb = mFrameBuffers[mCurrentFramebuffer];
    TextureData& tex = mTextures[fb.texture_id];
    if (!fb.has_depth_buffer || fb.depth_stencil_srv.Get() == nullptr || fb.render_target_view.Get() == nullptr || tex.width == 0 ||
        tex.height == 0) {
        mAmbientOcclusionFallbackCode = kAoFallbackRuntimeUnsupported;
        return;
    }

    if (!EnsureAmbientOcclusionResources(tex.width, tex.height)) {
        return;
    }

    static const float qualityRadiusMul[3] = { 1.0f, 1.6f, 2.2f };
    static const uint32_t qualitySamples[3] = { 8, 12, 16 };
    uint8_t qualityIndex = std::clamp<uint8_t>(mAmbientOcclusionConfig.quality, 1, 3) - 1;
    mAoParams.invResolution[0] = 1.0f / static_cast<float>(tex.width);
    mAoParams.invResolution[1] = 1.0f / static_cast<float>(tex.height);
    mAoParams.radiusPixels = std::clamp(mAmbientOcclusionConfig.radius * qualityRadiusMul[qualityIndex] * 12.0f, 0.5f, 64.0f);
    mAoParams.intensity = mAmbientOcclusionConfig.intensity;
    mAoParams.bias = mAmbientOcclusionConfig.bias;
    mAoParams.power = mAmbientOcclusionConfig.power;
    mAoParams.maxDistance = mAmbientOcclusionConfig.maxDistance;
    mAoParams.sampleCount = qualitySamples[qualityIndex];
    mAoParams.debugView = mAmbientOcclusionConfig.debugView ? 1 : 0;

    D3D11_MAPPED_SUBRESOURCE mapped;
    ZeroMemory(&mapped, sizeof(mapped));
    mContext->Map(mAoParamsCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    memcpy(mapped.pData, &mAoParams, sizeof(mAoParams));
    mContext->Unmap(mAoParamsCb.Get(), 0);

    const float clearAo[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    mContext->ClearRenderTargetView(mAoPrimary.rtv.Get(), clearAo);

    ID3D11RenderTargetView* aoRt = mAoPrimary.rtv.Get();
    mContext->OMSetRenderTargets(1, &aoRt, nullptr);
    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(tex.width);
    viewport.Height = static_cast<float>(tex.height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    mContext->RSSetViewports(1, &viewport);
    mContext->IASetInputLayout(nullptr);
    mContext->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mContext->VSSetShader(mAoVertexShader.Get(), nullptr, 0);
    mContext->PSSetShader(fb.msaa_level > 1 ? mAoPixelShaderMsaa.Get() : mAoPixelShader.Get(), nullptr, 0);
    ID3D11Buffer* aoCb = mAoParamsCb.Get();
    mContext->PSSetConstantBuffers(0, 1, &aoCb);
    ID3D11ShaderResourceView* depthSrv = fb.depth_stencil_srv.Get();
    mContext->PSSetShaderResources(0, 1, &depthSrv);
    ID3D11SamplerState* sampler = mAoSamplerState.Get();
    mContext->PSSetSamplers(0, 1, &sampler);
    mContext->Draw(3, 0);

    const int32_t blurPasses = std::clamp(mAmbientOcclusionConfig.blurPasses, 0, 4);
    for (int32_t pass = 0; pass < blurPasses; ++pass) {
        mAoParams.blurDirection[0] = 1.0f;
        mAoParams.blurDirection[1] = 0.0f;
        mContext->Map(mAoParamsCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        memcpy(mapped.pData, &mAoParams, sizeof(mAoParams));
        mContext->Unmap(mAoParamsCb.Get(), 0);

        ID3D11RenderTargetView* blurRt = mAoBlur.rtv.Get();
        mContext->OMSetRenderTargets(1, &blurRt, nullptr);
        mContext->PSSetShader(mAoBlurPixelShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* aoSrv = mAoPrimary.srv.Get();
        mContext->PSSetShaderResources(0, 1, &aoSrv);
        mContext->Draw(3, 0);

        mAoParams.blurDirection[0] = 0.0f;
        mAoParams.blurDirection[1] = 1.0f;
        mContext->Map(mAoParamsCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        memcpy(mapped.pData, &mAoParams, sizeof(mAoParams));
        mContext->Unmap(mAoParamsCb.Get(), 0);

        ID3D11RenderTargetView* aoRtVertical = mAoPrimary.rtv.Get();
        mContext->OMSetRenderTargets(1, &aoRtVertical, nullptr);
        mContext->PSSetShader(mAoBlurPixelShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* blurSrv = mAoBlur.srv.Get();
        mContext->PSSetShaderResources(0, 1, &blurSrv);
        mContext->Draw(3, 0);
    }

    ID3D11RenderTargetView* finalRt = fb.render_target_view.Get();
    mContext->OMSetRenderTargets(1, &finalRt, nullptr);
    mContext->VSSetShader(mAoVertexShader.Get(), nullptr, 0);
    mContext->PSSetShader(mAoCompositePixelShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* finalAoSrv = mAoPrimary.srv.Get();
    mContext->PSSetShaderResources(0, 1, &finalAoSrv);

    if (mAmbientOcclusionConfig.debugView) {
        mContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    } else {
        mContext->OMSetBlendState(mAoMultiplyBlendState.Get(), nullptr, 0xFFFFFFFF);
    }
    mContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    mContext->PSSetShaderResources(0, 1, nullSrv);
    mContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    ID3D11Buffer* defaultBuffers[2] = { mPerFrameCb.Get(), mPerDrawCb.Get() };
    mContext->PSSetConstantBuffers(0, 2, defaultBuffers);
}

bool GfxRenderingAPIDX11::IsAmbientOcclusionSupported() const {
    return true;
}

int32_t GfxRenderingAPIDX11::GetAmbientOcclusionFallbackCode() const {
    return mAmbientOcclusionFallbackCode;
}

void GfxRenderingAPIDX11::ClearAmbientOcclusionFallbackCode() {
    mAmbientOcclusionFallbackCode = kAoFallbackNone;
}

bool GfxRenderingAPIDX11::EnsureVolumetricsShaders() {
    if (mVolumetricsVertexShader.Get() != nullptr && mVolumetricsRaymarchPixelShader.Get() != nullptr &&
        mVolumetricsCompositePixelShader.Get() != nullptr && mVolumetricsParamsCb.Get() != nullptr &&
        mVolumetricsSamplerState.Get() != nullptr && mVolumetricsAdditiveBlendState.Get() != nullptr) {
        return true;
    }

#if DEBUG_D3D
    UINT compile_flags = D3DCOMPILE_DEBUG;
#else
    UINT compile_flags = D3DCOMPILE_OPTIMIZATION_LEVEL2;
#endif

    auto compileShader = [&](const char* source, const char* entryPoint, const char* target,
                             ComPtr<ID3DBlob>& outBlob) -> bool {
        ComPtr<ID3DBlob> errors;
        HRESULT hr = mD3dCompile(source, strlen(source), nullptr, nullptr, nullptr, entryPoint, target, compile_flags, 0,
                                 outBlob.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr)) {
            const char* errorText = errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown";
            SPDLOG_ERROR("[FAST][VOL][DX11] Shader compile failed {} {}: {}", entryPoint, target, errorText);
            mVolumetricsFallbackCode = kVolumetricsFallbackShaderCompileFailed;
            return false;
        }
        return true;
    };

    auto createOrFail = [&](HRESULT hr, const char* label) -> bool {
        if (FAILED(hr)) {
            SPDLOG_ERROR("[FAST][VOL][DX11] Resource creation failed: {} (hr=0x{:08X})", label,
                         static_cast<uint32_t>(hr));
            mVolumetricsFallbackCode = kVolumetricsFallbackRuntimeUnsupported;
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> raymarchBlob;
    ComPtr<ID3DBlob> compositeBlob;
    ComPtr<ID3DBlob> directionalShadowComputeBlob;
    if (!compileShader(kAoFullscreenVs, "VSMain", "vs_4_0", vsBlob) ||
        !compileShader(kVolumetricsRaymarchPs, "PSMain", "ps_4_0", raymarchBlob) ||
        !compileShader(kVolumetricsCompositePs, "PSMain", "ps_4_0", compositeBlob)) {
        return false;
    }
    if (mFeatureLevel >= D3D_FEATURE_LEVEL_11_0) {
        ComPtr<ID3DBlob> directionalErrors;
        HRESULT directionalHr =
            mD3dCompile(kVolumetricsDirectionalShadowCs, strlen(kVolumetricsDirectionalShadowCs), nullptr, nullptr, nullptr,
                        "CSMain", "cs_5_0", compile_flags, 0, directionalShadowComputeBlob.GetAddressOf(),
                        directionalErrors.GetAddressOf());
        if (FAILED(directionalHr)) {
            const char* errorText = directionalErrors != nullptr ? static_cast<const char*>(directionalErrors->GetBufferPointer())
                                                                 : "unknown";
            SPDLOG_WARN("[FAST][VOL][DX11] Directional shadow CS compile failed, continuing without light-space shadow proxy: {}",
                        errorText);
        }
    }

    if (!createOrFail(mDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                                  mVolumetricsVertexShader.ReleaseAndGetAddressOf()),
                      "CreateVertexShader(Volumetrics)") ||
        !createOrFail(mDevice->CreatePixelShader(raymarchBlob->GetBufferPointer(), raymarchBlob->GetBufferSize(),
                                                 nullptr, mVolumetricsRaymarchPixelShader.ReleaseAndGetAddressOf()),
                      "CreatePixelShader(Volumetrics Raymarch)") ||
        !createOrFail(mDevice->CreatePixelShader(compositeBlob->GetBufferPointer(), compositeBlob->GetBufferSize(),
                                                 nullptr, mVolumetricsCompositePixelShader.ReleaseAndGetAddressOf()),
                      "CreatePixelShader(Volumetrics Composite)")) {
        return false;
    }
    if (directionalShadowComputeBlob.Get() != nullptr &&
        !createOrFail(mDevice->CreateComputeShader(directionalShadowComputeBlob->GetBufferPointer(),
                                                   directionalShadowComputeBlob->GetBufferSize(), nullptr,
                                                   mVolumetricsDirectionalShadowComputeShader.ReleaseAndGetAddressOf()),
                      "CreateComputeShader(Volumetrics Directional Shadow)")) {
        mVolumetricsDirectionalShadowComputeShader.Reset();
    }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.ByteWidth = (sizeof(VolumetricsParams) + 15u) & ~15u;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!createOrFail(mDevice->CreateBuffer(&cbDesc, nullptr, mVolumetricsParamsCb.ReleaseAndGetAddressOf()),
                      "CreateBuffer(Volumetrics Params)")) {
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (!createOrFail(mDevice->CreateSamplerState(&samplerDesc, mVolumetricsSamplerState.ReleaseAndGetAddressOf()),
                      "CreateSamplerState(Volumetrics)")) {
        return false;
    }

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (!createOrFail(mDevice->CreateBlendState(&blendDesc, mVolumetricsAdditiveBlendState.ReleaseAndGetAddressOf()),
                      "CreateBlendState(Volumetrics Composite)")) {
        return false;
    }

    return true;
}

bool GfxRenderingAPIDX11::EnsureVolumetricsResources(uint32_t fullWidth, uint32_t fullHeight, uint32_t lowWidth,
                                                     uint32_t lowHeight) {
    if (!EnsureVolumetricsShaders()) {
        return false;
    }
    if (mVolumetricsFullWidth == fullWidth && mVolumetricsFullHeight == fullHeight && mVolumetricsLowWidth == lowWidth &&
        mVolumetricsLowHeight == lowHeight && mVolumetricsTargets[0].volumeTexture.Get() != nullptr &&
        mVolumetricsTargets[1].volumeTexture.Get() != nullptr && mVolumetricsTargets[0].depthTexture.Get() != nullptr &&
        mVolumetricsTargets[1].depthTexture.Get() != nullptr && mVolumetricsTargets[0].metricsTexture.Get() != nullptr &&
        mVolumetricsTargets[1].metricsTexture.Get() != nullptr && mVolumetricsMetricsReadback.Get() != nullptr) {
        return true;
    }

    auto createOrFail = [&](HRESULT hr, const char* label) -> bool {
        if (FAILED(hr)) {
            SPDLOG_ERROR("[FAST][VOL][DX11] Resource creation failed: {} (hr=0x{:08X})", label,
                         static_cast<uint32_t>(hr));
            mVolumetricsFallbackCode = kVolumetricsFallbackRuntimeUnsupported;
            return false;
        }
        return true;
    };

    auto createTarget = [&](VolumetricsRenderTargets& target) -> bool {
        D3D11_TEXTURE2D_DESC textureDesc = {};
        textureDesc.Width = lowWidth;
        textureDesc.Height = lowHeight;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        textureDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (!createOrFail(mDevice->CreateTexture2D(&textureDesc, nullptr, target.volumeTexture.ReleaseAndGetAddressOf()),
                          "CreateTexture2D(Volumetrics Volume)") ||
            !createOrFail(mDevice->CreateRenderTargetView(target.volumeTexture.Get(), nullptr,
                                                          target.volumeRtv.ReleaseAndGetAddressOf()),
                          "CreateRenderTargetView(Volumetrics Volume)") ||
            !createOrFail(mDevice->CreateShaderResourceView(target.volumeTexture.Get(), nullptr,
                                                            target.volumeSrv.ReleaseAndGetAddressOf()),
                          "CreateShaderResourceView(Volumetrics Volume)")) {
            return false;
        }

        textureDesc.Format = DXGI_FORMAT_R16_FLOAT;
        if (!createOrFail(mDevice->CreateTexture2D(&textureDesc, nullptr, target.depthTexture.ReleaseAndGetAddressOf()),
                          "CreateTexture2D(Volumetrics LowDepth)") ||
            !createOrFail(mDevice->CreateRenderTargetView(target.depthTexture.Get(), nullptr,
                                                          target.depthRtv.ReleaseAndGetAddressOf()),
                          "CreateRenderTargetView(Volumetrics LowDepth)") ||
            !createOrFail(mDevice->CreateShaderResourceView(target.depthTexture.Get(), nullptr,
                                                            target.depthSrv.ReleaseAndGetAddressOf()),
                          "CreateShaderResourceView(Volumetrics LowDepth)")) {
            return false;
        }

        textureDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (!createOrFail(mDevice->CreateTexture2D(&textureDesc, nullptr, target.metricsTexture.ReleaseAndGetAddressOf()),
                          "CreateTexture2D(Volumetrics Metrics)") ||
            !createOrFail(mDevice->CreateRenderTargetView(target.metricsTexture.Get(), nullptr,
                                                          target.metricsRtv.ReleaseAndGetAddressOf()),
                          "CreateRenderTargetView(Volumetrics Metrics)") ||
            !createOrFail(mDevice->CreateShaderResourceView(target.metricsTexture.Get(), nullptr,
                                                            target.metricsSrv.ReleaseAndGetAddressOf()),
                          "CreateShaderResourceView(Volumetrics Metrics)")) {
            return false;
        }

        return true;
    };

    if (!createTarget(mVolumetricsTargets[0]) || !createTarget(mVolumetricsTargets[1])) {
        return false;
    }

    D3D11_TEXTURE2D_DESC readbackDesc = {};
    readbackDesc.Width = lowWidth;
    readbackDesc.Height = lowHeight;
    readbackDesc.MipLevels = 1;
    readbackDesc.ArraySize = 1;
    readbackDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Usage = D3D11_USAGE_STAGING;
    readbackDesc.BindFlags = 0;
    readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    readbackDesc.MiscFlags = 0;
    if (!createOrFail(mDevice->CreateTexture2D(&readbackDesc, nullptr, mVolumetricsMetricsReadback.ReleaseAndGetAddressOf()),
                      "CreateTexture2D(Volumetrics Metrics Readback)")) {
        return false;
    }

    mVolumetricsFullWidth = fullWidth;
    mVolumetricsFullHeight = fullHeight;
    mVolumetricsLowWidth = lowWidth;
    mVolumetricsLowHeight = lowHeight;
    mVolumetricsHistoryIndex = 0;
    mVolumetricsStatsReadbackFrame = 0;
    mVolumetricsHistoryValid = false;
    mVolumetricsMetricsReadbackPending = false;
    return true;
}

void GfxRenderingAPIDX11::ApplyVolumetrics() {
    const GfxVolumetricsRuntimeStats previousStats = mVolumetricsRuntimeStats;
    mVolumetricsRuntimeStats = {};
    if (!mVolumetricsConfig.enabled || mVolumetricsConfig.quality == 0) {
        return;
    }

    mVolumetricsFallbackCode = kVolumetricsFallbackNone;
    if (mCurrentFramebuffer < 0 || mCurrentFramebuffer >= static_cast<int32_t>(mFrameBuffers.size())) {
        mVolumetricsFallbackCode = kVolumetricsFallbackRuntimeUnsupported;
        return;
    }

    FramebufferDX11& fb = mFrameBuffers[mCurrentFramebuffer];
    TextureData& tex = mTextures[fb.texture_id];
    if (fb.msaa_level > 1 || !fb.has_depth_buffer || fb.depth_stencil_srv.Get() == nullptr ||
        fb.render_target_view.Get() == nullptr || tex.width == 0 || tex.height == 0) {
        mVolumetricsFallbackCode = kVolumetricsFallbackRuntimeUnsupported;
        return;
    }

    const bool debugPreviewRequested = mVolumetricsConfig.debugMode != GFX_VOLUMETRICS_DEBUG_OFF;
    const uint8_t performanceMode = static_cast<uint8_t>(
        std::clamp(static_cast<int32_t>(mVolumetricsConfig.performanceMode),
                   static_cast<int32_t>(GFX_VOLUMETRICS_PERF_PERFORMANCE),
                   static_cast<int32_t>(GFX_VOLUMETRICS_PERF_QUALITY)));

    bool hasActiveLightPayload = false;
    uint32_t payloadLightCount = 0;
    for (size_t i = 0; i < mVolumetricLightCount; ++i) {
        const auto& light = mVolumetricLights[i];
        const float colorPeak = std::max(light.color[0], std::max(light.color[1], light.color[2]));
        const float effectiveIntensity = light.intensity * light.volumetricIntensity * colorPeak;
        if (effectiveIntensity > 0.0005f) {
            hasActiveLightPayload = true;
            payloadLightCount++;
        }
    }

    const bool raymarchActive =
        debugPreviewRequested || hasActiveLightPayload || performanceMode == GFX_VOLUMETRICS_PERF_QUALITY;
    const bool depthFogPathActive = true;
    const uint32_t lowPixelBudget =
        debugPreviewRequested ? (performanceMode == GFX_VOLUMETRICS_PERF_QUALITY ? 80000u : 60000u)
                              : (performanceMode == GFX_VOLUMETRICS_PERF_QUALITY
                                     ? 180000u
                                     : (performanceMode == GFX_VOLUMETRICS_PERF_BALANCED ? 120000u : 70000u));
    uint32_t lowWidth = 1u;
    uint32_t lowHeight = 1u;
    if (raymarchActive) {
        uint32_t lowDivisor = performanceMode == GFX_VOLUMETRICS_PERF_QUALITY ? 4u : 5u;
        while (lowDivisor < 32u) {
            const uint32_t candidateWidth = std::max<uint32_t>(1, tex.width / lowDivisor);
            const uint32_t candidateHeight = std::max<uint32_t>(1, tex.height / lowDivisor);
            if (candidateWidth * candidateHeight <= lowPixelBudget) {
                break;
            }
            lowDivisor++;
        }
        lowWidth = std::max<uint32_t>(1, tex.width / lowDivisor);
        lowHeight = std::max<uint32_t>(1, tex.height / lowDivisor);
    }
    if (!EnsureVolumetricsResources(tex.width, tex.height, lowWidth, lowHeight)) {
        return;
    }

    if (mVolumetricsConfig.resetHistory) {
        mVolumetricsHistoryValid = false;
    }

    const uint32_t writeIndex = mVolumetricsHistoryIndex;
    const uint32_t readIndex = 1u - writeIndex;
    uint32_t stepCount = 0u;
    if (raymarchActive) {
        if (debugPreviewRequested) {
            stepCount = performanceMode == GFX_VOLUMETRICS_PERF_QUALITY ? 18u : 12u;
        } else if (performanceMode == GFX_VOLUMETRICS_PERF_QUALITY) {
            stepCount = 28u;
        } else if (performanceMode == GFX_VOLUMETRICS_PERF_BALANCED) {
            stepCount = 18u;
        } else {
            stepCount = 12u;
        }
        if (mVolumetricsConfig.quality <= 1) {
            stepCount = std::min(stepCount, debugPreviewRequested ? 10u : 12u);
        }
    }
    const bool previousStatsMatch =
        previousStats.dispatched && previousStats.lowWidth == lowWidth && previousStats.lowHeight == lowHeight;
    const float previousMaxDensity = std::max(previousStatsMatch ? previousStats.maxDensity : 0.0f, 0.001f);
    const float previousMaxLightEnergy = std::max(previousStatsMatch ? previousStats.maxLightEnergy : 0.0f, 0.01f);
    const float previousMaxFinalVolume = std::max(previousStatsMatch ? previousStats.maxFinalVolume : 0.0f, 0.01f);

    ZeroMemory(&mVolumetricsParams, sizeof(mVolumetricsParams));
    mVolumetricsParams.invFullResolution[0] = 1.0f / static_cast<float>(tex.width);
    mVolumetricsParams.invFullResolution[1] = 1.0f / static_cast<float>(tex.height);
    mVolumetricsParams.invLowResolution[0] = 1.0f / static_cast<float>(lowWidth);
    mVolumetricsParams.invLowResolution[1] = 1.0f / static_cast<float>(lowHeight);
    memcpy(mVolumetricsParams.cameraWorldPosition, mVolumetricsConfig.cameraWorldPosition,
           sizeof(mVolumetricsParams.cameraWorldPosition));
    mVolumetricsParams.density = mVolumetricsConfig.density;
    memcpy(mVolumetricsParams.fogColor, mVolumetricsConfig.color, sizeof(mVolumetricsParams.fogColor));
    mVolumetricsParams.ambientIntensity = mVolumetricsConfig.ambientIntensity;
    mVolumetricsParams.anisotropy = mVolumetricsConfig.anisotropy;
    mVolumetricsParams.startDistance = mVolumetricsConfig.startDistance;
    mVolumetricsParams.maxDistance = mVolumetricsConfig.maxDistance;
    mVolumetricsParams.baseHeight = mVolumetricsConfig.baseHeight;
    mVolumetricsParams.heightFalloff = mVolumetricsConfig.heightFalloff;
    mVolumetricsParams.lightShaftIntensity = mVolumetricsConfig.lightShaftIntensity;
    mVolumetricsParams.shadowIntensity = mVolumetricsConfig.shadowIntensity;
    mVolumetricsParams.temporalBlend = mVolumetricsConfig.temporalBlend;
    mVolumetricsParams.jitterScale = mVolumetricsConfig.jitterScale;
    mVolumetricsParams.nearPlane = mVolumetricsConfig.nearPlane;
    mVolumetricsParams.farPlane = mVolumetricsConfig.farPlane;
    mVolumetricsParams.depthNdcParams[0] = 2.0f;
    mVolumetricsParams.depthNdcParams[1] = -1.0f;
    mVolumetricsParams.ndcDepthParams[0] = 0.5f;
    mVolumetricsParams.ndcDepthParams[1] = 0.5f;
    mVolumetricsParams.debugDensityScale = 1.0f / previousMaxDensity;
    mVolumetricsParams.debugLightScale = 1.0f / previousMaxLightEnergy;
    mVolumetricsParams.debugFinalScale = 1.0f / previousMaxFinalVolume;
    mVolumetricsParams.debugPreviewBlend = 1.0f;
    mVolumetricsParams.debugDensityBias = 0.0f;
    mVolumetricsParams.debugLightBias = 0.0f;
    mVolumetricsParams.debugFinalBias = 0.0f;
    mVolumetricsParams.debugMediumBias = 0.0f;
    mVolumetricsParams.aerialPerspectiveStrength = mVolumetricsConfig.aerialPerspectiveStrength;
    mVolumetricsParams.macroNoiseStrength = mVolumetricsConfig.macroNoiseStrength;
    mVolumetricsParams.valleyFogStrength = mVolumetricsConfig.valleyFogStrength;
    mVolumetricsParams.edgeHazeStrength = mVolumetricsConfig.edgeHazeStrength;
    mVolumetricsParams.depthExtinctionStrength = mVolumetricsConfig.depthExtinctionStrength;
    mVolumetricsParams.depthExtinctionExponent = mVolumetricsConfig.depthExtinctionExponent;
    mVolumetricsParams.horizonFogStrength = mVolumetricsConfig.horizonFogStrength;
    mVolumetricsParams.skyFallbackStrength = mVolumetricsConfig.skyFallbackStrength;
    mVolumetricsParams.directionalShadowBias = mVolumetricsConfig.directionalShadowBias;
    mVolumetricsParams.directionalShadowNormalBias = mVolumetricsConfig.directionalShadowNormalBias;
    mVolumetricsParams.directionalShadowSoftness = mVolumetricsConfig.directionalShadowSoftness;
    mVolumetricsParams.stepCount = stepCount;
    mVolumetricsParams.lightCount = static_cast<uint32_t>(std::min<size_t>(mVolumetricLightCount, GFX_MAX_VOLUMETRIC_LIGHTS));
    const bool screenDepthShadowProxyAllowed =
        raymarchActive && hasActiveLightPayload && performanceMode == GFX_VOLUMETRICS_PERF_QUALITY &&
        mVolumetricsConfig.shadowQuality > 0 && !debugPreviewRequested;
    mVolumetricsParams.shadowQuality = screenDepthShadowProxyAllowed ? mVolumetricsConfig.shadowQuality : 0u;
    mVolumetricsParams.historyValid = mVolumetricsHistoryValid ? 1u : 0u;
    mVolumetricsParams.heightFogEnabled = mVolumetricsConfig.heightFogEnabled ? 1u : 0u;
    mVolumetricsParams.debugMode = mVolumetricsConfig.debugMode;
    mVolumetricsParams.resetHistory = mVolumetricsConfig.resetHistory ? 1u : 0u;
    mVolumetricsParams.performanceMode = performanceMode;
    mVolumetricsParams.raymarchEnabled = raymarchActive ? 1u : 0u;
    mVolumetricsParams.depthFogPathEnabled = depthFogPathActive ? 1u : 0u;
    mVolumetricsParams.shadowMode = screenDepthShadowProxyAllowed ? GFX_VOLUMETRICS_SHADOW_SCREEN_DEPTH_PROXY
                                                                  : GFX_VOLUMETRICS_SHADOW_OFF;
    memcpy(mVolumetricsParams.currViewProj, mVolumetricsConfig.currentViewProjection, sizeof(mVolumetricsParams.currViewProj));
    memcpy(mVolumetricsParams.invViewProj, mVolumetricsConfig.inverseViewProjection, sizeof(mVolumetricsParams.invViewProj));
    memcpy(mVolumetricsParams.prevViewProj, mVolumetricsConfig.previousViewProjection, sizeof(mVolumetricsParams.prevViewProj));
    for (size_t i = 0; i < mVolumetricLightCount && i < GFX_MAX_VOLUMETRIC_LIGHTS; ++i) {
        const auto& light = mVolumetricLights[i];
        mVolumetricsParams.lightPositionRadius[i][0] = light.position[0];
        mVolumetricsParams.lightPositionRadius[i][1] = light.position[1];
        mVolumetricsParams.lightPositionRadius[i][2] = light.position[2];
        mVolumetricsParams.lightPositionRadius[i][3] = light.radius;
        mVolumetricsParams.lightDirectionIntensity[i][0] = light.direction[0];
        mVolumetricsParams.lightDirectionIntensity[i][1] = light.direction[1];
        mVolumetricsParams.lightDirectionIntensity[i][2] = light.direction[2];
        mVolumetricsParams.lightDirectionIntensity[i][3] = light.intensity;
        mVolumetricsParams.lightColorVolumetric[i][0] = light.color[0];
        mVolumetricsParams.lightColorVolumetric[i][1] = light.color[1];
        mVolumetricsParams.lightColorVolumetric[i][2] = light.color[2];
        mVolumetricsParams.lightColorVolumetric[i][3] = light.volumetricIntensity;
        mVolumetricsParams.lightConeShadow[i][0] = light.innerConeCos;
        mVolumetricsParams.lightConeShadow[i][1] = light.outerConeCos;
        mVolumetricsParams.lightConeShadow[i][2] = light.shadowBias;
        mVolumetricsParams.lightConeShadow[i][3] = light.shadowRange;
        mVolumetricsParams.lightShadowParams[i][0] = light.shadowNormalBias;
        mVolumetricsParams.lightShadowParams[i][1] = light.shadowResolution;
        mVolumetricsParams.lightShadowParams[i][2] = static_cast<float>(light.type);
        mVolumetricsParams.lightShadowParams[i][3] = light.castShadows ? 1.0f : 0.0f;
    }

    auto ensureDirectionalShadowResources = [&](uint32_t shadowMapSize) -> bool {
        if (shadowMapSize == 0 || mVolumetricsDirectionalShadowComputeShader.Get() == nullptr) {
            return false;
        }
        if (mVolumetricsDirectionalShadowTexture.Get() != nullptr && mVolumetricsDirectionalShadowSrv.Get() != nullptr &&
            mVolumetricsDirectionalShadowUav.Get() != nullptr && mVolumetricsDirectionalShadowSize == shadowMapSize) {
            return true;
        }

        mVolumetricsDirectionalShadowTexture.Reset();
        mVolumetricsDirectionalShadowSrv.Reset();
        mVolumetricsDirectionalShadowUav.Reset();

        D3D11_TEXTURE2D_DESC textureDesc = {};
        textureDesc.Width = shadowMapSize;
        textureDesc.Height = shadowMapSize;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R32_UINT;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.SampleDesc.Quality = 0;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        textureDesc.CPUAccessFlags = 0;
        textureDesc.MiscFlags = 0;

        auto createOrFail = [&](HRESULT hr, const char* label) -> bool {
            if (FAILED(hr)) {
                SPDLOG_ERROR("[FAST][VOL][DX11] Resource creation failed: {} (hr=0x{:08X})", label,
                             static_cast<uint32_t>(hr));
                mVolumetricsFallbackCode = kVolumetricsFallbackRuntimeUnsupported;
                return false;
            }
            return true;
        };

        if (!createOrFail(mDevice->CreateTexture2D(&textureDesc, nullptr, mVolumetricsDirectionalShadowTexture.ReleaseAndGetAddressOf()),
                          "CreateTexture2D(Volumetrics Directional Shadow)") ||
            !createOrFail(mDevice->CreateShaderResourceView(mVolumetricsDirectionalShadowTexture.Get(), nullptr,
                                                            mVolumetricsDirectionalShadowSrv.ReleaseAndGetAddressOf()),
                          "CreateShaderResourceView(Volumetrics Directional Shadow)") ||
            !createOrFail(mDevice->CreateUnorderedAccessView(mVolumetricsDirectionalShadowTexture.Get(), nullptr,
                                                             mVolumetricsDirectionalShadowUav.ReleaseAndGetAddressOf()),
                          "CreateUnorderedAccessView(Volumetrics Directional Shadow)")) {
            return false;
        }

        mVolumetricsDirectionalShadowSize = shadowMapSize;
        return true;
    };

    int32_t directionalShadowLightIndex = -1;
    float directionalShadowScore = 0.0f;
    for (size_t i = 0; i < mVolumetricLightCount && i < GFX_MAX_VOLUMETRIC_LIGHTS; ++i) {
        const auto& light = mVolumetricLights[i];
        if (light.type != GFX_VOLUMETRIC_LIGHT_DIRECTIONAL || light.castShadows == 0) {
            continue;
        }
        const float colorPeak = std::max(light.color[0], std::max(light.color[1], light.color[2]));
        const float effectiveIntensity = light.intensity * light.volumetricIntensity * colorPeak;
        if (effectiveIntensity > directionalShadowScore) {
            directionalShadowScore = effectiveIntensity;
            directionalShadowLightIndex = static_cast<int32_t>(i);
        }
    }

    if (mFeatureLevel >= D3D_FEATURE_LEVEL_11_0 && mVolumetricsDirectionalShadowComputeShader.Get() != nullptr &&
        directionalShadowLightIndex >= 0 && mVolumetricsParams.shadowQuality > 0) {
        const auto& shadowLight = mVolumetricLights[static_cast<size_t>(directionalShadowLightIndex)];
        const uint32_t baseShadowMapSize =
            mVolumetricsParams.shadowQuality >= 3 ? 2048u : (mVolumetricsParams.shadowQuality == 2 ? 1024u : 512u);
        const uint32_t requestedShadowMapSize =
            static_cast<uint32_t>(std::clamp(shadowLight.shadowResolution, 256.0f, 4096.0f));
        const uint32_t shadowMapSize =
            std::clamp(std::max(baseShadowMapSize, requestedShadowMapSize), 256u, 2048u);
        float shadowViewProjection[16] = {};
        if (BuildDirectionalShadowViewProjection(mVolumetricsConfig, shadowLight, shadowViewProjection) &&
            ensureDirectionalShadowResources(shadowMapSize)) {
            mVolumetricsParams.directionalShadowEnabled = 1u;
            mVolumetricsParams.directionalShadowLightIndex = static_cast<uint32_t>(directionalShadowLightIndex);
            mVolumetricsParams.directionalShadowMapSize = shadowMapSize;
            memcpy(mVolumetricsParams.directionalShadowViewProj, shadowViewProjection,
                   sizeof(mVolumetricsParams.directionalShadowViewProj));
        }
    }

    auto uploadVolumetricsParams = [&]() {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        mContext->Map(mVolumetricsParamsCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        memcpy(mapped.pData, &mVolumetricsParams, sizeof(mVolumetricsParams));
        mContext->Unmap(mVolumetricsParamsCb.Get(), 0);
    };
    uploadVolumetricsParams();

    if (mVolumetricsParams.directionalShadowEnabled != 0 && mVolumetricsDirectionalShadowUav.Get() != nullptr &&
        mVolumetricsDirectionalShadowSrv.Get() != nullptr && mVolumetricsDirectionalShadowComputeShader.Get() != nullptr) {
        const UINT clearValues[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
        mContext->ClearUnorderedAccessViewUint(mVolumetricsDirectionalShadowUav.Get(), clearValues);
        ID3D11Buffer* csCb = mVolumetricsParamsCb.Get();
        ID3D11ShaderResourceView* csSrvs[1] = { fb.depth_stencil_srv.Get() };
        ID3D11UnorderedAccessView* csUavs[1] = { mVolumetricsDirectionalShadowUav.Get() };
        mContext->CSSetShader(mVolumetricsDirectionalShadowComputeShader.Get(), nullptr, 0);
        mContext->CSSetConstantBuffers(0, 1, &csCb);
        mContext->CSSetShaderResources(0, 1, csSrvs);
        mContext->CSSetUnorderedAccessViews(0, 1, csUavs, nullptr);
        mContext->Dispatch((tex.width + 7u) / 8u, (tex.height + 7u) / 8u, 1u);
        ID3D11ShaderResourceView* nullCsSrvs[1] = { nullptr };
        ID3D11UnorderedAccessView* nullCsUavs[1] = { nullptr };
        mContext->CSSetShaderResources(0, 1, nullCsSrvs);
        mContext->CSSetUnorderedAccessViews(0, 1, nullCsUavs, nullptr);
        mContext->CSSetShader(nullptr, nullptr, 0);
    }

    const float clearVolume[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const float clearLowDepth[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    const float clearMetrics[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    mContext->ClearRenderTargetView(mVolumetricsTargets[writeIndex].volumeRtv.Get(), clearVolume);
    mContext->ClearRenderTargetView(mVolumetricsTargets[writeIndex].depthRtv.Get(), clearLowDepth);
    mContext->ClearRenderTargetView(mVolumetricsTargets[writeIndex].metricsRtv.Get(), clearMetrics);

    D3D11_VIEWPORT lowViewport = {};
    lowViewport.TopLeftX = 0.0f;
    lowViewport.TopLeftY = 0.0f;
    lowViewport.Width = static_cast<float>(lowWidth);
    lowViewport.Height = static_cast<float>(lowHeight);
    lowViewport.MinDepth = 0.0f;
    lowViewport.MaxDepth = 1.0f;

    ID3D11RenderTargetView* lowRts[3] = { mVolumetricsTargets[writeIndex].volumeRtv.Get(),
                                          mVolumetricsTargets[writeIndex].depthRtv.Get(),
                                          mVolumetricsTargets[writeIndex].metricsRtv.Get() };
    mContext->OMSetRenderTargets(3, lowRts, nullptr);
    mContext->RSSetViewports(1, &lowViewport);
    mContext->IASetInputLayout(nullptr);
    mContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* volCb = mVolumetricsParamsCb.Get();
    ID3D11SamplerState* volSampler = mVolumetricsSamplerState.Get();
    if (raymarchActive) {
        mContext->VSSetShader(mVolumetricsVertexShader.Get(), nullptr, 0);
        mContext->PSSetShader(mVolumetricsRaymarchPixelShader.Get(), nullptr, 0);
        mContext->PSSetConstantBuffers(0, 1, &volCb);
        ID3D11ShaderResourceView* raymarchSrvs[3] = { fb.depth_stencil_srv.Get(), mVolumetricsTargets[readIndex].volumeSrv.Get(),
                                                      mVolumetricsParams.directionalShadowEnabled != 0
                                                          ? mVolumetricsDirectionalShadowSrv.Get()
                                                          : nullptr };
        mContext->PSSetShaderResources(0, 3, raymarchSrvs);
        mContext->PSSetSamplers(0, 1, &volSampler);
        mContext->Draw(3, 0);
        ID3D11ShaderResourceView* nullRaymarchSrvs[3] = { nullptr, nullptr, nullptr };
        mContext->PSSetShaderResources(0, 3, nullRaymarchSrvs);
    }

    D3D11_VIEWPORT fullViewport = {};
    fullViewport.TopLeftX = 0.0f;
    fullViewport.TopLeftY = 0.0f;
    fullViewport.Width = static_cast<float>(tex.width);
    fullViewport.Height = static_cast<float>(tex.height);
    fullViewport.MinDepth = 0.0f;
    fullViewport.MaxDepth = 1.0f;

    ID3D11RenderTargetView* finalRt = fb.render_target_view.Get();
    mContext->OMSetRenderTargets(1, &finalRt, nullptr);
    mContext->RSSetViewports(1, &fullViewport);
    mContext->VSSetShader(mVolumetricsVertexShader.Get(), nullptr, 0);
    mContext->PSSetShader(mVolumetricsCompositePixelShader.Get(), nullptr, 0);
    bool statsReadValid = previousStatsMatch;
    float avgDensity = previousStatsMatch ? previousStats.avgDensity : 0.0f;
    float avgLightEnergy = previousStatsMatch ? previousStats.avgLightEnergy : 0.0f;
    float avgFinalVolume = previousStatsMatch ? previousStats.avgFinalVolume : 0.0f;
    float avgTransmittance = previousStatsMatch ? previousStats.avgTransmittance : 1.0f;
    float maxDensity = previousStatsMatch ? previousStats.maxDensity : 0.0f;
    float maxLightEnergy = previousStatsMatch ? previousStats.maxLightEnergy : 0.0f;
    float maxFinalVolume = previousStatsMatch ? previousStats.maxFinalVolume : 0.0f;
    float maxOpticalDepth = previousStatsMatch ? previousStats.maxOpticalDepth : 0.0f;
    uint32_t marchedPixelCount = previousStatsMatch ? previousStats.marchedPixelCount : 0u;
    bool freshStatsRead = false;

    auto consumeMetricsReadback = [&](const D3D11_MAPPED_SUBRESOURCE& metricsMapped) -> bool {
        constexpr uint32_t kSampleGridX = 16u;
        constexpr uint32_t kSampleGridY = 16u;
        double sumDensity = 0.0;
        double sumLightEnergy = 0.0;
        double sumFinalVolume = 0.0;
        double sumTransmittance = 0.0;
        float peakDensity = 0.0f;
        float peakLightEnergy = 0.0f;
        float peakFinalVolume = 0.0f;
        float peakOpticalDepth = 0.0f;
        uint32_t sampledPixels = 0;
        uint32_t marchedSamples = 0;
        for (uint32_t sampleY = 0; sampleY < kSampleGridY; ++sampleY) {
            const uint32_t pixelY =
                (kSampleGridY <= 1u) ? 0u : ((sampleY * (lowHeight - 1u)) / (kSampleGridY - 1u));
            const auto* row = reinterpret_cast<const float*>(
                reinterpret_cast<const uint8_t*>(metricsMapped.pData) + pixelY * metricsMapped.RowPitch);
            for (uint32_t sampleX = 0; sampleX < kSampleGridX; ++sampleX) {
                const uint32_t pixelX =
                    (kSampleGridX <= 1u) ? 0u : ((sampleX * (lowWidth - 1u)) / (kSampleGridX - 1u));
                const float* pixel = row + (pixelX * 4u);
                const float densityValue = std::max(pixel[0], 0.0f);
                const float lightValue = std::max(pixel[1], 0.0f);
                const float finalValue = std::max(pixel[2], 0.0f);
                const float opticalDepthValue = std::max(pixel[3], 0.0f);
                sumDensity += densityValue;
                sumLightEnergy += lightValue;
                sumFinalVolume += finalValue;
                sumTransmittance += exp(-opticalDepthValue);
                peakDensity = std::max(peakDensity, densityValue);
                peakLightEnergy = std::max(peakLightEnergy, lightValue);
                peakFinalVolume = std::max(peakFinalVolume, finalValue);
                peakOpticalDepth = std::max(peakOpticalDepth, opticalDepthValue);
                if (densityValue > 0.0001f || lightValue > 0.0001f || finalValue > 0.0001f ||
                    opticalDepthValue > 0.0001f) {
                    marchedSamples++;
                }
                sampledPixels++;
            }
        }
        if (sampledPixels == 0u) {
            return false;
        }

        const float invSampleCount = 1.0f / static_cast<float>(sampledPixels);
        avgDensity = static_cast<float>(sumDensity) * invSampleCount;
        avgLightEnergy = static_cast<float>(sumLightEnergy) * invSampleCount;
        avgFinalVolume = static_cast<float>(sumFinalVolume) * invSampleCount;
        avgTransmittance = static_cast<float>(sumTransmittance) * invSampleCount;
        maxDensity = peakDensity;
        maxLightEnergy = peakLightEnergy;
        maxFinalVolume = peakFinalVolume;
        maxOpticalDepth = peakOpticalDepth;
        marchedPixelCount = marchedSamples;
        return true;
    };

    if (mVolumetricsMetricsReadback.Get() != nullptr) {
        if (mVolumetricsMetricsReadbackPending) {
            D3D11_MAPPED_SUBRESOURCE metricsMapped = {};
            const HRESULT mapHr = mContext->Map(mVolumetricsMetricsReadback.Get(), 0, D3D11_MAP_READ,
                                                D3D11_MAP_FLAG_DO_NOT_WAIT, &metricsMapped);
            if (SUCCEEDED(mapHr)) {
                statsReadValid = consumeMetricsReadback(metricsMapped);
                freshStatsRead = statsReadValid;
                mContext->Unmap(mVolumetricsMetricsReadback.Get(), 0);
                mVolumetricsMetricsReadbackPending = false;
            } else if (mapHr != DXGI_ERROR_WAS_STILL_DRAWING) {
                mVolumetricsMetricsReadbackPending = false;
            }
        }

        const uint32_t statsReadbackInterval = debugPreviewRequested ? 6u : 16u;
        mVolumetricsStatsReadbackFrame++;
        if (!mVolumetricsMetricsReadbackPending &&
            (!statsReadValid || (mVolumetricsStatsReadbackFrame % statsReadbackInterval) == 0u)) {
            mContext->CopyResource(mVolumetricsMetricsReadback.Get(), mVolumetricsTargets[writeIndex].metricsTexture.Get());
            mVolumetricsMetricsReadbackPending = true;
        }
    }

    if (!raymarchActive) {
        const float distanceSpan = std::max(mVolumetricsConfig.maxDistance - mVolumetricsConfig.startDistance, 1.0f);
        const float opticalDepthEstimate =
            std::clamp(mVolumetricsConfig.density * distanceSpan * 0.01f *
                           std::max(mVolumetricsConfig.depthExtinctionStrength, 0.35f),
                       0.0f, 6.0f);
        avgDensity = opticalDepthEstimate * 0.45f;
        maxDensity = opticalDepthEstimate;
        avgLightEnergy = 0.0f;
        maxLightEnergy = 0.0f;
        avgFinalVolume = opticalDepthEstimate * std::max(mVolumetricsConfig.ambientIntensity, 0.08f) * 0.08f;
        maxFinalVolume = opticalDepthEstimate * std::max(mVolumetricsConfig.ambientIntensity, 0.08f) * 0.14f;
        maxOpticalDepth = opticalDepthEstimate;
        avgTransmittance = std::exp(-opticalDepthEstimate * 0.5f);
        marchedPixelCount = 0u;
        statsReadValid = true;
    }

    if (statsReadValid) {
        const float debugDensityBias = avgDensity * 0.92f;
        const float debugLightBias = avgLightEnergy * 0.88f;
        const float debugFinalBias = avgFinalVolume * 0.9f;
        mVolumetricsParams.debugDensityBias = debugDensityBias;
        mVolumetricsParams.debugLightBias = debugLightBias;
        mVolumetricsParams.debugFinalBias = debugFinalBias;
        mVolumetricsParams.debugMediumBias = avgDensity * 0.8f;
        mVolumetricsParams.debugDensityScale = 1.0f / std::max(maxDensity - debugDensityBias, 0.00025f);
        mVolumetricsParams.debugLightScale = 1.0f / std::max(maxLightEnergy - debugLightBias, 0.001f);
        mVolumetricsParams.debugFinalScale = 1.0f / std::max(maxFinalVolume - debugFinalBias, 0.001f);
        mVolumetricsParams.debugPreviewBlend = 1.75f;
        uploadVolumetricsParams();
    }

    ID3D11ShaderResourceView* compositeSrvs[5] = { mVolumetricsTargets[writeIndex].volumeSrv.Get(),
                                                   mVolumetricsTargets[writeIndex].depthSrv.Get(),
                                                   fb.depth_stencil_srv.Get(),
                                                   mVolumetricsTargets[writeIndex].metricsSrv.Get(),
                                                   mVolumetricsDirectionalShadowSrv.Get() };
    mContext->PSSetShaderResources(0, 5, compositeSrvs);
    if (mVolumetricsConfig.debugMode != GFX_VOLUMETRICS_DEBUG_OFF) {
        mContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    } else {
        mContext->OMSetBlendState(mVolumetricsAdditiveBlendState.Get(), nullptr, 0xFFFFFFFF);
    }
    mContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    mContext->PSSetShaderResources(0, 5, nullSrvs);
    mContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    ID3D11Buffer* defaultBuffers[2] = { mPerFrameCb.Get(), mPerDrawCb.Get() };
    mContext->PSSetConstantBuffers(0, 2, defaultBuffers);
    mVolumetricsHistoryValid = true;
    mVolumetricsHistoryIndex = readIndex;
    mVolumetricsFallbackCode = kVolumetricsFallbackNone;
    mVolumetricsRuntimeStats.dispatched = true;
    mVolumetricsRuntimeStats.executed = false;
    mVolumetricsRuntimeStats.producedMedium = false;
    mVolumetricsRuntimeStats.producedVolume = false;
    mVolumetricsRuntimeStats.producedFinalVolume = false;
    mVolumetricsRuntimeStats.debugPreviewValid = debugPreviewRequested && statsReadValid;
    mVolumetricsRuntimeStats.historyValid = mVolumetricsHistoryValid;
    mVolumetricsRuntimeStats.hasActiveLightPayload = hasActiveLightPayload;
    mVolumetricsRuntimeStats.depthFogPathActive = depthFogPathActive;
    mVolumetricsRuntimeStats.raymarchActive = raymarchActive;
    mVolumetricsRuntimeStats.performanceMode = performanceMode;
    mVolumetricsRuntimeStats.shadowMode = static_cast<uint8_t>(mVolumetricsParams.shadowMode);
    mVolumetricsRuntimeStats.payloadLightCount = payloadLightCount;
    mVolumetricsRuntimeStats.lowWidth = lowWidth;
    mVolumetricsRuntimeStats.lowHeight = lowHeight;
    mVolumetricsRuntimeStats.stepCount = stepCount;
    mVolumetricsRuntimeStats.raymarchPixelCount = raymarchActive ? lowWidth * lowHeight : 0u;
    mVolumetricsRuntimeStats.statsAgeFrames = freshStatsRead ? 0u : (previousStats.dispatched ? previousStats.statsAgeFrames + 1u : 0u);
    mVolumetricsRuntimeStats.avgDensity = avgDensity;
    mVolumetricsRuntimeStats.avgLightEnergy = avgLightEnergy;
    mVolumetricsRuntimeStats.avgFinalVolume = avgFinalVolume;
    mVolumetricsRuntimeStats.avgTransmittance = avgTransmittance;
    mVolumetricsRuntimeStats.maxDensity = maxDensity;
    mVolumetricsRuntimeStats.maxLightEnergy = maxLightEnergy;
    mVolumetricsRuntimeStats.maxFinalVolume = maxFinalVolume;
    mVolumetricsRuntimeStats.maxOpticalDepth = maxOpticalDepth;
    mVolumetricsRuntimeStats.marchedPixelCount = marchedPixelCount;
    mVolumetricsRuntimeStats.directionalShadowMapSize =
        mVolumetricsParams.directionalShadowEnabled != 0 ? mVolumetricsParams.directionalShadowMapSize : 0u;
    mVolumetricsRuntimeStats.directionalShadowCasterIndex =
        mVolumetricsParams.directionalShadowEnabled != 0 ? static_cast<int32_t>(mVolumetricsParams.directionalShadowLightIndex)
                                                         : -1;
    if (statsReadValid) {
        const bool producedMedium =
            (raymarchActive ? marchedPixelCount > 0u : depthFogPathActive) &&
            (maxDensity > 0.0015f || avgDensity > 0.00075f);
        const bool producedFinalVolume =
            (raymarchActive ? marchedPixelCount > 0u : depthFogPathActive) &&
            (maxLightEnergy > 0.01f || avgLightEnergy > 0.003f || maxFinalVolume > 0.01f || avgFinalVolume > 0.003f);
        mVolumetricsRuntimeStats.producedMedium = producedMedium;
        mVolumetricsRuntimeStats.producedVolume = producedFinalVolume;
        mVolumetricsRuntimeStats.producedFinalVolume = producedFinalVolume;
        mVolumetricsRuntimeStats.executed = producedFinalVolume;
    }
}

bool GfxRenderingAPIDX11::IsVolumetricsSupported() const {
    return true;
}

int32_t GfxRenderingAPIDX11::GetVolumetricsFallbackCode() const {
    return mVolumetricsFallbackCode;
}

void GfxRenderingAPIDX11::ClearVolumetricsFallbackCode() {
    mVolumetricsFallbackCode = kVolumetricsFallbackNone;
}

void GfxRenderingAPIDX11::EndFrame() {
    // Do not force a GPU flush every frame. Volumetrics uses async readback and low-res targets; flushing here turns
    // pending GPU work into a CPU stall and was a major cause of the 1 FPS debug/custom-volumetrics path.
#if DEBUG_D3D
    mContext->Flush();
#endif
}

void GfxRenderingAPIDX11::FinishRender() {
}

int GfxRenderingAPIDX11::CreateFramebuffer() {

    uint32_t texture_id = NewTexture();
    TextureData& t = mTextures[texture_id];

    size_t index = mFrameBuffers.size();
    mFrameBuffers.resize(mFrameBuffers.size() + 1);
    FramebufferDX11& data = mFrameBuffers.back();
    data.texture_id = texture_id;

    uint32_t tile = 0;
    uint32_t saved = mCurrentTextureIds[tile];
    mCurrentTextureIds[tile] = texture_id;
    SetSamplerParameters(0, true, G_TX_WRAP, G_TX_WRAP);
    mCurrentTextureIds[tile] = saved;

    return (int)index;
}

void GfxRenderingAPIDX11::UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                      bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                                      bool can_extract_depth) {
    FramebufferDX11& fb = mFrameBuffers[fb_id];
    TextureData& tex = mTextures[fb.texture_id];

    width = ((width) > (1U) ? (width) : (1U));
    height = ((height) > (1U) ? (height) : (1U));
    // We can't use MSAA the way we are using it on feature level 10_0 hardware, so disable it altogether.
    msaa_level = mFeatureLevel < D3D_FEATURE_LEVEL_10_1 ? 1 : msaa_level;
    while (msaa_level > 1 && mMsaaNumQualityLevels[msaa_level - 1] == 0) {
        --msaa_level;
    }

    bool diff = tex.width != width || tex.height != height || fb.msaa_level != msaa_level;

    if (diff || (fb.render_target_view.Get() != nullptr) != render_target) {
        if (fb_id != 0) {
            D3D11_TEXTURE2D_DESC texture_desc;
            texture_desc.Width = width;
            texture_desc.Height = height;
            texture_desc.Usage = D3D11_USAGE_DEFAULT;
            texture_desc.BindFlags =
                (msaa_level <= 1 ? D3D11_BIND_SHADER_RESOURCE : 0) | (render_target ? D3D11_BIND_RENDER_TARGET : 0);
            texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            texture_desc.CPUAccessFlags = 0;
            texture_desc.MiscFlags = 0;
            texture_desc.ArraySize = 1;
            texture_desc.MipLevels = 1;
            texture_desc.SampleDesc.Count = msaa_level;
            texture_desc.SampleDesc.Quality = 0;

            ThrowIfFailed(mDevice->CreateTexture2D(&texture_desc, nullptr, tex.texture.ReleaseAndGetAddressOf()));

            if (msaa_level <= 1) {
                ThrowIfFailed(mDevice->CreateShaderResourceView(tex.texture.Get(), nullptr,
                                                                tex.resource_view.ReleaseAndGetAddressOf()));
            }
        } else if (diff || (render_target && tex.texture.Get() == nullptr)) {
            DXGI_SWAP_CHAIN_DESC1 desc1;
            IDXGISwapChain1* swap_chain = mWindowBackend->GetSwapChain();
            ThrowIfFailed(swap_chain->GetDesc1(&desc1));
            if (desc1.Width != width || desc1.Height != height) {
                fb.render_target_view.Reset();
                tex.texture.Reset();
                ThrowIfFailed(swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, desc1.Flags));
            }
            ThrowIfFailed(
                swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)tex.texture.ReleaseAndGetAddressOf()));
        }
        if (render_target) {
            ThrowIfFailed(mDevice->CreateRenderTargetView(tex.texture.Get(), nullptr,
                                                          fb.render_target_view.ReleaseAndGetAddressOf()));
        }

        tex.width = width;
        tex.height = height;
    }

    if (has_depth_buffer &&
        (diff || !fb.has_depth_buffer || (fb.depth_stencil_srv.Get() != nullptr) != can_extract_depth)) {
        fb.depth_stencil_srv.Reset();
        CreateDepthStencilObjects(width, height, msaa_level, fb.depth_stencil_view.ReleaseAndGetAddressOf(),
                                  can_extract_depth ? fb.depth_stencil_srv.GetAddressOf() : nullptr);
    }
    if (!has_depth_buffer) {
        fb.depth_stencil_view.Reset();
        fb.depth_stencil_srv.Reset();
    }

    fb.has_depth_buffer = has_depth_buffer;
    fb.msaa_level = msaa_level;
}

void GfxRenderingAPIDX11::StartDrawToFramebuffer(int fb_id, float noise_scale) {
    FramebufferDX11& fb = mFrameBuffers[fb_id];
    mRenderTargetHeight = mTextures[fb.texture_id].height;

    mContext->OMSetRenderTargets(1, fb.render_target_view.GetAddressOf(),
                                 fb.has_depth_buffer ? fb.depth_stencil_view.Get() : nullptr);

    mCurrentFramebuffer = fb_id;

    if (noise_scale != 0.0f) {
        mPerFrameCbData.noise_scale = 1.0f / noise_scale;
    }

    D3D11_MAPPED_SUBRESOURCE ms;
    ZeroMemory(&ms, sizeof(D3D11_MAPPED_SUBRESOURCE));
    mContext->Map(mPerFrameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, &mPerFrameCbData, sizeof(PerFrameCB));
    mContext->Unmap(mPerFrameCb.Get(), 0);
}

void GfxRenderingAPIDX11::ClearFramebuffer(bool color, bool depth) {
    FramebufferDX11& fb = mFrameBuffers[mCurrentFramebuffer];
    if (color) {
        const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        mContext->ClearRenderTargetView(fb.render_target_view.Get(), clearColor);
    }
    if (depth && fb.has_depth_buffer) {
        mContext->ClearDepthStencilView(fb.depth_stencil_view.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    }
}

void GfxRenderingAPIDX11::ResolveMSAAColorBuffer(int fb_id_target, int fb_id_source) {
    FramebufferDX11& fb_dst = mFrameBuffers[fb_id_target];
    FramebufferDX11& fb_src = mFrameBuffers[fb_id_source];

    mContext->ResolveSubresource(mTextures[fb_dst.texture_id].texture.Get(), 0,
                                 mTextures[fb_src.texture_id].texture.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
}

void* GfxRenderingAPIDX11::GetFramebufferTextureId(int fb_id) {
    return (void*)mTextures[mFrameBuffers[fb_id].texture_id].resource_view.Get();
}

void GfxRenderingAPIDX11::SelectTextureFb(int fbID) {
    int tile = 0;
    SelectTexture(tile, mFrameBuffers[fbID].texture_id);
}

void GfxRenderingAPIDX11::CopyFramebuffer(int fb_dst_id, int fb_src_id, int srcX0, int srcY0, int srcX1, int srcY1,
                                          int dstX0, int dstY0, int dstX1, int dstY1) {
    if (fb_src_id >= (int)mFrameBuffers.size() || fb_dst_id >= (int)mFrameBuffers.size()) {
        return;
    }

    FramebufferDX11& fb_dst = mFrameBuffers[fb_dst_id];
    FramebufferDX11& fb_src = mFrameBuffers[fb_src_id];

    TextureData& td_dst = mTextures[fb_dst.texture_id];
    TextureData& td_src = mTextures[fb_src.texture_id];

    // Textures are the same size so we can do a direct copy or resolve
    if (td_src.height == td_dst.height && td_src.width == td_dst.width) {
        if (fb_src.msaa_level <= 1) {
            mContext->CopyResource(td_dst.texture.Get(), td_src.texture.Get());
        } else {
            mContext->ResolveSubresource(td_dst.texture.Get(), 0, td_src.texture.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        }
        return;
    }

    if (srcY1 > (int)td_src.height || srcX1 > (int)td_src.width || srcX0 < 0 || srcY0 < 0 ||
        dstY1 > (int)td_dst.height || dstX1 > (int)td_dst.width || dstX0 < 0 || dstY0 < 0) {
        // Using a source region larger than the source resource or copy outside of the destination resource is
        // considered undefined behavior and could lead to removal of the rendering mDevice
        return;
    }

    D3D11_BOX region;
    region.left = srcX0;
    region.right = srcX1;
    region.top = srcY0;
    region.bottom = srcY1;
    region.front = 0;
    region.back = 1;

    // We can't region copy a multi-sample texture to a single sample texture
    if (fb_src.msaa_level <= 1) {
        mContext->CopySubresourceRegion(td_dst.texture.Get(), dstX0, dstY0, 0, 0, td_src.texture.Get(), 0, &region);
    } else {
        // Setup a temporary texture
        TextureData td_resolved;
        td_resolved.width = td_src.width;
        td_resolved.height = td_src.height;

        D3D11_TEXTURE2D_DESC texture_desc;
        texture_desc.Width = td_src.width;
        texture_desc.Height = td_src.height;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture_desc.CPUAccessFlags = 0;
        texture_desc.MiscFlags = 0;
        texture_desc.ArraySize = 1;
        texture_desc.MipLevels = 1;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.SampleDesc.Quality = 0;

        ThrowIfFailed(mDevice->CreateTexture2D(&texture_desc, nullptr, td_resolved.texture.GetAddressOf()));

        // Resolve multi-sample to temporary
        mContext->ResolveSubresource(td_resolved.texture.Get(), 0, td_src.texture.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        // Then copy the region to the destination
        mContext->CopySubresourceRegion(td_dst.texture.Get(), dstX0, dstY0, 0, 0, td_resolved.texture.Get(), 0,
                                        &region);
    }
}

void GfxRenderingAPIDX11::ReadFramebufferToCPU(int fb_id, uint32_t width, uint32_t height, uint16_t* rgba16_buf) {
    if (fb_id >= (int)mFrameBuffers.size()) {
        return;
    }

    FramebufferDX11& fb = mFrameBuffers[fb_id];
    TextureData& td = mTextures[fb.texture_id];

    // Query actual texture dimensions — CopyResource requires matching sizes
    D3D11_TEXTURE2D_DESC srcDesc;
    td.texture->GetDesc(&srcDesc);

    // Reuse cached staging texture when dimensions match — avoids per-frame CreateTexture2D
    if (!mReadbackStaging || mReadbackStagingW != srcDesc.Width || mReadbackStagingH != srcDesc.Height) {
        mReadbackStaging.Reset();

        D3D11_TEXTURE2D_DESC texture_desc;
        texture_desc.Width = srcDesc.Width;
        texture_desc.Height = srcDesc.Height;
        texture_desc.Usage = D3D11_USAGE_STAGING;
        texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        texture_desc.BindFlags = 0;
        texture_desc.MiscFlags = 0;
        texture_desc.ArraySize = 1;
        texture_desc.MipLevels = 1;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.SampleDesc.Quality = 0;

        ThrowIfFailed(mDevice->CreateTexture2D(&texture_desc, nullptr, mReadbackStaging.GetAddressOf()));
        mReadbackStagingW = srcDesc.Width;
        mReadbackStagingH = srcDesc.Height;
    }

    // Copy the framebuffer texture to the staging texture
    mContext->CopyResource(mReadbackStaging.Get(), td.texture.Get());

    // Map the staging texture to a resource that we can read
    D3D11_MAPPED_SUBRESOURCE resource = {};
    ThrowIfFailed(mContext->Map(mReadbackStaging.Get(), 0, D3D11_MAP_READ, 0, &resource));

    if (!resource.pData) {
        mContext->Unmap(mReadbackStaging.Get(), 0);
        return;
    }

    // Convert RGBA32 → RGBA16 with nearest-neighbor scaling from actual texture
    // dimensions to requested output dimensions, respecting RowPitch for row stride
    for (uint32_t j = 0; j < height; j++) {
        uint32_t srcY = j * srcDesc.Height / height;
        uint8_t* srcRow = (uint8_t*)resource.pData + srcY * resource.RowPitch;
        for (uint32_t i = 0; i < width; i++) {
            uint32_t srcX = i * srcDesc.Width / width;
            uint32_t pixel = ((uint32_t*)srcRow)[srcX];
            uint8_t r = (((pixel & 0xFF) + 4) * 0x1F) / 0xFF;
            uint8_t g = ((((pixel >> 8) & 0xFF) + 4) * 0x1F) / 0xFF;
            uint8_t b = ((((pixel >> 16) & 0xFF) + 4) * 0x1F) / 0xFF;
            uint8_t a = ((pixel >> 24) & 0xFF) ? 1 : 0;

            rgba16_buf[i + (j * width)] = (r << 11) | (g << 6) | (b << 1) | a;
        }
    }

    mContext->Unmap(mReadbackStaging.Get(), 0);
}

void GfxRenderingAPIDX11::SetTextureFilter(FilteringMode mode) {
    mCurrentFilterMode = mode;
    gfx_texture_cache_clear();
}

FilteringMode GfxRenderingAPIDX11::GetTextureFilter() {
    return mCurrentFilterMode;
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIDX11::GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    FramebufferDX11& fb = mFrameBuffers[fb_id];
    TextureData& td = mTextures[fb.texture_id];

    if (coordinates.size() > mCoordBufferSize) {
        mCoordBuffer.Reset();
        mCoordBufferSrv.Reset();
        mDepthValueOutputBuffer.Reset();
        mDepthValueOutputUav.Reset();
        mDepthValueOutputBufferCopy.Reset();

        D3D11_BUFFER_DESC coord_buf_desc;
        coord_buf_desc.Usage = D3D11_USAGE_DYNAMIC;
        coord_buf_desc.ByteWidth = sizeof(Coord) * coordinates.size();
        coord_buf_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        coord_buf_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        coord_buf_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        coord_buf_desc.StructureByteStride = sizeof(Coord);

        ThrowIfFailed(mDevice->CreateBuffer(&coord_buf_desc, nullptr, mCoordBuffer.GetAddressOf()));

        D3D11_SHADER_RESOURCE_VIEW_DESC coord_buf_srv_desc;
        coord_buf_srv_desc.Format = DXGI_FORMAT_UNKNOWN;
        coord_buf_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        coord_buf_srv_desc.Buffer.FirstElement = 0;
        coord_buf_srv_desc.Buffer.NumElements = coordinates.size();

        ThrowIfFailed(
            mDevice->CreateShaderResourceView(mCoordBuffer.Get(), &coord_buf_srv_desc, mCoordBufferSrv.GetAddressOf()));

        D3D11_BUFFER_DESC output_buffer_desc;
        output_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
        output_buffer_desc.ByteWidth = sizeof(float) * coordinates.size();
        output_buffer_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        output_buffer_desc.CPUAccessFlags = 0;
        output_buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        output_buffer_desc.StructureByteStride = sizeof(float);
        ThrowIfFailed(mDevice->CreateBuffer(&output_buffer_desc, nullptr, mDepthValueOutputBuffer.GetAddressOf()));

        D3D11_UNORDERED_ACCESS_VIEW_DESC output_buffer_uav_desc;
        output_buffer_uav_desc.Format = DXGI_FORMAT_UNKNOWN;
        output_buffer_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        output_buffer_uav_desc.Buffer.FirstElement = 0;
        output_buffer_uav_desc.Buffer.NumElements = coordinates.size();
        output_buffer_uav_desc.Buffer.Flags = 0;
        ThrowIfFailed(mDevice->CreateUnorderedAccessView(mDepthValueOutputBuffer.Get(), &output_buffer_uav_desc,
                                                         mDepthValueOutputUav.GetAddressOf()));

        output_buffer_desc.Usage = D3D11_USAGE_STAGING;
        output_buffer_desc.BindFlags = 0;
        output_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ThrowIfFailed(mDevice->CreateBuffer(&output_buffer_desc, nullptr, mDepthValueOutputBufferCopy.GetAddressOf()));

        mCoordBufferSize = coordinates.size();
    }

    D3D11_MAPPED_SUBRESOURCE ms;

    if (fb.msaa_level > 1 && mComputeShaderMsaa.Get() == nullptr) {
        ThrowIfFailed(mDevice->CreateComputeShader(mComputeShaderMsaaBlob->GetBufferPointer(),
                                                   mComputeShaderMsaaBlob->GetBufferSize(), nullptr,
                                                   mComputeShaderMsaa.GetAddressOf()));
    }

    // ImGui overwrites these values, so we cannot set them once at init
    mContext->CSSetShader(fb.msaa_level > 1 ? mComputeShaderMsaa.Get() : mComputeShader.Get(), nullptr, 0);
    mContext->CSSetUnorderedAccessViews(0, 1, mDepthValueOutputUav.GetAddressOf(), nullptr);

    ThrowIfFailed(mContext->Map(mCoordBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms));
    Coord* coord_cb = (Coord*)ms.pData;
    {
        size_t i = 0;
        for (const auto& coord : coordinates) {
            coord_cb[i].x = coord.first;
            // We invert y because the gfx_pc assumes OpenGL coordinates (bottom-left corner is origin), while DX's
            // origin is top-left corner
            coord_cb[i].y = td.height - 1 - coord.second;
            ++i;
        }
    }
    mContext->Unmap(mCoordBuffer.Get(), 0);

    // The depth stencil texture can only have one mapping at a time, so unbind from the OM
    ID3D11RenderTargetView* null_arr1[1] = { nullptr };
    mContext->OMSetRenderTargets(1, null_arr1, nullptr);

    ID3D11ShaderResourceView* srvs[2] = { fb.depth_stencil_srv.Get(), mCoordBufferSrv.Get() };
    mContext->CSSetShaderResources(0, 2, srvs);

    mContext->Dispatch(coordinates.size(), 1, 1);

    mContext->CopyResource(mDepthValueOutputBufferCopy.Get(), mDepthValueOutputBuffer.Get());
    ThrowIfFailed(mContext->Map(mDepthValueOutputBufferCopy.Get(), 0, D3D11_MAP_READ, 0, &ms));
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res;
    {
        size_t i = 0;
        for (const auto& coord : coordinates) {
            const float normalizedDepth = ((float*)ms.pData)[i++];
            const auto depthValue =
                static_cast<uint16_t>(std::clamp(normalizedDepth * 65532.0f, 0.0f, 65532.0f));
            res.emplace(coord, depthValue);
        }
    }
    mContext->Unmap(mDepthValueOutputBufferCopy.Get(), 0);

    ID3D11ShaderResourceView* null_arr[2] = { nullptr, nullptr };
    mContext->CSSetShaderResources(0, 2, null_arr);

    return res;
}

ImTextureID GfxRenderingAPIDX11::GetTextureById(int id) {
    return mTextures[id].resource_view.Get();
}

void GfxRenderingAPIDX11::SetSrgbMode() {
    mSrgbMode = true;
}

#define RAND_NOISE "((random(float3(floor(screenSpace.xy * noise_scale), noise_frame)) + 1.0) / 2.0)"

static const char* prism_shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha,
                                            bool first_cycle, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            default:
            case SHADER_0:
                return with_alpha ? "float4(0.0, 0.0, 0.0, 0.0)" : "float3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "float4(1.0, 1.0, 1.0, 1.0)" : "float3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "input.input1" : "input.input1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "input.input2" : "input.input2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "input.input3" : "input.input3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "input.input4" : "input.input4.rgb";
            case SHADER_TEXEL0:
                return first_cycle ? (with_alpha ? "texVal0" : "texVal0.rgb")
                                   : (with_alpha ? "texVal1" : "texVal1.rgb");
            case SHADER_TEXEL0A:
                return first_cycle
                           ? (hint_single_element ? "texVal0.a"
                                                  : (with_alpha ? "float4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                                : "float3(texVal0.a, texVal0.a, texVal0.a)"))
                           : (hint_single_element ? "texVal1.a"
                                                  : (with_alpha ? "float4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                                : "float3(texVal1.a, texVal1.a, texVal1.a)"));
            case SHADER_TEXEL1A:
                return first_cycle
                           ? (hint_single_element ? "texVal1.a"
                                                  : (with_alpha ? "float4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                                : "float3(texVal1.a, texVal1.a, texVal1.a)"))
                           : (hint_single_element ? "texVal0.a"
                                                  : (with_alpha ? "float4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                                : "float3(texVal0.a, texVal0.a, texVal0.a)"));
            case SHADER_TEXEL1:
                return first_cycle ? (with_alpha ? "texVal1" : "texVal1.rgb")
                                   : (with_alpha ? "texVal0" : "texVal0.rgb");
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:
                return with_alpha ? "float4(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")"
                                  : "float3(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")";
        }
    } else {
        switch (item) {
            default:
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "input.input1.a";
            case SHADER_INPUT_2:
                return "input.input2.a";
            case SHADER_INPUT_3:
                return "input.input3.a";
            case SHADER_INPUT_4:
                return "input.input4.a";
            case SHADER_TEXEL0:
                return first_cycle ? "texVal0.a" : "texVal1.a";
            case SHADER_TEXEL0A:
                return first_cycle ? "texVal0.a" : "texVal1.a";
            case SHADER_TEXEL1A:
                return first_cycle ? "texVal1.a" : "texVal0.a";
            case SHADER_TEXEL1:
                return first_cycle ? "texVal1.a" : "texVal0.a";
            case SHADER_COMBINED:
                return "texel.a";
            case SHADER_NOISE:
                return RAND_NOISE;
        }
    }
}

bool prism_get_bool(prism::ContextTypes* value) {
    if (std::holds_alternative<int>(*value)) {
        return std::get<int>(*value) == 1;
    }
    return false;
}

#undef RAND_NOISE

prism::ContextTypes* prism_append_formula(prism::ContextTypes* _, prism::ContextTypes* a_arg,
                                          prism::ContextTypes* a_single, prism::ContextTypes* a_mult,
                                          prism::ContextTypes* a_mix, prism::ContextTypes* a_with_alpha,
                                          prism::ContextTypes* a_only_alpha, prism::ContextTypes* a_alpha,
                                          prism::ContextTypes* a_first_cycle) {
    auto c = std::get<prism::MTDArray<int>>(*a_arg);
    bool do_single = prism_get_bool(a_single);
    bool do_multiply = prism_get_bool(a_mult);
    bool do_mix = prism_get_bool(a_mix);
    bool with_alpha = prism_get_bool(a_with_alpha);
    bool only_alpha = prism_get_bool(a_only_alpha);
    bool opt_alpha = prism_get_bool(a_alpha);
    bool first_cycle = prism_get_bool(a_first_cycle);
    std::string out = "";
    if (do_single) {
        out += prism_shader_item_to_str(c.at(only_alpha, 3), with_alpha, only_alpha, opt_alpha, first_cycle, false);
    } else if (do_multiply) {
        out += prism_shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += " * ";
        out += prism_shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
    } else if (do_mix) {
        out += "lerp(";
        out += prism_shader_item_to_str(c.at(only_alpha, 1), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ", ";
        out += prism_shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ", ";
        out += prism_shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
        out += ")";
    } else {
        out += "(";
        out += prism_shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += " - ";
        out += prism_shader_item_to_str(c.at(only_alpha, 1), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ") * ";
        out += prism_shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
        out += " + ";
        out += prism_shader_item_to_str(c.at(only_alpha, 3), with_alpha, only_alpha, opt_alpha, first_cycle, false);
    }
    return new prism::ContextTypes{ out };
}

static size_t raw_numFloats = 0;

prism::ContextTypes* update_raw_floats(prism::ContextTypes* _, prism::ContextTypes* num) {
    raw_numFloats += std::get<int>(*num);
    return nullptr;
}

std::optional<std::string> dx_include_fs(const std::string& path) {
    auto init = std::make_shared<Ship::ResourceInitData>();
    init->Type = (uint32_t)Ship::ResourceType::Shader;
    init->ByteOrder = Ship::Endianness::Native;
    init->Format = RESOURCE_FORMAT_BINARY;
    auto res = static_pointer_cast<Ship::Shader>(
        Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(path, true, init));
    if (res == nullptr) {
        return std::nullopt;
    }

    auto inc = static_cast<std::string*>(res->GetRawPointer());
    return *inc;
}

std::string gfx_direct3d_common_build_shader(size_t& numFloats, const CCFeatures& cc_features,
                                             bool include_root_signature, bool three_point_filtering, bool use_srgb) {
    raw_numFloats = 4;

    prism::Processor processor;
    prism::ContextItems mContext = {
        { "SHADER_0", SHADER_0 },
        { "SHADER_INPUT_1", SHADER_INPUT_1 },
        { "SHADER_INPUT_2", SHADER_INPUT_2 },
        { "SHADER_INPUT_3", SHADER_INPUT_3 },
        { "SHADER_INPUT_4", SHADER_INPUT_4 },
        { "SHADER_INPUT_5", SHADER_INPUT_5 },
        { "SHADER_INPUT_6", SHADER_INPUT_6 },
        { "SHADER_INPUT_7", SHADER_INPUT_7 },
        { "SHADER_TEXEL0", SHADER_TEXEL0 },
        { "SHADER_TEXEL0A", SHADER_TEXEL0A },
        { "SHADER_TEXEL1", SHADER_TEXEL1 },
        { "SHADER_TEXEL1A", SHADER_TEXEL1A },
        { "SHADER_1", SHADER_1 },
        { "SHADER_COMBINED", SHADER_COMBINED },
        { "SHADER_NOISE", SHADER_NOISE },
        { "o_c", M_ARRAY(cc_features.c, int, 2, 2, 4) },
        { "o_alpha", cc_features.opt_alpha },
        { "o_fog", cc_features.opt_fog },
        { "o_texture_edge", cc_features.opt_texture_edge },
        { "o_noise", cc_features.opt_noise },
        { "o_2cyc", cc_features.opt_2cyc },
        { "o_alpha_threshold", cc_features.opt_alpha_threshold },
        { "o_invisible", cc_features.opt_invisible },
        { "o_grayscale", cc_features.opt_grayscale },
        { "o_prim_depth", cc_features.opt_prim_depth },
        { "o_textures", M_ARRAY(cc_features.usedTextures, bool, 2) },
        { "o_masks", M_ARRAY(cc_features.used_masks, bool, 2) },
        { "o_blend", M_ARRAY(cc_features.used_blend, bool, 2) },
        { "o_clamp", M_ARRAY(cc_features.clamp, bool, 2, 2) },
        { "o_inputs", cc_features.numInputs },
        { "o_do_mix", M_ARRAY(cc_features.do_mix, bool, 2, 2) },
        { "o_do_single", M_ARRAY(cc_features.do_single, bool, 2, 2) },
        { "o_do_multiply", M_ARRAY(cc_features.do_multiply, bool, 2, 2) },
        { "o_color_alpha_same", M_ARRAY(cc_features.color_alpha_same, bool, 2) },
        { "o_root_signature", include_root_signature },
        { "o_three_point_filtering", three_point_filtering },
        { "srgb_mode", use_srgb },
        { "append_formula", (InvokeFunc)prism_append_formula },
        { "update_floats", (InvokeFunc)update_raw_floats },
    };
    processor.populate(mContext);
    auto init = std::make_shared<Ship::ResourceInitData>();
    init->Type = (uint32_t)Ship::ResourceType::Shader;
    init->ByteOrder = Ship::Endianness::Native;
    init->Format = RESOURCE_FORMAT_BINARY;
    const char* shaderName = Fast::gfx_get_shader(cc_features.shader_id);
    std::string path = "shaders/directx/default.shader.hlsl";

    if (nullptr != shaderName) {
        path = std::string(shaderName) + ".hlsl";
    }

    auto res = static_pointer_cast<Ship::Shader>(Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(
        "shaders/directx/default.shader.hlsl", true, init));

    if (res == nullptr) {
        SPDLOG_ERROR("Failed to load default directx shader, missing f3d.o2r?");
        abort();
    }

    auto shader = static_cast<std::string*>(res->GetRawPointer());
    processor.load(*shader);
    processor.bind_include_loader(dx_include_fs);
    auto result = processor.process();
    numFloats = raw_numFloats;
    // SPDLOG_INFO("=========== DX11 SHADER ============");
    // SPDLOG_INFO(result);
    // SPDLOG_INFO("====================================");
    return result;
}
} // namespace Fast
#endif
