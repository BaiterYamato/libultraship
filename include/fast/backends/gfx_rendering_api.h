#pragma once

#include <stdint.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <unordered_map>
#include <set>
#include "imconfig.h"

namespace Fast {
struct ShaderProgram;

struct GfxClipParameters {
    bool z_is_from_0_to_1;
    bool invertY;
};

struct GfxAmbientOcclusionConfig {
    bool enabled = false;
    uint8_t quality = 0; // 0=off, 1=low, 2=medium, 3=high
    float radius = 0.55f;
    float intensity = 0.9f;
    float bias = 0.02f;
    float power = 1.2f;
    float maxDistance = 1200.0f;
    int32_t blurPasses = 2;
    bool debugView = false;
};

constexpr size_t GFX_MAX_VOLUMETRIC_LIGHTS = 8;

enum GfxVolumetricLightType : uint8_t {
    GFX_VOLUMETRIC_LIGHT_DIRECTIONAL = 0,
    GFX_VOLUMETRIC_LIGHT_POINT = 1,
    GFX_VOLUMETRIC_LIGHT_SPOT = 2,
};

enum GfxVolumetricsDebugMode : uint8_t {
    GFX_VOLUMETRICS_DEBUG_OFF = 0,
    GFX_VOLUMETRICS_DEBUG_FOG_DENSITY = 1,
    GFX_VOLUMETRICS_DEBUG_LIGHT_ENERGY = 2,
    GFX_VOLUMETRICS_DEBUG_FINAL_VOLUME = 3,
    GFX_VOLUMETRICS_DEBUG_TRANSMITTANCE = 4,
    GFX_VOLUMETRICS_DEBUG_SHADOW_OCCLUSION = 5,
};

enum GfxVolumetricsPerformanceMode : uint8_t {
    GFX_VOLUMETRICS_PERF_PERFORMANCE = 0,
    GFX_VOLUMETRICS_PERF_BALANCED = 1,
    GFX_VOLUMETRICS_PERF_QUALITY = 2,
};

enum GfxVolumetricsShadowMode : uint8_t {
    GFX_VOLUMETRICS_SHADOW_OFF = 0,
    GFX_VOLUMETRICS_SHADOW_SCREEN_DEPTH_PROXY = 1,
    GFX_VOLUMETRICS_SHADOW_REAL_SHADOW = 2,
};

struct GfxVolumetricLight {
    uint8_t type = GFX_VOLUMETRIC_LIGHT_POINT;
    uint8_t castShadows = 0;
    uint8_t reserved0 = 0;
    uint8_t reserved1 = 0;
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float radius = 0.0f;
    float direction[3] = { 0.0f, -1.0f, 0.0f };
    float intensity = 1.0f;
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float volumetricIntensity = 1.0f;
    float innerConeCos = 0.9396926f;
    float outerConeCos = 0.7660444f;
    float shadowBias = 0.01f;
    float shadowNormalBias = 0.02f;
    float shadowRange = 800.0f;
    float shadowResolution = 512.0f;
};

struct GfxVolumetricsConfig {
    bool enabled = false;
    uint8_t quality = 0; // 0=off, 1=low, 2=medium, 3=high
    uint8_t shadowQuality = 0; // 0=off, 1=low, 2=medium, 3=high
    uint8_t performanceMode = GFX_VOLUMETRICS_PERF_BALANCED;
    bool heightFogEnabled = false;
    bool debugView = false;
    uint8_t debugMode = GFX_VOLUMETRICS_DEBUG_OFF;
    bool resetHistory = false;
    uint8_t maxShadowedLights = 0;
    float density = 0.03f;
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float ambientIntensity = 0.35f;
    float anisotropy = 0.2f;
    float startDistance = 64.0f;
    float maxDistance = 2400.0f;
    float baseHeight = 0.0f;
    float heightFalloff = 0.0025f;
    float lightShaftIntensity = 1.0f;
    float shadowIntensity = 0.6f;
    float temporalBlend = 0.88f;
    float jitterScale = 1.0f;
    float aerialPerspectiveStrength = 1.0f;
    float macroNoiseStrength = 1.0f;
    float valleyFogStrength = 1.0f;
    float edgeHazeStrength = 1.0f;
    float depthExtinctionStrength = 1.0f;
    float depthExtinctionExponent = 1.25f;
    float horizonFogStrength = 1.0f;
    float skyFallbackStrength = 0.18f;
    float directionalShadowBias = 0.01f;
    float directionalShadowNormalBias = 0.02f;
    float directionalShadowSoftness = 1.0f;
    float cameraWorldPosition[3] = { 0.0f, 0.0f, 0.0f };
    float nearPlane = 10.0f;
    float farPlane = 3200.0f;
    float currentViewProjection[16] = { 0.0f };
    float inverseViewProjection[16] = { 0.0f };
    float previousViewProjection[16] = { 0.0f };
};

struct GfxVolumetricsRuntimeStats {
    bool executed = false;
    bool dispatched = false;
    bool producedMedium = false;
    bool producedVolume = false;
    bool producedFinalVolume = false;
    bool debugPreviewValid = false;
    bool historyValid = false;
    bool hasActiveLightPayload = false;
    bool depthFogPathActive = false;
    bool raymarchActive = false;
    uint8_t performanceMode = GFX_VOLUMETRICS_PERF_BALANCED;
    uint8_t shadowMode = GFX_VOLUMETRICS_SHADOW_OFF;
    uint32_t payloadLightCount = 0;
    uint32_t lowWidth = 0;
    uint32_t lowHeight = 0;
    uint32_t stepCount = 0;
    uint32_t raymarchPixelCount = 0;
    uint32_t statsAgeFrames = 0;
    uint32_t marchedPixelCount = 0;
    float avgDensity = 0.0f;
    float avgLightEnergy = 0.0f;
    float avgFinalVolume = 0.0f;
    float avgTransmittance = 1.0f;
    float maxDensity = 0.0f;
    float maxLightEnergy = 0.0f;
    float maxFinalVolume = 0.0f;
    float maxOpticalDepth = 0.0f;
    uint32_t directionalShadowMapSize = 0;
    int32_t directionalShadowCasterIndex = -1;
};

enum FilteringMode { FILTER_THREE_POINT, FILTER_LINEAR, FILTER_NONE };

// A hash function used to hash a: pair<float, float>
struct hash_pair_ff {
    size_t operator()(const std::pair<float, float>& p) const {
        const auto hash1 = std::hash<float>{}(p.first);
        const auto hash2 = std::hash<float>{}(p.second);

        // If hash1 == hash2, their XOR is zero.
        return (hash1 != hash2) ? hash1 ^ hash2 : hash1;
    }
};

class GfxRenderingAPI {
  public:
    virtual ~GfxRenderingAPI() = default;
    virtual const char* GetName() = 0;
    virtual int GetMaxTextureSize() = 0;
    virtual GfxClipParameters GetClipParameters() = 0;
    virtual void UnloadShader(ShaderProgram* oldPrg) = 0;
    virtual void LoadShader(ShaderProgram* newPrg) = 0;
    virtual ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint32_t shaderId1) = 0;
    virtual ShaderProgram* LookupShader(uint64_t shaderId0, uint32_t shaderId1) = 0;
    virtual void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) = 0;
    virtual uint32_t NewTexture() = 0;
    virtual void SelectTexture(int tile, uint32_t textureId) = 0;
    virtual void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) = 0;
    virtual void SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) = 0;
    virtual void SetDepthTestAndMask(bool depth_test, bool z_upd) = 0;
    virtual void SetZmodeDecal(bool decal) = 0;
    virtual void SetViewport(int x, int y, int width, int height) = 0;
    virtual void SetScissor(int x, int y, int width, int height) = 0;
    virtual void SetUseAlpha(bool useAlpha) = 0;
    virtual void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) = 0;
    virtual void Init() = 0;
    virtual void OnResize() = 0;
    virtual void StartFrame() = 0;
    virtual void ApplyAmbientOcclusion() {}
    virtual void ApplyVolumetrics() {}
    virtual void EndFrame() = 0;
    virtual void FinishRender() = 0;
    virtual int CreateFramebuffer() = 0;
    virtual void UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                             bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                             bool can_extract_depth) = 0;
    virtual void StartDrawToFramebuffer(int fbId, float noiseScale) = 0;
    virtual void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0,
                                 int dstY0, int dstX1, int dstY1) = 0;
    virtual void ClearFramebuffer(bool color, bool depth) = 0;
    virtual void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) = 0;
    virtual void ResolveMSAAColorBuffer(int fbIdTarger, int fbIdSrc) = 0;
    virtual std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) = 0;
    virtual void* GetFramebufferTextureId(int fbId) = 0;
    virtual void SelectTextureFb(int fbId) = 0;
    virtual void DeleteTexture(uint32_t texId) = 0;
    virtual void SetTextureFilter(FilteringMode mode) = 0;
    virtual FilteringMode GetTextureFilter() = 0;
    virtual void SetSrgbMode() = 0;
    virtual ImTextureID GetTextureById(int id) = 0;
    virtual void SetAmbientOcclusionConfig(const GfxAmbientOcclusionConfig& config) {
        mAmbientOcclusionConfig = config;
    }
    virtual GfxAmbientOcclusionConfig GetAmbientOcclusionConfig() const {
        return mAmbientOcclusionConfig;
    }
    virtual bool IsAmbientOcclusionSupported() const {
        return false;
    }
    virtual int32_t GetAmbientOcclusionFallbackCode() const {
        return 0;
    }
    virtual void ClearAmbientOcclusionFallbackCode() {
    }
    virtual void SetVolumetricsConfig(const GfxVolumetricsConfig& config) {
        mVolumetricsConfig = config;
    }
    virtual GfxVolumetricsConfig GetVolumetricsConfig() const {
        return mVolumetricsConfig;
    }
    virtual void SetVolumetricLights(const GfxVolumetricLight* lights, size_t count) {
        mVolumetricLightCount = std::min(count, mVolumetricLights.size());
        for (size_t i = 0; i < mVolumetricLightCount; ++i) {
            mVolumetricLights[i] = lights[i];
        }
        for (size_t i = mVolumetricLightCount; i < mVolumetricLights.size(); ++i) {
            mVolumetricLights[i] = {};
        }
    }
    virtual size_t GetVolumetricLightCount() const {
        return mVolumetricLightCount;
    }
    virtual bool IsVolumetricsSupported() const {
        return false;
    }
    virtual GfxVolumetricsRuntimeStats GetVolumetricsRuntimeStats() const {
        return mVolumetricsRuntimeStats;
    }
    virtual int32_t GetVolumetricsFallbackCode() const {
        return 0;
    }
    virtual void ClearVolumetricsFallbackCode() {
    }

  protected:
    int8_t mCurrentDepthTest = 0;
    int8_t mCurrentDepthMask = 0;
    int8_t mCurrentZmodeDecal = 0;
    int8_t mLastDepthTest = -1;
    int8_t mLastDepthMask = -1;
    int8_t mLastZmodeDecal = -1;
    bool mSrgbMode = false;
    GfxAmbientOcclusionConfig mAmbientOcclusionConfig{};
    GfxVolumetricsConfig mVolumetricsConfig{};
    GfxVolumetricsRuntimeStats mVolumetricsRuntimeStats{};
    std::array<GfxVolumetricLight, GFX_MAX_VOLUMETRIC_LIGHTS> mVolumetricLights{};
    size_t mVolumetricLightCount = 0;
};
} // namespace Fast
