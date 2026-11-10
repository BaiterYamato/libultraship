#pragma once

#if defined(ENABLE_DX11) || defined(ENABLE_DX12)

#ifdef __cplusplus
#include "../interpreter.h"
#include "../fog_volume.h"
#include <array>
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

// SOH [Enhancement] Toon lighting: the per-object dominant light + frame-global ramp shape. Kept in
// its own constant buffer (register b2) rather than PerFrameCB so that buffer stays purely frame-global;
// this one is re-uploaded per toon draw. The padding keeps each float3 on a 16-byte boundary so the
// layout matches the HLSL cbuffer packing rules. Total size 64 bytes (multiple of 16).
struct PerToonCB {
    float toon_light_dir[3];
    float toon_ramp_center;
    float toon_light_color[3];
    float toon_ramp_softness;
    float toon_ambient[3];
    float toon_highlight_intensity;
    float toon_shadow_intensity;
    float toon_debug;
    float _toon_pad[2];
};

struct PerDrawCB {
    struct Texture {
        uint32_t width;
        uint32_t height;
        uint32_t linear_filtering;
        uint32_t padding;
    } mTextures[SHADER_MAX_TEXTURES];
};

struct PerPrimDepthCB {
    float prim_depth;
    float _pad[3]; // 16-byte CB alignment
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
    uint64_t shader_id1;
    uint8_t numInputs;
    uint8_t numFloats;
    bool usedTextures[SHADER_MAX_TEXTURES];
    bool opt_toon = false; // SOH [Enhancement] toon lighting variant
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
    struct ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) override;
    struct ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) override;
    void ShaderGetInfo(struct ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override;
    void ClearShaderCache() override;
    uint32_t NewTexture() override;
    void SelectTexture(int tile, uint32_t textureId) override;
    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override;
    void SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) override;
    void SetDepthTestAndMask(bool depth_test, bool z_upd) override;
    void SetCurrentPrimDepth(float depth) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height) override;
    void SetUseAlpha(bool useAlpha) override;
    void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) override;
    void Init() override;
    void OnResize() override;
    void StartFrame() override;
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
    bool SupportsFogVolume() const override { return true; }
    bool SupportsFogFlow() const override { return true; }
    void DrawFogVolume(const FastFogVolume& volume, const FastFogFlow* flow) override;
    void SelectTextureFb(int fbId) override;
    void DeleteTexture(uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;
    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;

    PFN_D3D11_CREATE_DEVICE mDX11CreateDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> mContext;
    Microsoft::WRL::ComPtr<ID3D11Device> mDevice;
    GfxWindowBackendDXGI* mWindowBackend = nullptr;
    D3D_FEATURE_LEVEL mFeatureLevel;

  private:
    bool CompileFogShader(const char* entry, const char* profile, ID3DBlob** blob, bool multisampled = false);
    bool InitFogVolume();
    void UpdateFogSupportRanges(const FastFogVolume& volume, const FastFogFlow& flow, UINT width, UINT height);
    void UpdateFogLift(const FastFogFlow& flow, FogFlowAction action, UINT width, UINT height);
    void UpdateFogUpper(const FastFogVolume& volume, FogFlowAction action, UINT width, UINT height);
    bool InitFogMsaa();
    bool InitFogLod(UINT width, UINT height, bool multisampled, bool useSun);
    bool InitFogMsaaCache(UINT width, UINT height);
    void DrawFogLod(const FastFogVolume& volume, const FastFogFlow& flow, FogFlowAction action,
                    const D3D11_VIEWPORT& viewport, int mode, float fieldFraction, float extinction,
                    bool useSun, ID3D11Query* lowEnd);
    bool InitFogFlow();
    bool InitFogVertical();
    bool InitFogParticles();
    bool InitFogFieldBlend();
    ID3D11ShaderResourceView* BlendFogField(ID3D11ShaderResourceView* current, UINT width, UINT height, float fraction);
    ID3D11ShaderResourceView* UpdateFogFlow(const FastFogVolume& volume, const FastFogFlow& flow,
                                          FogFlowAction& action, ID3D11Query* particleBegin, bool& particles);
    void AuditFogFlow(const FastFogVolume& volume, const FastFogFlow& flow, FogFlowCursor cursor,
                      ID3D11ShaderResourceView* source);
    void CollectFogTiming();
    struct FogTiming {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> begin;
        Microsoft::WRL::ComPtr<ID3D11Query> fieldEnd;
        Microsoft::WRL::ComPtr<ID3D11Query> particleBegin;
        Microsoft::WRL::ComPtr<ID3D11Query> lowEnd;
        Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool advanced = false;
        bool particles = false;
        bool reduced = false;
        bool pending = false;
    };
    Microsoft::WRL::ComPtr<ID3D11VertexShader> mFogVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> mFogPs, mFogMsaaPs;
    struct FogLodShaders {
        Microsoft::WRL::ComPtr<ID3D11PixelShader> low, upscale;
        bool failed = false;
    };
    std::array<FogLodShaders, 2> mFogLodShaders{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> mFogCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mFogTemporalCb;
    struct FogHistoryTarget {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    };
    std::array<FogHistoryTarget, 2> mFogHistory{};
    FogHistoryTarget mFogReducedSun{};
    bool mFogReducedSunFailed = false;
    FogHistoryTarget mFogUniformCache{};
    Microsoft::WRL::ComPtr<ID3D11PixelShader> mFogUniformPs, mFogCachedUpscalePs;
    UINT mFogCacheWidth = 0, mFogCacheHeight = 0;
    bool mFogMsaaCacheFailed = false;
    FogTemporalConstants mFogLastCamera{};
    std::array<float, 4> mFogLastDepth{};
    float mFogLastFieldFraction = 1.0f;
    D3D11_VIEWPORT mFogLastViewport{};
    UINT mFogHistoryWidth = 0, mFogHistoryHeight = 0;
    UINT mFogLodWidth = 0, mFogLodHeight = 0;
    UINT mFogLastMsaa = 0, mFogReportedMsaa = 0;
    UINT mFogReportedLights = 0;
    bool mFogAtmosphereLogged = false;
    uint64_t mFogFrame = 0, mFogLastFrame = 0, mFogLastStream = 0;
    int mFogLastFramebuffer = -1, mFogLodMode = -1;
    unsigned mFogHistoryIndex = 0, mFogHistoryFrames = 0, mFogHistoryResets = 0, mFogReducedReuses = 0;
    bool mFogLodFailed = false;
    bool mFogMsaaFailed = false;
    Microsoft::WRL::ComPtr<ID3D11BlendState> mFogBlend;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> mFogRaster;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> mFogDepth;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mFogGridTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mFogGridView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mFogSupportTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mFogSupportView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mFogLiftTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mFogLiftView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mFogUpperTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mFogUpperView;
    std::array<std::array<float, 2>, FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE> mFogUpperLatest{};
    bool mFogUpperHasHistory = false, mFogUpperLogged = false;
    std::array<std::array<float, 2>, FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE> mFogLiftLatest{};
    bool mFogLiftCurrent = false, mFogLiftPrevious = false;
    bool mFogCpuField = false;
    float mFogDomainX = 0.0f, mFogDomainZ = 0.0f;
    float mFogStepX = 0.0f, mFogStepZ = 0.0f;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> mFogGridSampler;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mFogNoiseView;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> mFogNoiseSampler;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mFogFlowCS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mFogFlowCb, mFogFlowInput;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mFogFlowInputView;
    struct FogFlowTexture {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> target;
    };
    std::array<FogFlowTexture, 2> mFogFlowTextures{};
    std::array<FogFlowTexture, 2> mFogVerticalTextures{};
    FogFlowTexture mFogBlendedVertical{};
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mFogVerticalCS;
    bool mFogVerticalActive = false, mFogVerticalFailed = false, mFogVerticalLogged = false;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mFogParticleStepCS, mFogParticleDepositCS, mFogParticlePathsCS;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mFogParticleView;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> mFogParticleTarget;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mFogParticlePathsView;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> mFogParticlePathsTarget;
    std::array<std::pair<float, uint32_t>, FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE> mFogParticleTopology{};
    bool mFogParticlePathsReady = false;
    std::array<FogFlowTexture, 2> mFogParticleFields{};
    FogFlowTexture mFogBlendedField{};
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mFogFieldBlendCS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mFogFieldBlendCb;
    bool mFogCanBlend = false, mFogFieldBlendFailed = false, mFogBlendLogged = false;
    std::array<float, 2> mFogExtinction{};
    bool mFogExtinctionLogged = false;
    unsigned mFogParticleSteps = 0;
    bool mFogParticlesFailed = false, mFogParticlesActive = false;
    FogFlowCursor mFogFlowCursor{};
    unsigned mFogFlowIndex = 0;
    unsigned mFogFlowSteps = 0, mFogFlowSeeds = 0, mFogFlowReuses = 0;
    bool mFogFlowFailed = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mFogAuditStaging;
    Microsoft::WRL::ComPtr<ID3D11Query> mFogAuditReady;
    FogFlowCursor mFogAuditCursor{}, mFogAuditPrevious{};
    std::array<uint8_t, FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE> mFogAuditMask{};
    std::array<float, FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE> mFogAuditDensity{}, mFogAuditSpeed{};
    float mFogAuditRestDensity = 1.0f;
    bool mFogAuditPending = false, mFogAuditFailed = false;
    std::array<FogTiming, 4> mFogTiming{};
    std::array<double, 120> mFogSamples{};
    std::array<double, 120> mFogFieldSamples{}, mFogRaymarchSamples{};
    std::array<double, 120> mFogParticleSamples{};
    std::array<double, 120> mFogLowSamples{}, mFogUpscaleSamples{};
    size_t mFogSampleCount = 0;
    size_t mFogFlowSampleCount = 0;
    size_t mFogParticleSampleCount = 0;
    size_t mFogLodSampleCount = 0;
    unsigned mFogTimingReports = 0;
    bool mFogInitFailed = false;
    bool mFogGridLogged = false;
    void CreateDepthStencilObjects(uint32_t width, uint32_t height, uint32_t msaa_count, ID3D11DepthStencilView** view,
                                   ID3D11ShaderResourceView** srv);

    HMODULE mDX11Module;

    HMODULE mCompilerModule;
    pD3DCompile mD3dCompile;

    uint32_t mMsaaNumQualityLevels[D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT];

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> mRasterizerState;
    // SOH [Enhancement] Depth-stencil states, created lazily and cached for the device's lifetime (the
    // stencil features flip the mode many times per frame, and each flip used to re-run
    // CreateDepthStencilState). Key: depthTest | depthMask<<1 | zmodeDecal<<2 | stencilMode<<3 — same
    // scheme as the Metal backend's cache.
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> mDepthStencilStates[64];
    int mLastStencilMode = -1; // SOH [Enhancement] world light casting: cache tracker for mStencilMode
    Microsoft::WRL::ComPtr<ID3D11Buffer> mVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mPerFrameCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mPerDrawCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mPerPrimDepthCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mPerToonCb; // Wind Waker Style toon lighting (register b3)
    Microsoft::WRL::ComPtr<ID3D11Buffer> mCoordBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mCoordBufferSrv;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mDepthValueOutputBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mDepthValueOutputBufferCopy;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> mDepthValueOutputUav;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mComputeShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mComputeShaderMsaa;
    Microsoft::WRL::ComPtr<ID3DBlob> mComputeShaderMsaaBlob;
    size_t mCoordBufferSize;

#if DEBUG_D3D
    Microsoft::WRL::ComPtr<ID3D11Debug> debug;
#endif

    PerFrameCB mPerFrameCbData;
    PerDrawCB mPerDrawCbData;
    PerPrimDepthCB mPerPrimDepthCbData;
    PerToonCB mPerToonCbData; // Wind Waker Style toon lighting

    std::map<std::pair<uint64_t, uint32_t>, struct ShaderProgramD3D11> mShaderProgramPool;

    std::vector<struct TextureData> mTextures;
    int mCurrentTile;
    uint32_t mCurrentTextureIds[SHADER_MAX_TEXTURES] = {};

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

    // Cached staging texture for ReadFramebufferToCPU — avoids CreateTexture2D/Release per frame
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mReadbackStaging;
    uint32_t mReadbackStagingW = 0;
    uint32_t mReadbackStagingH = 0;
};

std::string gfx_direct3d_common_build_shader(size_t& numFloats, const CCFeatures& cc_features,
                                             bool include_root_signature, bool three_point_filtering, bool use_srgb);
} // namespace Fast
#endif
#endif
