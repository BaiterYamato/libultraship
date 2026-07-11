#pragma once

#if defined(ENABLE_DX11) || defined(ENABLE_DX12)

#ifdef __cplusplus
#include "../interpreter.h"
#include <cstdint>
#include <string>
#include "gfx_rendering_api.h"
#include "d3d11.h"
#include "d3dcompiler.h"

namespace Fast {

struct PerFrameCB {
    uint32_t noise_frame;
    float noise_scale;
    uint32_t padding[2]; // constant buffers must be multiples of 16 bytes in size
};

struct PerDrawCB {
    struct Texture {
        uint32_t width;
        uint32_t height;
        uint32_t linear_filtering;
        uint32_t padding;
    } mTextures[SHADER_MAX_TEXTURES];
};

struct Coord {
    int x, y;
};

struct TextureData {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resource_view;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_state;
    uint32_t width;
    uint32_t height;
    bool linear_filtering;
};

struct FramebufferDX11 {
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_view;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_stencil_view;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth_stencil_srv;
    uint32_t texture_id;
    bool has_depth_buffer;
    uint32_t msaa_level;
};

struct ShaderProgramD3D11 {
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state;

    uint64_t shader_id0;
    uint32_t shader_id1;
    uint8_t numInputs;
    uint8_t numFloats;
    bool usedTextures[SHADER_MAX_TEXTURES];
};

class GfxWindowBackendDXGI;

class GfxRenderingAPIDX11 final : public GfxRenderingAPI {
  public:
    GfxRenderingAPIDX11() = default;
    ~GfxRenderingAPIDX11() override;
    GfxRenderingAPIDX11(GfxWindowBackendDXGI* backend);
    const char* GetName() override;
    int GetMaxTextureSize() override;
    GfxClipParameters GetClipParameters() override;
    void UnloadShader(struct ShaderProgram* oldPrg) override;
    void LoadShader(struct ShaderProgram* newPrg) override;
    struct ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint32_t shaderId1) override;
    struct ShaderProgram* LookupShader(uint64_t shaderId0, uint32_t shaderId1) override;
    void ShaderGetInfo(struct ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override;
    uint32_t NewTexture() override;
    void SelectTexture(int tile, uint32_t textureId) override;
    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override;
    void SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) override;
    void SetDepthTestAndMask(bool depth_test, bool z_upd) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height) override;
    void SetUseAlpha(bool useAlpha) override;
    void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) override;
    void Init() override;
    void OnResize() override;
    void StartFrame() override;
    void ApplyAmbientOcclusion() override;
    void ApplyVolumetrics() override;
    void EndFrame() override;
    void FinishRender() override;
    int CreateFramebuffer() override;
    void UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                     bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                     bool can_extract_depth) override;
    void StartDrawToFramebuffer(int fbId, float noiseScale) override;
    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
                         int dstX1, int dstY1) override;
    void ClearFramebuffer(bool color, bool depth) override;
    void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override;
    void ResolveMSAAColorBuffer(int fbIdTarger, int fbIdSrc) override;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int fbId) override;
    void SelectTextureFb(int fbId) override;
    void DeleteTexture(uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;
    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;
    bool IsAmbientOcclusionSupported() const override;
    int32_t GetAmbientOcclusionFallbackCode() const override;
    void ClearAmbientOcclusionFallbackCode() override;
    bool IsVolumetricsSupported() const override;
    int32_t GetVolumetricsFallbackCode() const override;
    void ClearVolumetricsFallbackCode() override;

    PFN_D3D11_CREATE_DEVICE mDX11CreateDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> mContext;
    Microsoft::WRL::ComPtr<ID3D11Device> mDevice;
    GfxWindowBackendDXGI* mWindowBackend = nullptr;
    D3D_FEATURE_LEVEL mFeatureLevel;

  private:
    void CreateDepthStencilObjects(uint32_t width, uint32_t height, uint32_t msaa_count, ID3D11DepthStencilView** view,
                                   ID3D11ShaderResourceView** srv);
    bool EnsureAmbientOcclusionResources(uint32_t width, uint32_t height);
    bool EnsureAmbientOcclusionShaders();
    bool EnsureVolumetricsResources(uint32_t fullWidth, uint32_t fullHeight, uint32_t lowWidth, uint32_t lowHeight);
    bool EnsureVolumetricsShaders();

    HMODULE mDX11Module;

    HMODULE mCompilerModule;
    pD3DCompile mD3dCompile;

    uint32_t mMsaaNumQualityLevels[D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT];

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> mRasterizerState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> mDepthStencilState;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mPerFrameCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mPerDrawCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mAoParamsCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mCoordBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mCoordBufferSrv;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mDepthValueOutputBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mDepthValueOutputBufferCopy;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> mDepthValueOutputUav;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mComputeShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mComputeShaderMsaa;
    Microsoft::WRL::ComPtr<ID3DBlob> mComputeShaderMsaaBlob;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> mAoVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> mAoPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> mAoPixelShaderMsaa;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> mAoBlurPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> mAoCompositePixelShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> mAoSamplerState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> mAoMultiplyBlendState;
    size_t mCoordBufferSize;

#if DEBUG_D3D
    Microsoft::WRL::ComPtr<ID3D11Debug> debug;
#endif

    PerFrameCB mPerFrameCbData;
    PerDrawCB mPerDrawCbData;

    std::map<std::pair<uint64_t, uint32_t>, struct ShaderProgramD3D11> mShaderProgramPool;

    std::vector<struct TextureData> mTextures;
    int mCurrentTile;
    uint32_t mCurrentTextureIds[SHADER_MAX_TEXTURES];

    std::vector<FramebufferDX11> mFrameBuffers;

    // Current state

    struct ShaderProgramD3D11* mShaderProgram;

    int32_t mRenderTargetHeight;
    int mCurrentFramebuffer;
    FilteringMode mCurrentFilterMode = FILTER_NONE;

    // Previous states (to prevent setting states needlessly)

    struct ShaderProgramD3D11* mLastShaderProgram = nullptr;
    uint32_t mLastVertexBufferStride = 0;
    Microsoft::WRL::ComPtr<ID3D11BlendState> mLastBlendState = nullptr;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mLastResourceViews[SHADER_MAX_TEXTURES] = { nullptr, nullptr };
    Microsoft::WRL::ComPtr<ID3D11SamplerState> mLastSamplerStates[SHADER_MAX_TEXTURES] = { nullptr, nullptr };

    D3D_PRIMITIVE_TOPOLOGY mLastPrimitaveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    int32_t mAmbientOcclusionFallbackCode = 0;
    int32_t mVolumetricsFallbackCode = 0;

    struct AoParams {
        float invResolution[2] = { 0.0f, 0.0f };
        float radiusPixels = 4.0f;
        float intensity = 1.0f;
        float bias = 0.01f;
        float power = 1.0f;
        float maxDistance = 1200.0f;
        uint32_t sampleCount = 8;
        uint32_t debugView = 0;
        float blurDirection[2] = { 1.0f, 0.0f };
    } mAoParams;

    struct AoRenderTargets {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };
    AoRenderTargets mAoPrimary;
    AoRenderTargets mAoBlur;
    uint32_t mAoWidth = 0;
    uint32_t mAoHeight = 0;

    struct VolumetricsParams {
        float invFullResolution[2] = { 0.0f, 0.0f };
        float invLowResolution[2] = { 0.0f, 0.0f };
        float cameraWorldPosition[3] = { 0.0f, 0.0f, 0.0f };
        float density = 0.0f;
        float fogColor[3] = { 1.0f, 1.0f, 1.0f };
        float ambientIntensity = 0.35f;
        float anisotropy = 0.0f;
        float startDistance = 0.0f;
        float maxDistance = 0.0f;
        float baseHeight = 0.0f;
        float heightFalloff = 0.0f;
        float lightShaftIntensity = 0.0f;
        float shadowIntensity = 0.0f;
        float temporalBlend = 0.0f;
        float jitterScale = 0.0f;
        float nearPlane = 0.0f;
        float farPlane = 0.0f;
        float paddingAfterFarPlane = 0.0f;
        float depthNdcParams[2] = { 2.0f, -1.0f };
        float ndcDepthParams[2] = { 0.5f, 0.5f };
        float debugDensityScale = 1.0f;
        float debugLightScale = 1.0f;
        float debugFinalScale = 1.0f;
        float debugPreviewBlend = 1.0f;
        float debugDensityBias = 0.0f;
        float debugLightBias = 0.0f;
        float debugFinalBias = 0.0f;
        float debugMediumBias = 0.0f;
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
        float padding3 = 0.0f;
        uint32_t stepCount = 0;
        uint32_t lightCount = 0;
        uint32_t shadowQuality = 0;
        uint32_t historyValid = 0;
        uint32_t heightFogEnabled = 0;
        uint32_t debugMode = 0;
        uint32_t resetHistory = 0;
        uint32_t performanceMode = GFX_VOLUMETRICS_PERF_BALANCED;
        uint32_t raymarchEnabled = 1;
        uint32_t depthFogPathEnabled = 1;
        uint32_t shadowMode = GFX_VOLUMETRICS_SHADOW_OFF;
        uint32_t padding1 = 0;
        float currViewProj[16] = { 0.0f };
        float invViewProj[16] = { 0.0f };
        float prevViewProj[16] = { 0.0f };
        uint32_t directionalShadowEnabled = 0;
        uint32_t directionalShadowLightIndex = 0;
        uint32_t directionalShadowMapSize = 0;
        uint32_t padding2 = 0;
        float directionalShadowViewProj[16] = { 0.0f };
        float lightPositionRadius[GFX_MAX_VOLUMETRIC_LIGHTS][4] = {};
        float lightDirectionIntensity[GFX_MAX_VOLUMETRIC_LIGHTS][4] = {};
        float lightColorVolumetric[GFX_MAX_VOLUMETRIC_LIGHTS][4] = {};
        float lightConeShadow[GFX_MAX_VOLUMETRIC_LIGHTS][4] = {};
        float lightShadowParams[GFX_MAX_VOLUMETRIC_LIGHTS][4] = {};
    } mVolumetricsParams;

    struct VolumetricsRenderTargets {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> volumeTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> volumeRtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> volumeSrv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> depthRtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSrv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> metricsTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> metricsRtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> metricsSrv;
    };
    Microsoft::WRL::ComPtr<ID3D11Buffer> mVolumetricsParamsCb;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> mVolumetricsVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> mVolumetricsRaymarchPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> mVolumetricsCompositePixelShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mVolumetricsDirectionalShadowComputeShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> mVolumetricsSamplerState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> mVolumetricsAdditiveBlendState;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mVolumetricsMetricsReadback;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mVolumetricsDirectionalShadowTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mVolumetricsDirectionalShadowSrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> mVolumetricsDirectionalShadowUav;
    VolumetricsRenderTargets mVolumetricsTargets[2];
    uint32_t mVolumetricsFullWidth = 0;
    uint32_t mVolumetricsFullHeight = 0;
    uint32_t mVolumetricsLowWidth = 0;
    uint32_t mVolumetricsLowHeight = 0;
    uint32_t mVolumetricsDirectionalShadowSize = 0;
    uint32_t mVolumetricsHistoryIndex = 0;
    uint32_t mVolumetricsStatsReadbackFrame = 0;
    bool mVolumetricsHistoryValid = false;
    bool mVolumetricsMetricsReadbackPending = false;
};

std::string gfx_direct3d_common_build_shader(size_t& numFloats, const CCFeatures& cc_features,
                                             bool include_root_signature, bool three_point_filtering, bool use_srgb);
} // namespace Fast
#endif
#endif
