#ifdef ENABLE_DX11

#include <cstdio>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <vector>
#include <cmath>

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
#include "fast/fog_volume.h"
#include "fog_field_blend_shader.h"
#include "fog_volume_shader.h"
#include "fog_flow_shader.h"
#include "fog_particle_shader.h"

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
    // SOH [Enhancement] World light casting: use a combined depth+stencil format on ALL feature levels
    // so the stencil light-volume technique always has a stencil plane. Link-Span: on 10_0+ keep the float
    // depth (D32_FLOAT_S8X24) so the depth test, and the image without stencil users, matches D32_FLOAT.
    const bool floatDepth = mFeatureLevel >= D3D_FEATURE_LEVEL_10_0;
    texture_desc.Format = floatDepth ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    texture_desc.SampleDesc.Count = msaa_count;
    texture_desc.SampleDesc.Quality = 0;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | (srv != nullptr ? D3D11_BIND_SHADER_RESOURCE : 0);
    texture_desc.CPUAccessFlags = 0;
    texture_desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> texture;
    ThrowIfFailed(mDevice->CreateTexture2D(&texture_desc, nullptr, texture.GetAddressOf()));

    D3D11_DEPTH_STENCIL_VIEW_DESC view_desc;
    // SOH [Enhancement] world light casting (depth+stencil)
    view_desc.Format = floatDepth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
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
        // SOH [Enhancement] World light casting: depth read view matching the depth+stencil texture
        // (the X8/X8X24 part is the stencil, ignored by GetPixelDepth's compute shader).
        srv_desc.Format = floatDepth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
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
        mLastStencilMode = -1; // SOH [Enhancement] world light casting / actor shadows
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
    // Matches the CPU mBufVbo allocation in interpreter.cpp (VBO_MAX_FLOATS_PER_VERTEX floats/vertex).
    vertex_buffer_desc.ByteWidth =
        256 * VBO_MAX_FLOATS_PER_VERTEX * 3 * sizeof(float); // Same as buf_vbo size in gfx_pc
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
    // Create the toon-lighting constant buffer (register b3), uploaded per toon draw.
    constant_buffer_desc.ByteWidth = sizeof(PerToonCB);
    ThrowIfFailed(mDevice->CreateBuffer(&constant_buffer_desc, nullptr, mPerToonCb.GetAddressOf()),
                  mWindowBackend->GetWindowHandle(), "Failed to create toon-lighting constant buffer.");

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
    // SOH [Enhancement] Toon lighting world-space normal (order must match the vbo packing).
    if (cc_features.opt_toon) {
        ied[ied_index++] = {
            "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0
        };
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
    prg->opt_toon = cc_features.opt_toon; // SOH [Enhancement] toon lighting
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

    // SOH [Enhancement] World light casting: also rebuild when the stencil mode changes. Link-Span: a decal-only change
    // must NOT rebuild the state, as upstream: the image without mods has to stay identical (fixing that stale compare
    // function is a separate renderer decision, not transport).
    if (mLastDepthTest != mCurrentDepthTest || mLastDepthMask != mCurrentDepthMask ||
        mLastStencilMode != mStencilMode) {
        mLastDepthTest = mCurrentDepthTest;
        mLastDepthMask = mCurrentDepthMask;
        mLastStencilMode = mStencilMode;

        // SOH [Enhancement] Depth-stencil states are cached (created lazily, kept for the device's
        // lifetime): the stencil features flip the mode many times per frame, and re-running
        // CreateDepthStencilState on every flip churned a driver object per change. Same key scheme as
        // the Metal backend's cache.
        uint32_t dsKey = (mCurrentDepthTest ? 1u : 0u) | ((mCurrentDepthMask ? 1u : 0u) << 1) |
                         ((mCurrentZmodeDecal ? 1u : 0u) << 2) | (((uint32_t)mStencilMode & 7u) << 3);
        if (mDepthStencilStates[dsKey] == nullptr) {
            D3D11_DEPTH_STENCIL_DESC depth_stencil_desc;
            ZeroMemory(&depth_stencil_desc, sizeof(D3D11_DEPTH_STENCIL_DESC));

            depth_stencil_desc.DepthEnable = mCurrentDepthTest || mCurrentDepthMask;
            depth_stencil_desc.DepthWriteMask =
                mCurrentDepthMask ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            depth_stencil_desc.DepthFunc =
                mCurrentDepthTest ? (mCurrentZmodeDecal ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_LESS)
                                  : D3D11_COMPARISON_ALWAYS;

            // SOH [Enhancement] World light casting / actor shadows stencil volume state. Off leaves the
            // stencil disabled (rendering unchanged). The single-sided mask modes (z-fail) use identical
            // front/back ops because those passes cull game-side; VolumeIncrDecr is the two-sided single
            // pass — opposite WRAP ops per facing (D3D11_STENCIL_OP_INCR/DECR wrap; the _SAT variants
            // clamp), order/polarity independent. The composite draws where stencil != 0 and zeroes it as
            // it goes (self-clearing).
            if (mStencilMode == (int)StencilMode::Off) {
                depth_stencil_desc.StencilEnable = false;
            } else {
                depth_stencil_desc.StencilEnable = true;
                depth_stencil_desc.StencilReadMask = 0xFF;
                depth_stencil_desc.StencilWriteMask = 0xFF;

                D3D11_DEPTH_STENCILOP_DESC op;
                op.StencilFailOp = D3D11_STENCIL_OP_KEEP;
                if (mStencilMode == (int)StencilMode::VolumeIncr) {
                    op.StencilDepthFailOp = D3D11_STENCIL_OP_INCR_SAT;
                    op.StencilPassOp = D3D11_STENCIL_OP_KEEP;
                    op.StencilFunc = D3D11_COMPARISON_ALWAYS;
                } else if (mStencilMode == (int)StencilMode::VolumeDecr) {
                    op.StencilDepthFailOp = D3D11_STENCIL_OP_DECR_SAT;
                    op.StencilPassOp = D3D11_STENCIL_OP_KEEP;
                    op.StencilFunc = D3D11_COMPARISON_ALWAYS;
                } else if (mStencilMode == (int)StencilMode::VolumeIncrDecr) {
                    op.StencilDepthFailOp = D3D11_STENCIL_OP_INCR; // wrap
                    op.StencilPassOp = D3D11_STENCIL_OP_KEEP;
                    op.StencilFunc = D3D11_COMPARISON_ALWAYS;
                } else { // Composite: draw where stencil != ref(0), zeroing it
                    op.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
                    op.StencilPassOp = D3D11_STENCIL_OP_ZERO;
                    op.StencilFunc = D3D11_COMPARISON_NOT_EQUAL;
                }
                depth_stencil_desc.FrontFace = op;
                depth_stencil_desc.BackFace = op;
                if (mStencilMode == (int)StencilMode::VolumeIncrDecr) {
                    depth_stencil_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR; // wrap
                }
            }

            ThrowIfFailed(
                mDevice->CreateDepthStencilState(&depth_stencil_desc, mDepthStencilStates[dsKey].GetAddressOf()));
        }
        mContext->OMSetDepthStencilState(mDepthStencilStates[dsKey].Get(), 0);
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
    // Toon lighting: per-object dominant light + ramp shape into the dedicated toon
    // CB (b2), re-uploaded per toon draw (WRITE_DISCARD makes this safe). Only the toon pixel shader
    // reads it, and PerFrameCB is left untouched so it stays frame-global.
    if (mShaderProgram->opt_toon) {
        for (int j = 0; j < 3; j++) {
            mPerToonCbData.toon_light_dir[j] = mToonLightDir[j];
            mPerToonCbData.toon_light_color[j] = mToonLightColor[j];
            mPerToonCbData.toon_ambient[j] = mToonAmbient[j];
        }
        mPerToonCbData.toon_ramp_center = mToonRampCenter;
        mPerToonCbData.toon_ramp_softness = mToonRampSoftness;
        mPerToonCbData.toon_highlight_intensity = mToonHighlightIntensity;
        mPerToonCbData.toon_shadow_intensity = mToonShadowIntensity;
        mPerToonCbData.toon_debug = mToonDebug;
        D3D11_MAPPED_SUBRESOURCE toon_ms;
        ZeroMemory(&toon_ms, sizeof(D3D11_MAPPED_SUBRESOURCE));
        mContext->Map(mPerToonCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &toon_ms);
        memcpy(toon_ms.pData, &mPerToonCbData, sizeof(PerToonCB));
        mContext->Unmap(mPerToonCb.Get(), 0);
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
    ++mFogFrame;
    // Set per-frame constant buffer
    // b2 remains reserved for Unbound primitive depth; b3 is the optional toon input.
    ID3D11Buffer* buffers[4] = { mPerFrameCb.Get(), mPerDrawCb.Get(), mPerPrimDepthCb.Get(), mPerToonCb.Get() };
    mContext->PSSetConstantBuffers(0, 4, buffers);

    mPerFrameCbData.noise_frame++;
    if (mPerFrameCbData.noise_frame > 150) {
        // No high values, as noise starts to look ugly
        mPerFrameCbData.noise_frame = 0;
    }
}

void GfxRenderingAPIDX11::EndFrame() {
    mContext->Flush();
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

namespace {
struct FogConstants {
    float parameters[9][4];
    float viewport[4];
    float support[4];
    float lighting[4];
    float atmosphere[4];
    float visual[4];
    FastFogLight lights[FAST_FOG_LIGHT_COUNT];
};
static_assert(offsetof(FastFogVolume, cells) == sizeof(FogConstants::parameters));

constexpr unsigned FogReportLimit = 4;

bool CreateFogStateTexture(ID3D11Device* device, ID3D11ShaderResourceView** view, ID3D11UnorderedAccessView** target) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = FAST_FOG_GRID_SIDE;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> texture;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf())) &&
           SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, view)) &&
           SUCCEEDED(device->CreateUnorderedAccessView(texture.Get(), nullptr, target));
}
} // namespace

bool GfxRenderingAPIDX11::CompileFogShader(const char* entry, const char* profile, ID3DBlob** blob, bool multisampled) {
    const D3D_SHADER_MACRO macros[]{{"FOG_MSAA", multisampled ? "1" : "0"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> errors;
    const auto result = mD3dCompile(FogVolumeShader, sizeof(FogVolumeShader) - 1, "FogVolume", macros, nullptr,
                                    entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, errors.GetAddressOf());
    if (FAILED(result)) {
        SPDLOG_WARN("Fast3D fog shader {} (MSAA={}) failed: {}", entry, multisampled,
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no diagnostics");
    }
    return SUCCEEDED(result);
}

bool GfxRenderingAPIDX11::InitFogVolume() {
    if (mFogInitFailed) return false;
    if (mFogPs) return true;
    ComPtr<ID3DBlob> vs, ps;
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(FogConstants);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    raster.MultisampleEnable = TRUE;

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;

    D3D11_TEXTURE2D_DESC field{};
    field.Width = field.Height = FAST_FOG_GRID_SIDE;
    field.MipLevels = field.ArraySize = 1;
    field.Format = DXGI_FORMAT_R32G32_FLOAT;
    field.SampleDesc.Count = 1;
    field.Usage = D3D11_USAGE_DEFAULT;
    field.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;

    if (!CompileFogShader("VSMain", "vs_4_0", vs.GetAddressOf()) ||
        !CompileFogShader("PSMain", "ps_4_0", ps.GetAddressOf()) ||
        FAILED(mDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, mFogVs.GetAddressOf())) ||
        FAILED(mDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, mFogPs.GetAddressOf())) ||
        FAILED(mDevice->CreateBuffer(&cb, nullptr, mFogCb.GetAddressOf())) ||
        FAILED(mDevice->CreateBlendState(&blend, mFogBlend.GetAddressOf())) ||
        FAILED(mDevice->CreateRasterizerState(&raster, mFogRaster.GetAddressOf())) ||
        FAILED(mDevice->CreateDepthStencilState(&depth, mFogDepth.GetAddressOf())) ||
        FAILED(mDevice->CreateTexture2D(&field, nullptr, mFogGridTexture.GetAddressOf())) ||
        FAILED(mDevice->CreateShaderResourceView(mFogGridTexture.Get(), nullptr, mFogGridView.GetAddressOf())) ||
        FAILED(mDevice->CreateSamplerState(&sampler, mFogGridSampler.GetAddressOf()))) {
        mFogInitFailed = true;
        SPDLOG_ERROR("Fast3D fog pass initialization failed");
        return false;
    }

    field.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (SUCCEEDED(mDevice->CreateTexture2D(&field, nullptr, mFogSupportTexture.GetAddressOf()))) {
        mDevice->CreateShaderResourceView(mFogSupportTexture.Get(), nullptr, mFogSupportView.GetAddressOf());
    }
    if (SUCCEEDED(mDevice->CreateTexture2D(&field, nullptr, mFogUpperTexture.GetAddressOf()))) {
        mDevice->CreateShaderResourceView(mFogUpperTexture.Get(), nullptr, mFogUpperView.GetAddressOf());
    }
    if (SUCCEEDED(mDevice->CreateTexture2D(&field, nullptr, mFogLiftTexture.GetAddressOf()))) {
        mDevice->CreateShaderResourceView(mFogLiftTexture.Get(), nullptr, mFogLiftView.GetAddressOf());
    }

    auto noise = field;
    noise.Width = noise.Height = Fast::FogNoiseSide;
    noise.Format = DXGI_FORMAT_R8_UNORM;
    noise.Usage = D3D11_USAGE_IMMUTABLE;
    const D3D11_SUBRESOURCE_DATA noiseData{Fast::FogNoise.data(), Fast::FogNoiseSide, 0};
    ComPtr<ID3D11Texture2D> noiseTexture;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    if (SUCCEEDED(mDevice->CreateTexture2D(&noise, &noiseData, noiseTexture.GetAddressOf())) &&
        SUCCEEDED(mDevice->CreateSamplerState(&sampler, mFogNoiseSampler.GetAddressOf()))) {
        mDevice->CreateShaderResourceView(noiseTexture.Get(), nullptr, mFogNoiseView.GetAddressOf());
    }

    for (auto& timing : mFogTiming) {
        const D3D11_QUERY_DESC disjoint{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        const D3D11_QUERY_DESC timestamp{ D3D11_QUERY_TIMESTAMP, 0 };
        if (FAILED(mDevice->CreateQuery(&disjoint, timing.disjoint.GetAddressOf())) ||
            FAILED(mDevice->CreateQuery(&timestamp, timing.begin.GetAddressOf())) ||
            FAILED(mDevice->CreateQuery(&timestamp, timing.fieldEnd.GetAddressOf())) ||
            FAILED(mDevice->CreateQuery(&timestamp, timing.particleBegin.GetAddressOf())) ||
            FAILED(mDevice->CreateQuery(&timestamp, timing.lowEnd.GetAddressOf())) ||
            FAILED(mDevice->CreateQuery(&timestamp, timing.end.GetAddressOf()))) {
            timing = {};
        }
    }
    SPDLOG_INFO("Fast3D fog pass initialized (DirectX 11)");
    return true;
}

void GfxRenderingAPIDX11::UpdateFogSupportRanges(const FastFogVolume& volume, const FastFogFlow& flow, UINT width,
                                                UINT height) {
    float ranges[FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE][4];
    for (UINT z = 0; z < height; ++z) {
        for (UINT x = 0; x < width; ++x) {
            const UINT nextX = std::min(x + 1, width - 1), nextZ = std::min(z + 1, height - 1);
            const UINT corners[]{z * width + x, z * width + nextX, nextZ * width + x, nextZ * width + nextX};
            auto& range = ranges[z * width + x];
            range[0] = range[2] = 1000001.0f;
            range[1] = range[3] = -1000001.0f;
            for (const UINT index : corners) {
                const auto& cell = flow.cells[index];
                if (!(cell.open & 1u)) continue;
                range[0] = std::min(range[0], cell.initial[1]);
                range[1] = std::max(range[1], cell.initial[1]);
                if (volume.upperCount && volume.upperCells[index][0] > 0.0001f) {
                    const float floor = volume.upperCells[index][1] / volume.upperCells[index][0];
                    range[2] = std::min(range[2], floor);
                    range[3] = std::max(range[3], floor);
                }
            }
        }
    }
    const D3D11_BOX rect{0, 0, 0, width, height, 1};
    mContext->UpdateSubresource(mFogSupportTexture.Get(), 0, &rect, ranges, width * sizeof(ranges[0]), 0);
}

void GfxRenderingAPIDX11::UpdateFogUpper(const FastFogVolume& volume, FogFlowAction action, UINT width, UINT height) {
    const bool blend = action == FogFlowAction::Advance && mFogUpperHasHistory;
    mFogUpperHasHistory = volume.upperCount != 0;
    if (!mFogUpperHasHistory) return;
    float values[FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE][4];
    for (UINT i = 0; i < width * height; ++i) {
        const auto& cell = volume.upperCells[i];
        auto& previous = mFogUpperLatest[i];
        values[i][0] = blend ? previous[0] : cell[0];
        values[i][1] = blend ? previous[1] : cell[1];
        values[i][2] = cell[0];
        values[i][3] = cell[1];
        previous = {cell[0], cell[1]};
    }
    const D3D11_BOX rect{0, 0, 0, width, height, 1};
    mContext->UpdateSubresource(mFogUpperTexture.Get(), 0, &rect, values, width * sizeof(values[0]), 0);
    if (!mFogUpperLogged) {
        mFogUpperLogged = true;
        SPDLOG_INFO("Fast3D fog stacked supports cells={}", volume.upperCount);
    }
}

void GfxRenderingAPIDX11::UpdateFogLift(const FastFogFlow& flow, FogFlowAction action, UINT width, UINT height) {
    mFogLiftPrevious = action == FogFlowAction::Advance && mFogLiftCurrent;
    mFogLiftCurrent = (flow.flags & FAST_FOG_FLOW_LIFT) && flow.maxRaisedFraction > 0.002f;
    if (!mFogLiftPrevious && !mFogLiftCurrent) return;
    float lift[FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE][4];
    for (UINT i = 0; i < width * height; ++i) {
        const float base = mFogLiftCurrent ? flow.raisedFraction[i] : 0.0f;
        const float upper = mFogLiftCurrent ? flow.upperRaisedFraction[i] : 0.0f;
        auto& previous = mFogLiftLatest[i];
        lift[i][0] = mFogLiftPrevious ? previous[0] : 0.0f;
        lift[i][1] = base;
        lift[i][2] = mFogLiftPrevious ? previous[1] : 0.0f;
        lift[i][3] = upper;
        previous = {base, upper};
    }
    const D3D11_BOX rect{0, 0, 0, width, height, 1};
    mContext->UpdateSubresource(mFogLiftTexture.Get(), 0, &rect, lift, width * sizeof(lift[0]), 0);
}

bool GfxRenderingAPIDX11::InitFogMsaa() {
    if (mFogMsaaFailed) return false;
    if (mFogMsaaPs) return true;
    ComPtr<ID3DBlob> bytecode;
    if (!CompileFogShader("PSMain", "ps_4_1", bytecode.GetAddressOf(), true) ||
        FAILED(mDevice->CreatePixelShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr,
                                         mFogMsaaPs.GetAddressOf()))) {
        mFogMsaaFailed = true;
        SPDLOG_WARN("Fast3D fog could not initialize the multisampled pass");
        return false;
    }
    return true;
}

static bool CreateFogColorTarget(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
                                 ID3D11RenderTargetView** target, ID3D11ShaderResourceView** view) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf())) &&
           SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, target)) &&
           SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, view));
}

bool GfxRenderingAPIDX11::InitFogLod(UINT width, UINT height, bool multisampled) {
    auto& shaders = mFogLodShaders[multisampled];
    if (mFogLodFailed || shaders.failed) return false;
    if (!shaders.upscale) {
        const auto compile = [&](const char* entry, const char* profile, ID3D11PixelShader** shader) {
            ComPtr<ID3DBlob> bytecode;
            return CompileFogShader(entry, profile, bytecode.GetAddressOf(), multisampled) &&
                   SUCCEEDED(mDevice->CreatePixelShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                                                       nullptr, shader));
        };
        if (!compile("PSLow", multisampled ? "ps_4_1" : "ps_4_0", shaders.low.GetAddressOf()) ||
            !compile("PSUpscale", "ps_5_0", shaders.upscale.GetAddressOf())) {
            shaders.failed = true;
            return false;
        }
    }
    if (!mFogTemporalCb) {
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(FogTemporalConstants);
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(mDevice->CreateBuffer(&cb, nullptr, mFogTemporalCb.GetAddressOf()))) {
            mFogLodFailed = true;
            return false;
        }
    }
    if (width == mFogHistoryWidth && height == mFogHistoryHeight) return true;

    std::array<FogHistoryTarget, 2> targets;
    for (auto& slot : targets) {
        if (!CreateFogColorTarget(mDevice.Get(), width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                  slot.target.GetAddressOf(), slot.view.GetAddressOf())) {
            SPDLOG_WARN("Fast3D fog could not allocate LOD textures {}x{}", width, height);
            mFogLodFailed = true;
            return false;
        }
    }
    mFogHistory = std::move(targets);
    mFogHistoryWidth = width;
    mFogHistoryHeight = height;
    return true;
}

bool GfxRenderingAPIDX11::InitFogMsaaCache(UINT width, UINT height) {
    if (mFogMsaaCacheFailed) return false;
    if (!mFogUniformPs) {
        const auto compile = [&](const char* entry, ID3D11PixelShader** shader) {
            ComPtr<ID3DBlob> bytecode;
            return CompileFogShader(entry, "ps_5_0", bytecode.GetAddressOf(), true) &&
                   SUCCEEDED(mDevice->CreatePixelShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                                                       nullptr, shader));
        };
        if (!compile("PSUniformFog", mFogUniformPs.GetAddressOf()) ||
            !compile("PSUpscaleCached", mFogCachedUpscalePs.GetAddressOf())) {
            mFogMsaaCacheFailed = true;
            return false;
        }
    }
    if (mFogCacheWidth == width && mFogCacheHeight == height) return true;
    FogHistoryTarget target;
    if (!CreateFogColorTarget(mDevice.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                              target.target.GetAddressOf(), target.view.GetAddressOf())) {
        mFogMsaaCacheFailed = true;
        return false;
    }
    mFogUniformCache = std::move(target);
    mFogCacheWidth = width;
    mFogCacheHeight = height;
    SPDLOG_INFO("Fast3D fog MSAA cache {}x{}", width, height);
    return true;
}

void GfxRenderingAPIDX11::DrawFogLod(const FastFogVolume& volume, const FastFogFlow& flow, FogFlowAction action,
                                    const D3D11_VIEWPORT& viewport, int mode, float fieldFraction, float extinction,
                                    ID3D11Query* lowEnd) {
    const auto& fb = mFrameBuffers[mCurrentFramebuffer];
    const auto& shaders = mFogLodShaders[fb.msaa_level > 1];
    const auto sameViewport = viewport.TopLeftX == mFogLastViewport.TopLeftX &&
                              viewport.TopLeftY == mFogLastViewport.TopLeftY &&
                              viewport.Width == mFogLastViewport.Width && viewport.Height == mFogLastViewport.Height &&
                              mCurrentFramebuffer == mFogLastFramebuffer;
    const auto square = [](float value) { return value * value; };
    const auto& old = mFogLastCamera;
    const float shift = square(volume.eye[0] - old.previousEye[0]) + square(volume.eye[1] - old.previousEye[1]) +
                        square(volume.eye[2] - old.previousEye[2]);
    const float facing = volume.forward[0] * old.previousForward[0] + volume.forward[1] * old.previousForward[1] +
                         volume.forward[2] * old.previousForward[2];
    const float upright = volume.up[0] * old.previousUp[0] + volume.up[1] * old.previousUp[1] +
                          volume.up[2] * old.previousUp[2];
    const bool sameDepth = std::equal(volume.depth, volume.depth + 4, mFogLastDepth.begin());
    const bool cameraCut = !sameDepth || shift > square(125.0f) || facing < 0.94f || upright < 0.94f ||
                           std::abs(volume.right[3] - old.previousRight[3]) > 0.13f ||
                           std::abs(volume.up[3] - old.previousUp[3]) > 0.13f;
    const char* reset = nullptr;
    if (!mFogLastFrame) reset = "initial";
    else if (mFogLastStream != flow.stream) reset = "scene";
    else if (!sameViewport) reset = "resize";
    else if (mFogLastMsaa != fb.msaa_level) reset = "msaa";
    else if (cameraCut) reset = "camera";
    else if (action == FogFlowAction::Seed || mFogFrame - mFogLastFrame > 1) reset = "gap";

    FogTemporalConstants constants = old;
    const bool sameCamera = std::equal(volume.eye, volume.eye + 3, old.previousEye) &&
                            std::equal(volume.forward, volume.forward + 3, old.previousForward) &&
                            std::equal(volume.right, volume.right + 4, old.previousRight) &&
                            std::equal(volume.up, volume.up + 4, old.previousUp);
    const bool sameField = action == FogFlowAction::Reuse && fieldFraction == mFogLastFieldFraction &&
                           extinction == old.previousForward[3];
    const bool reuseReduced = mode == 2 && !reset && sameCamera && sameField;
    constants.previousForward[3] = sameCamera ? 0.0f : 1.0f;
    constants.low[0] = static_cast<float>(mFogHistoryWidth);
    constants.low[1] = static_cast<float>(mFogHistoryHeight);
    constants.low[2] = 0.0f;
    constants.low[3] = 0.5f;
    if (mode == 2) {
        if (reset) {
            ++mFogHistoryResets;
            if (mFogHistoryResets <= 24) SPDLOG_INFO("Fast3D fog history reset reason={}", reset);
        } else if (!reuseReduced) {
            constexpr float offsets[]{ 0.28f, 0.72f, 0.43f, 0.57f };
            constants.low[2] = sameField ? 0.78f : 0.38f;
            constants.low[3] = offsets[mFogFrame % std::size(offsets)];
            ++mFogHistoryFrames;
        }
    }
    mContext->UpdateSubresource(mFogTemporalCb.Get(), 0, nullptr, &constants, 0, 0);
    mContext->PSSetConstantBuffers(1, 1, mFogTemporalCb.GetAddressOf());
    if (reuseReduced) {
        ++mFogReducedReuses;
    } else {
        ID3D11ShaderResourceView* previous = constants.low[2] > 0.0f
            ? mFogHistory[mFogHistoryIndex].view.Get() : nullptr;
        mContext->PSSetShaderResources(2, 1, &previous);
        mFogHistoryIndex ^= 1u;
        const D3D11_VIEWPORT reduced{ 0.0f, 0.0f, constants.low[0], constants.low[1], 0.0f, 1.0f };
        mContext->RSSetViewports(1, &reduced);
        mContext->OMSetRenderTargets(1, mFogHistory[mFogHistoryIndex].target.GetAddressOf(), nullptr);
        mContext->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        mContext->PSSetShader(shaders.low.Get(), nullptr, 0);
        mContext->Draw(3, 0);
        mContext->OMSetRenderTargets(1, fb.render_target_view.GetAddressOf(), nullptr);
        mContext->RSSetViewports(1, &viewport);
        mContext->OMSetBlendState(mFogBlend.Get(), nullptr, 0xffffffffu);
    }
    if (lowEnd) mContext->End(lowEnd);

    mContext->PSSetShaderResources(2, 1, mFogHistory[mFogHistoryIndex].view.GetAddressOf());
    const auto width = static_cast<UINT>(viewport.Width);
    const auto height = static_cast<UINT>(viewport.Height);
    const bool cache = fb.msaa_level >= 4 && viewport.Width == float(width) && viewport.Height == float(height) &&
                       viewport.TopLeftX == std::floor(viewport.TopLeftX) &&
                       viewport.TopLeftY == std::floor(viewport.TopLeftY) && InitFogMsaaCache(width, height);
    if (cache) {
        ComPtr<ID3D11ShaderResourceView> oldCache;
        mContext->PSGetShaderResources(5, 1, oldCache.GetAddressOf());
        const D3D11_VIEWPORT single{0.0f, 0.0f, viewport.Width, viewport.Height, 0.0f, 1.0f};
        mContext->RSSetViewports(1, &single);
        mContext->OMSetRenderTargets(1, mFogUniformCache.target.GetAddressOf(), nullptr);
        mContext->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        mContext->PSSetShader(mFogUniformPs.Get(), nullptr, 0);
        mContext->Draw(3, 0);
        mContext->OMSetRenderTargets(1, fb.render_target_view.GetAddressOf(), nullptr);
        mContext->RSSetViewports(1, &viewport);
        mContext->OMSetBlendState(mFogBlend.Get(), nullptr, 0xffffffffu);
        mContext->PSSetShaderResources(5, 1, mFogUniformCache.view.GetAddressOf());
        mContext->PSSetShader(mFogCachedUpscalePs.Get(), nullptr, 0);
        mContext->Draw(3, 0);
        mContext->PSSetShaderResources(5, 1, oldCache.GetAddressOf());
    } else {
        mContext->PSSetShader(shaders.upscale.Get(), nullptr, 0);
        mContext->Draw(3, 0);
    }

    mFogLastFrame = mode == 2 ? mFogFrame : 0;
    mFogLastStream = flow.stream;
    mFogLastViewport = viewport;
    mFogLastFramebuffer = mCurrentFramebuffer;
    mFogLastMsaa = fb.msaa_level;
    mFogLastFieldFraction = fieldFraction;
    std::copy_n(volume.eye, 4, mFogLastCamera.previousEye);
    std::copy_n(volume.forward, 4, mFogLastCamera.previousForward);
    mFogLastCamera.previousForward[3] = extinction;
    std::copy_n(volume.right, 4, mFogLastCamera.previousRight);
    std::copy_n(volume.up, 4, mFogLastCamera.previousUp);
    std::copy_n(volume.depth, 4, mFogLastDepth.begin());
}

bool GfxRenderingAPIDX11::InitFogFlow() {
    if (mFogFlowFailed || mDevice->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) return false;
    if (mFogFlowCS) return true;

    ComPtr<ID3DBlob> shader, errors;
    const auto compiled = mD3dCompile(FogFlowShader, sizeof(FogFlowShader) - 1, "FogFlow", nullptr, nullptr, "CSMain",
                                      "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shader.GetAddressOf(),
                                      errors.GetAddressOf());
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(FogFlowConstants);
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    constexpr UINT maxCells = FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE;
    static_assert(sizeof(FastFogFlowCell) == 22 * sizeof(float));
    D3D11_BUFFER_DESC input{};
    input.ByteWidth = maxCells * sizeof(FastFogFlowCell);
    input.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    input.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    input.StructureByteStride = sizeof(FastFogFlowCell);
    D3D11_SHADER_RESOURCE_VIEW_DESC inputView{};
    inputView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    inputView.Buffer.NumElements = maxCells;

    if (FAILED(compiled) ||
        FAILED(mDevice->CreateComputeShader(shader->GetBufferPointer(), shader->GetBufferSize(), nullptr,
                                           mFogFlowCS.GetAddressOf())) ||
        FAILED(mDevice->CreateBuffer(&cb, nullptr, mFogFlowCb.GetAddressOf())) ||
        FAILED(mDevice->CreateBuffer(&input, nullptr, mFogFlowInput.GetAddressOf())) ||
        FAILED(mDevice->CreateShaderResourceView(mFogFlowInput.Get(), &inputView, mFogFlowInputView.GetAddressOf())) ||
        !CreateFogStateTexture(mDevice.Get(), mFogFlowTextures[0].view.GetAddressOf(),
                               mFogFlowTextures[0].target.GetAddressOf()) ||
        !CreateFogStateTexture(mDevice.Get(), mFogFlowTextures[1].view.GetAddressOf(),
                               mFogFlowTextures[1].target.GetAddressOf())) {
        mFogFlowFailed = true;
        SPDLOG_WARN("Fast3D fog compute unavailable, using CPU field: {}",
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "resource initialization failed");
        return false;
    }
    return true;
}

bool GfxRenderingAPIDX11::InitFogVertical() {
    if (mFogVerticalFailed) return false;
    if (mFogVerticalCS) return true;
    const D3D_SHADER_MACRO macros[]{{"FOG_VERTICAL", "1"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> shader, errors;
    const auto compiled = mD3dCompile(FogFlowShader, sizeof(FogFlowShader) - 1, "FogVertical", macros, nullptr,
                                      "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                      shader.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(compiled) ||
        FAILED(mDevice->CreateComputeShader(shader->GetBufferPointer(), shader->GetBufferSize(), nullptr,
                                           mFogVerticalCS.GetAddressOf())) ||
        !CreateFogStateTexture(mDevice.Get(), mFogVerticalTextures[0].view.GetAddressOf(),
                               mFogVerticalTextures[0].target.GetAddressOf()) ||
        !CreateFogStateTexture(mDevice.Get(), mFogVerticalTextures[1].view.GetAddressOf(),
                               mFogVerticalTextures[1].target.GetAddressOf()) ||
        !CreateFogStateTexture(mDevice.Get(), mFogBlendedVertical.view.GetAddressOf(),
                               mFogBlendedVertical.target.GetAddressOf())) {
        mFogVerticalFailed = true;
        SPDLOG_WARN("Fast3D fog vertical compute unavailable, using CPU lift: {}",
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "resource initialization failed");
        return false;
    }
    return true;
}

bool GfxRenderingAPIDX11::InitFogParticles() {
    if (mFogParticlesFailed) return false;
    if (mFogParticleStepCS) return true;
    const auto compile = [&](const char* deposit, ID3D11ComputeShader** shader) {
        const D3D_SHADER_MACRO macros[]{{"FOG_PARTICLE_DEPOSIT", deposit}, {nullptr, nullptr}};
        ComPtr<ID3DBlob> blob, errors;
        const auto result = mD3dCompile(FogParticleShader, sizeof(FogParticleShader) - 1, "FogParticles", macros,
                                        nullptr, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                        blob.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(result)) {
            SPDLOG_WARN("Fast3D fog particle shader: {}",
                        errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
            return false;
        }
        return SUCCEEDED(mDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader));
    };
    D3D11_BUFFER_DESC state{};
    state.StructureByteStride = 8 * sizeof(float);
    state.ByteWidth = FAST_FOG_PARTICLE_COUNT * state.StructureByteStride;
    state.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    state.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    view.Buffer.NumElements = FAST_FOG_PARTICLE_COUNT;
    D3D11_UNORDERED_ACCESS_VIEW_DESC target{};
    target.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    target.Buffer.NumElements = FAST_FOG_PARTICLE_COUNT;
    auto paths = state;
    paths.StructureByteStride = 2 * sizeof(uint32_t);
    constexpr UINT pathCount = FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE;
    paths.ByteWidth = pathCount * paths.StructureByteStride;
    auto pathView = view;
    auto pathTarget = target;
    pathView.Buffer.NumElements = pathTarget.Buffer.NumElements = pathCount;
    ComPtr<ID3D11Buffer> buffer, pathBuffer;
    if (!compile("0", mFogParticleStepCS.GetAddressOf()) || !compile("1", mFogParticleDepositCS.GetAddressOf()) ||
        !compile("2", mFogParticlePathsCS.GetAddressOf()) ||
        FAILED(mDevice->CreateBuffer(&state, nullptr, buffer.GetAddressOf())) ||
        FAILED(mDevice->CreateShaderResourceView(buffer.Get(), &view, mFogParticleView.GetAddressOf())) ||
        FAILED(mDevice->CreateUnorderedAccessView(buffer.Get(), &target, mFogParticleTarget.GetAddressOf())) ||
        FAILED(mDevice->CreateBuffer(&paths, nullptr, pathBuffer.GetAddressOf())) ||
        FAILED(mDevice->CreateShaderResourceView(pathBuffer.Get(), &pathView, mFogParticlePathsView.GetAddressOf())) ||
        FAILED(mDevice->CreateUnorderedAccessView(pathBuffer.Get(), &pathTarget, mFogParticlePathsTarget.GetAddressOf())) ||
        !CreateFogStateTexture(mDevice.Get(), mFogParticleFields[0].view.GetAddressOf(),
                               mFogParticleFields[0].target.GetAddressOf()) ||
        !CreateFogStateTexture(mDevice.Get(), mFogParticleFields[1].view.GetAddressOf(),
                               mFogParticleFields[1].target.GetAddressOf())) {
        mFogParticlesFailed = true;
        SPDLOG_WARN("Fast3D fog particles unavailable, using unmodified GPU field");
        return false;
    }
    return true;
}

bool GfxRenderingAPIDX11::InitFogFieldBlend() {
    if (mFogFieldBlendFailed) return false;
    if (mFogFieldBlendCS) return true;
    ComPtr<ID3DBlob> shader, errors;
    const auto result = mD3dCompile(FogFieldBlendShader, sizeof(FogFieldBlendShader) - 1, "FogFieldBlend", nullptr,
                                    nullptr, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                    shader.GetAddressOf(), errors.GetAddressOf());
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(FogFieldBlendConstants);
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(result) ||
        FAILED(mDevice->CreateComputeShader(shader->GetBufferPointer(), shader->GetBufferSize(), nullptr,
                                           mFogFieldBlendCS.GetAddressOf())) ||
        FAILED(mDevice->CreateBuffer(&constants, nullptr, mFogFieldBlendCb.GetAddressOf())) ||
        !CreateFogStateTexture(mDevice.Get(), mFogBlendedField.view.GetAddressOf(),
                               mFogBlendedField.target.GetAddressOf())) {
        mFogFieldBlendFailed = true;
        SPDLOG_WARN("Fast3D fog interpolation unavailable: {}",
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "resource initialization failed");
        return false;
    }
    return true;
}

ID3D11ShaderResourceView* GfxRenderingAPIDX11::BlendFogField(ID3D11ShaderResourceView* current, UINT width, UINT height,
                                                            float fraction) {
    const auto& states = mFogParticlesActive ? mFogParticleFields : mFogFlowTextures;
    const FogFieldBlendConstants constants{width, height, fraction, uint32_t(mFogVerticalActive)};
    mContext->UpdateSubresource(mFogFieldBlendCb.Get(), 0, nullptr, &constants, 0, 0);

    ComPtr<ID3D11ComputeShader> oldShader;
    ComPtr<ID3D11Buffer> oldCb;
    ComPtr<ID3D11ShaderResourceView> oldPrevious, oldCurrent, oldVerticalPrevious, oldVerticalCurrent;
    ComPtr<ID3D11UnorderedAccessView> oldTarget, oldVerticalTarget;
    mContext->CSGetShader(oldShader.GetAddressOf(), nullptr, nullptr);
    mContext->CSGetConstantBuffers(0, 1, oldCb.GetAddressOf());
    mContext->CSGetShaderResources(0, 1, oldPrevious.GetAddressOf());
    mContext->CSGetShaderResources(1, 1, oldCurrent.GetAddressOf());
    mContext->CSGetUnorderedAccessViews(0, 1, oldTarget.GetAddressOf());
    if (mFogVerticalActive) {
        mContext->CSGetShaderResources(2, 1, oldVerticalPrevious.GetAddressOf());
        mContext->CSGetShaderResources(3, 1, oldVerticalCurrent.GetAddressOf());
        mContext->CSGetUnorderedAccessViews(1, 1, oldVerticalTarget.GetAddressOf());
    }

    ID3D11ShaderResourceView* inputs[]{states[mFogFlowIndex ^ 1u].view.Get(), current};
    mContext->CSSetShader(mFogFieldBlendCS.Get(), nullptr, 0);
    mContext->CSSetConstantBuffers(0, 1, mFogFieldBlendCb.GetAddressOf());
    mContext->CSSetShaderResources(0, 2, inputs);
    mContext->CSSetUnorderedAccessViews(0, 1, mFogBlendedField.target.GetAddressOf(), nullptr);
    if (mFogVerticalActive) {
        ID3D11ShaderResourceView* vertical[]{mFogVerticalTextures[mFogFlowIndex ^ 1u].view.Get(),
                                            mFogVerticalTextures[mFogFlowIndex].view.Get()};
        mContext->CSSetShaderResources(2, 2, vertical);
        mContext->CSSetUnorderedAccessViews(1, 1, mFogBlendedVertical.target.GetAddressOf(), nullptr);
    }
    mContext->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    if (mFogVerticalActive) {
        mContext->CSSetUnorderedAccessViews(1, 1, oldVerticalTarget.GetAddressOf(), nullptr);
        ID3D11ShaderResourceView* previous[]{oldVerticalPrevious.Get(), oldVerticalCurrent.Get()};
        mContext->CSSetShaderResources(2, 2, previous);
    }
    mContext->CSSetUnorderedAccessViews(0, 1, oldTarget.GetAddressOf(), nullptr);
    ID3D11ShaderResourceView* oldInputs[]{oldPrevious.Get(), oldCurrent.Get()};
    mContext->CSSetShaderResources(0, 2, oldInputs);
    mContext->CSSetConstantBuffers(0, 1, oldCb.GetAddressOf());
    mContext->CSSetShader(oldShader.Get(), nullptr, 0);
    if (!mFogBlendLogged) {
        mFogBlendLogged = true;
        SPDLOG_INFO("Fast3D fog interpolating field between gameplay ticks");
    }
    return mFogBlendedField.view.Get();
}

void GfxRenderingAPIDX11::AuditFogFlow(const FastFogVolume& volume, const FastFogFlow& flow, FogFlowCursor cursor,
                                      ID3D11ShaderResourceView* source) {
    if (mFogAuditFailed) return;
    if (mFogAuditPending) {
        const auto ready = mContext->GetData(mFogAuditReady.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (ready == S_FALSE) return;
        mFogAuditPending = false;
        if (FAILED(ready)) return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto result = mContext->Map(mFogAuditStaging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
            mFogAuditPending = true;
            return;
        }
        if (FAILED(result)) return;
        const auto& stamp = mFogAuditCursor;
        const bool contiguous = mFogAuditPrevious.Next(stamp) == FogFlowAction::Advance;
        unsigned cleared = 0, forced = 0, spread = 0, newFlow = 0, rim = 0;
        float fastest = 0.0f, transported = 0.0f, peakDensity = 0.0f;
        for (uint32_t z = 0; z < stamp.depth; ++z) {
            const auto* row = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + z * mapped.RowPitch);
            for (uint32_t x = 0; x < stamp.width; ++x) {
                const size_t i = z * stamp.width + x;
                const uint8_t mask = mFogAuditMask[i];
                if (mask == 0) continue;
                const float density = row[4 * x];
                const float speed = std::hypot(row[4 * x + 2], row[4 * x + 3]);
                if (density < 0.5f * mFogAuditRestDensity) ++cleared;
                if (mask == 2) ++forced;
                if (mask == 1 && density > 1.20f * mFogAuditRestDensity) ++rim;
                if (contiguous && mask == 1) {
                    const float loss = mFogAuditDensity[i] - density;
                    if (loss > 0.0005f) ++spread;
                    if (mFogAuditSpeed[i] < 0.05f && speed > 0.1f) ++newFlow;
                    transported = std::max(transported, loss);
                }
                fastest = std::max(fastest, speed);
                peakDensity = std::max(peakDensity, density);
                mFogAuditDensity[i] = density;
                mFogAuditSpeed[i] = speed;
            }
        }
        mContext->Unmap(mFogAuditStaging.Get(), 0);
        mFogAuditPrevious = stamp;
        SPDLOG_INFO("Fast3D fog audit stream={} tick={} clear={} forced={} spread={} new_flow={} peak={:.2f} loss={:.5f} "
                    "rim={} peak_rel={:.3f}", stamp.stream, stamp.tick, cleared, forced, spread, newFlow, fastest,
                    transported, rim, peakDensity / mFogAuditRestDensity);
    }
    if (!volume.audit) return;
    if (!mFogAuditStaging) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = FAST_FOG_GRID_SIDE;
        desc.ArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const D3D11_QUERY_DESC event{ D3D11_QUERY_EVENT, 0 };
        if (FAILED(mDevice->CreateTexture2D(&desc, nullptr, mFogAuditStaging.GetAddressOf())) ||
            FAILED(mDevice->CreateQuery(&event, mFogAuditReady.GetAddressOf()))) {
            mFogAuditFailed = true;
            SPDLOG_WARN("Fast3D fog audit readback unavailable");
            return;
        }
    }
    const auto hasImpulse = [](const FastFogImpulse& impulse) {
        return impulse.scale < 0.9999f || std::abs(impulse.vx) + std::abs(impulse.vz) > 0.001f;
    };
    for (uint32_t i = 0; i < cursor.width * cursor.depth; ++i) {
        const auto& cell = flow.cells[i];
        mFogAuditMask[i] = (cell.open & 1u) == 0 ? 0 : (hasImpulse(cell.before) || hasImpulse(cell.after) ? 2 : 1);
    }
    ComPtr<ID3D11Resource> texture;
    source->GetResource(texture.GetAddressOf());
    mContext->CopyResource(mFogAuditStaging.Get(), texture.Get());
    mContext->End(mFogAuditReady.Get());
    mFogAuditCursor = cursor;
    mFogAuditRestDensity = (flow.flags & FAST_FOG_FLOW_VORTICES) ? FAST_FOG_VORTEX_REST_DENSITY : 1.0f;
    mFogAuditPending = true;
}

ID3D11ShaderResourceView* GfxRenderingAPIDX11::UpdateFogFlow(const FastFogVolume& volume, const FastFogFlow& flow,
                                                           FogFlowAction& action, ID3D11Query* particleBegin,
                                                           bool& particles) {
    if (!InitFogFlow()) return nullptr;
    const auto width = static_cast<UINT>(volume.grid[0]);
    const auto depth = static_cast<UINT>(volume.grid[1]);
    const FogFlowCursor incoming{ flow.stream, flow.tick, width, depth };
    const auto tickAction = mFogFlowCursor.Next(incoming);
    action = tickAction;
    if (tickAction == FogFlowAction::Stale) return nullptr;
    const float originX = volume.minimum[0], originZ = volume.minimum[2];
    const float stepX = (volume.maximum[0] - originX) / (width - 1);
    const float stepZ = (volume.maximum[2] - originZ) / (depth - 1);
    const bool newStream = incoming.stream != mFogFlowCursor.stream;
    const bool sameSpacing = std::abs(stepX - mFogStepX) < 0.001f && std::abs(stepZ - mFogStepZ) < 0.001f;
    const bool domainChanged = !newStream && (originX != mFogDomainX || originZ != mFogDomainZ || !sameSpacing);
    const bool vortices = (flow.flags & FAST_FOG_FLOW_VORTICES) != 0;
    const bool cpuField = (flow.flags & FAST_FOG_FLOW_CPU_FIELD) != 0;
    const bool modeChanged = cpuField != mFogCpuField;
    const bool useVertical = !cpuField && vortices && (flow.flags & FAST_FOG_FLOW_VERTICAL_COMPUTE) && InitFogVertical();
    if (domainChanged || modeChanged || useVertical != mFogVerticalActive) action = FogFlowAction::Seed;
    if (action == FogFlowAction::Reuse) {
        ++mFogFlowReuses;
        return mFogParticlesActive ? mFogParticleFields[mFogFlowIndex].view.Get()
                                   : mFogFlowTextures[mFogFlowIndex].view.Get();
    }

    const bool seed = action == FogFlowAction::Seed;
    const bool useParticles = (flow.flags & FAST_FOG_FLOW_PARTICLES) && InitFogParticles();
    const float shiftX = (originX - mFogDomainX) / stepX;
    const float shiftZ = (originZ - mFogDomainZ) / stepZ;
    const bool translateParticles = domainChanged && sameSpacing && tickAction != FogFlowAction::Seed &&
                                    std::abs(shiftX) < width - 1 && std::abs(shiftZ) < depth - 1;
    const bool keepParticleModeChange = modeChanged && !domainChanged && !newStream && tickAction != FogFlowAction::Seed;
    const bool seedParticles = !mFogParticlesActive || (seed && !translateParticles && !keepParticleModeChange);
    // In CPU mode the seed kernel copies the submitted state. Particle and visual history
    // still follow the tick cursor, so they continue across consecutive snapshots.
    const bool copySnapshot = seed || cpuField;
    const FogFlowConstants constants{
        width, depth, uint32_t(copySnapshot), volume.grid[2], flow.dt, stepX, stepZ,
        std::exp(-(vortices ? 1.35f : 3.5f) * flow.dt),
        flow.tick, FAST_FOG_PARTICLE_COUNT, vortices ? FAST_FOG_VORTEX_REST_DENSITY : 1.0f, 0.30f,
        translateParticles ? shiftX : 0.0f, translateParticles ? shiftZ : 0.0f,
        uint32_t(seedParticles), tickAction == FogFlowAction::Reuse ? 0.0f : flow.dt,
    };
    const D3D11_BOX span{ 0, 0, 0, width * depth * static_cast<UINT>(sizeof(FastFogFlowCell)), 1, 1 };
    mContext->UpdateSubresource(mFogFlowInput.Get(), 0, &span, flow.cells, 0, 0);
    mContext->UpdateSubresource(mFogFlowCb.Get(), 0, nullptr, &constants, 0, 0);

    ComPtr<ID3D11ComputeShader> oldShader;
    ComPtr<ID3D11Buffer> oldCb;
    ComPtr<ID3D11ShaderResourceView> oldPrevious, oldInput, oldTopology, oldPaths;
    ComPtr<ID3D11UnorderedAccessView> oldTarget, oldVerticalTarget;
    mContext->CSGetShader(oldShader.GetAddressOf(), nullptr, nullptr);
    mContext->CSGetConstantBuffers(0, 1, oldCb.GetAddressOf());
    mContext->CSGetShaderResources(0, 1, oldPrevious.GetAddressOf());
    mContext->CSGetShaderResources(1, 1, oldInput.GetAddressOf());
    if (useVertical || useParticles) mContext->CSGetShaderResources(2, 1, oldTopology.GetAddressOf());
    if (useParticles) mContext->CSGetShaderResources(3, 1, oldPaths.GetAddressOf());
    mContext->CSGetUnorderedAccessViews(0, 1, oldTarget.GetAddressOf());
    if (useVertical) mContext->CSGetUnorderedAccessViews(1, 1, oldVerticalTarget.GetAddressOf());

    auto& output = mFogFlowTextures[mFogFlowIndex ^ 1u];
    auto& particleOutput = mFogParticleFields[mFogFlowIndex ^ 1u];
    ID3D11ShaderResourceView* inputs[] = {
        copySnapshot ? nullptr : mFogFlowTextures[mFogFlowIndex].view.Get(), mFogFlowInputView.Get(),
    };
    mContext->CSSetShader(useVertical ? mFogVerticalCS.Get() : mFogFlowCS.Get(), nullptr, 0);
    mContext->CSSetConstantBuffers(0, 1, mFogFlowCb.GetAddressOf());
    mContext->CSSetShaderResources(0, 2, inputs);
    mContext->CSSetUnorderedAccessViews(0, 1, output.target.GetAddressOf(), nullptr);
    if (useVertical) {
        ID3D11ShaderResourceView* previous = seed ? nullptr : mFogVerticalTextures[mFogFlowIndex].view.Get();
        mContext->CSSetShaderResources(2, 1, &previous);
        mContext->CSSetUnorderedAccessViews(1, 1, mFogVerticalTextures[mFogFlowIndex ^ 1u].target.GetAddressOf(), nullptr);
    }
    mContext->Dispatch((width + 7) / 8, (depth + 7) / 8, 1);
    if (useVertical) mContext->CSSetUnorderedAccessViews(1, 1, oldVerticalTarget.GetAddressOf(), nullptr);

    ID3D11ShaderResourceView* emptyInputs[2]{};
    mContext->CSSetShaderResources(0, 2, emptyInputs);
    if (useParticles) {
        if (particleBegin) mContext->End(particleBegin);
        mContext->CSSetShaderResources(2, 1, mFogFlowInputView.GetAddressOf());
        bool topologyChanged = seed || !mFogParticlePathsReady;
        for (UINT i = 0; i < width * depth; ++i) {
            const auto state = std::pair{flow.cells[i].initial[1], flow.cells[i].open};
            topologyChanged |= state != mFogParticleTopology[i];
            mFogParticleTopology[i] = state;
        }
        if (topologyChanged) {
            mContext->CSSetUnorderedAccessViews(0, 1, mFogParticlePathsTarget.GetAddressOf(), nullptr);
            mContext->CSSetShader(mFogParticlePathsCS.Get(), nullptr, 0);
            mContext->Dispatch((width + 7) / 8, (depth + 7) / 8, 1);
            mFogParticlePathsReady = true;
        }
        mContext->CSSetUnorderedAccessViews(0, 1, mFogParticleTarget.GetAddressOf(), nullptr);
        mContext->CSSetShaderResources(0, 1, output.view.GetAddressOf());
        mContext->CSSetShader(mFogParticleStepCS.Get(), nullptr, 0);
        mContext->Dispatch((FAST_FOG_PARTICLE_COUNT + 31) / 32, 1, 1);
        mContext->CSSetUnorderedAccessViews(0, 1, particleOutput.target.GetAddressOf(), nullptr);
        mContext->CSSetShaderResources(1, 1, mFogParticleView.GetAddressOf());
        mContext->CSSetShaderResources(3, 1, mFogParticlePathsView.GetAddressOf());
        mContext->CSSetShader(mFogParticleDepositCS.Get(), nullptr, 0);
        mContext->Dispatch((width + 7) / 8, (depth + 7) / 8, 1);
        mContext->CSSetShaderResources(0, 2, emptyInputs);
        if (++mFogParticleSteps == 1) {
            SPDLOG_INFO("Fast3D fog particles advancing count={} collision=faces path_cache=true", FAST_FOG_PARTICLE_COUNT);
        }
        particles = true;
    }
    mContext->CSSetUnorderedAccessViews(0, 1, oldTarget.GetAddressOf(), nullptr);
    if (useVertical || useParticles) mContext->CSSetShaderResources(2, 1, oldTopology.GetAddressOf());
    if (useParticles) mContext->CSSetShaderResources(3, 1, oldPaths.GetAddressOf());
    ID3D11ShaderResourceView* oldInputs[]{oldPrevious.Get(), oldInput.Get()};
    mContext->CSSetShaderResources(0, 2, oldInputs);
    mContext->CSSetConstantBuffers(0, 1, oldCb.GetAddressOf());
    mContext->CSSetShader(oldShader.Get(), nullptr, 0);
    if (volume.audit || mFogAuditPending) AuditFogFlow(volume, flow, incoming, output.view.Get());
    mFogFlowIndex ^= 1u;
    mFogExtinction[mFogFlowIndex] = volume.forward[3];
    mFogFlowCursor = incoming;
    mFogCanBlend = !seed && useParticles == mFogParticlesActive && useVertical == mFogVerticalActive;
    mFogParticlesActive = useParticles;
    mFogVerticalActive = useVertical;
    if (cpuField && modeChanged) SPDLOG_INFO("Fast3D fog using CPU field snapshots {}x{}", width, depth);
    if (keepParticleModeChange) {
        SPDLOG_INFO("Fast3D fog field source={} stream={} tick={} keep_particles={}", cpuField ? "CPU" : "GPU",
                    incoming.stream, incoming.tick, useParticles && !seedParticles);
    }
    mFogCpuField = cpuField;
    if (useVertical && !seed && !mFogVerticalLogged) {
        mFogVerticalLogged = true;
        SPDLOG_INFO("Fast3D fog vertical compute advancing {}x{}", width, depth);
    }
    mFogDomainX = originX;
    mFogDomainZ = originZ;
    mFogStepX = stepX;
    mFogStepZ = stepZ;

    if (seed) {
        ++mFogFlowSeeds;
        if (domainChanged) {
            SPDLOG_INFO("Fast3D fog grid shift cells=({:.2f},{:.2f}) stream={} tick={} keep_particles={}",
                        shiftX, shiftZ, incoming.stream, incoming.tick, useParticles && !seedParticles);
        } else if (!cpuField && (newStream || incoming.tick % 120 == 0)) {
            SPDLOG_INFO("Fast3D fog compute seed {}x{} stream={} tick={}", width, depth, incoming.stream, incoming.tick);
        }
    } else if (!cpuField && ++mFogFlowSteps == 1) {
        SPDLOG_INFO("Fast3D fog compute advancing {}x{} solver={}", width, depth, vortices ? "vortices" : "classic");
    }
    return useParticles ? particleOutput.view.Get() : output.view.Get();
}

void GfxRenderingAPIDX11::CollectFogTiming() {
    for (auto& timing : mFogTiming) {
        if (!timing.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
        uint64_t first = 0, field = 0, last = 0, particle = 0, lowEnd = 0;
        constexpr UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
        const HRESULT clockStatus = mContext->GetData(timing.disjoint.Get(), &clock, sizeof(clock), flags);
        const HRESULT beginStatus = mContext->GetData(timing.begin.Get(), &first, sizeof(first), flags);
        const HRESULT fieldStatus = mContext->GetData(timing.fieldEnd.Get(), &field, sizeof(field), flags);
        const HRESULT particleStatus = timing.particles
            ? mContext->GetData(timing.particleBegin.Get(), &particle, sizeof(particle), flags) : S_OK;
        const HRESULT lowStatus = timing.reduced
            ? mContext->GetData(timing.lowEnd.Get(), &lowEnd, sizeof(lowEnd), flags) : S_OK;
        const HRESULT endStatus = mContext->GetData(timing.end.Get(), &last, sizeof(last), flags);
        if (clockStatus == S_FALSE || beginStatus == S_FALSE || fieldStatus == S_FALSE || particleStatus == S_FALSE ||
            lowStatus == S_FALSE ||
            endStatus == S_FALSE) continue;
        timing.pending = false;
        if (FAILED(clockStatus) || FAILED(beginStatus) || FAILED(endStatus) || clock.Disjoint || !clock.Frequency ||
            last < first || mFogTimingReports >= FogReportLimit) {
            continue;
        }
        mFogSamples[mFogSampleCount++] = static_cast<double>(last - first) * 1000.0 / clock.Frequency;
        if (timing.reduced && SUCCEEDED(fieldStatus) && SUCCEEDED(lowStatus) && field >= first &&
            lowEnd >= field && last >= lowEnd) {
            const double milliseconds = 1000.0 / clock.Frequency;
            mFogLowSamples[mFogLodSampleCount] = (lowEnd - field) * milliseconds;
            mFogUpscaleSamples[mFogLodSampleCount++] = (last - lowEnd) * milliseconds;
        }
        if (timing.advanced && SUCCEEDED(fieldStatus) && field >= first && last >= field) {
            const double milliseconds = 1000.0 / clock.Frequency;
            const bool particleMeasured = timing.particles && SUCCEEDED(particleStatus) &&
                                          particle >= first && particle <= field;
            if (particleMeasured) mFogParticleSamples[mFogParticleSampleCount++] = (field - particle) * milliseconds;
            mFogFieldSamples[mFogFlowSampleCount] = ((particleMeasured ? particle : field) - first) * milliseconds;
            mFogRaymarchSamples[mFogFlowSampleCount++] = (last - field) * milliseconds;
        }
        if (mFogSampleCount == mFogSamples.size()) {
            std::sort(mFogSamples.begin(), mFogSamples.end());
            const auto middle = mFogSamples.size() / 2;
            const auto p95 = mFogSamples.size() * 95 / 100 - 1;
            SPDLOG_INFO("Fast3D fog GPU n={} p50_ms={:.4f} p95_ms={:.4f}", mFogSamples.size(),
                        (mFogSamples[middle - 1] + mFogSamples[middle]) * 0.5, mFogSamples[p95]);
            if (mFogLodMode >= 0) {
                SPDLOG_INFO("Fast3D fog LOD timing mode={} full={}x{} history_frames={} resets={} cached_frames={}",
                            mFogLodMode, mFogLodWidth, mFogLodHeight, mFogHistoryFrames, mFogHistoryResets,
                            mFogReducedReuses);
            }
            if (mFogLodSampleCount) {
                const auto count = mFogLodSampleCount;
                std::sort(mFogLowSamples.begin(), mFogLowSamples.begin() + count);
                std::sort(mFogUpscaleSamples.begin(), mFogUpscaleSamples.begin() + count);
                SPDLOG_INFO("Fast3D fog LOD GPU n={} march_p50_ms={:.4f} upscale_p50_ms={:.4f}", count,
                            mFogLowSamples[count / 2], mFogUpscaleSamples[count / 2]);
            }
            if (mFogFlowSampleCount) {
                const auto count = mFogFlowSampleCount;
                std::sort(mFogFieldSamples.begin(), mFogFieldSamples.begin() + count);
                std::sort(mFogRaymarchSamples.begin(), mFogRaymarchSamples.begin() + count);
                const auto percentile95 = (count - 1) * 95 / 100;
                SPDLOG_INFO("Fast3D fog compute GPU n={} field_p50_ms={:.4f} field_p95_ms={:.4f} "
                            "raymarch_p50_ms={:.4f} raymarch_p95_ms={:.4f} steps={} seeds={} reuses={}",
                            count, mFogFieldSamples[count / 2], mFogFieldSamples[percentile95],
                            mFogRaymarchSamples[count / 2], mFogRaymarchSamples[percentile95], mFogFlowSteps,
                            mFogFlowSeeds, mFogFlowReuses);
            }
            if (mFogParticleSampleCount) {
                const auto count = mFogParticleSampleCount;
                std::sort(mFogParticleSamples.begin(), mFogParticleSamples.begin() + count);
                SPDLOG_INFO("Fast3D fog particles GPU n={} p50_ms={:.4f} p95_ms={:.4f}", count,
                            mFogParticleSamples[count / 2], mFogParticleSamples[(count - 1) * 95 / 100]);
            }
            mFogSampleCount = 0;
            mFogFlowSampleCount = 0;
            mFogParticleSampleCount = 0;
            mFogLodSampleCount = 0;
            ++mFogTimingReports;
        }
    }
}

void GfxRenderingAPIDX11::DrawFogVolume(const FastFogVolume& volume, const FastFogFlow* flow) {
    const auto& fb = mFrameBuffers[mCurrentFramebuffer];
    if (!fb.depth_stencil_srv || !fb.render_target_view) return;
    const bool multisampled = fb.msaa_level > 1;
    D3D11_VIEWPORT viewport{};
    UINT count = 1;
    mContext->RSGetViewports(&count, &viewport);
    if (!count || viewport.Width <= 0.0f || viewport.Height <= 0.0f || !InitFogVolume()) return;
    if (multisampled && !InitFogMsaa()) return;
    if (fb.msaa_level != mFogReportedMsaa) {
        mFogReportedMsaa = fb.msaa_level;
        SPDLOG_INFO("Fast3D fog MSAA samples={}", mFogReportedMsaa);
    }

    CollectFogTiming();
    FogTiming* measurement = nullptr;
    if (volume.eye[3] == 1.0f && mFogTimingReports < FogReportLimit) {
        for (auto& timing : mFogTiming) {
            if (timing.disjoint && !timing.pending) {
                measurement = &timing;
                mContext->Begin(timing.disjoint.Get());
                mContext->End(timing.begin.Get());
                break;
            }
        }
    }

    FogConstants constants{ {}, { viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height } };
    std::memcpy(constants.parameters, &volume, sizeof(constants.parameters));
    const auto lightCount = std::min<uint32_t>(volume.lightCount, FAST_FOG_LIGHT_COUNT);
    constants.lighting[0] = static_cast<float>(lightCount);
    std::copy_n(volume.lights, lightCount, constants.lights);
    std::copy_n(volume.atmosphere, 4, constants.atmosphere);
    if (volume.atmosphere[3] > 0.0f && !mFogAtmosphereLogged) {
        mFogAtmosphereLogged = true;
        SPDLOG_INFO("Fast3D fog distant atmosphere rgb=({:.3f},{:.3f},{:.3f})", volume.atmosphere[0],
                    volume.atmosphere[1], volume.atmosphere[2]);
    }
    if (lightCount > mFogReportedLights) {
        mFogReportedLights = lightCount;
        SPDLOG_INFO("Fast3D fog local lights={}", lightCount);
    }
    auto action = FogFlowAction::Stale;
    bool particles = false;
    auto* fieldView = flow ? UpdateFogFlow(volume, *flow, action,
                                           measurement ? measurement->particleBegin.Get() : nullptr, particles)
                           : nullptr;
    const auto fieldWidth = static_cast<UINT>(volume.grid[0]);
    const auto fieldHeight = static_cast<UINT>(volume.grid[1]);
    const bool liveFlow = fieldView && action != FogFlowAction::Stale;
    float fieldFraction = 1.0f;
    if (liveFlow && mFogCanBlend && volume.interpolation < 1.0f && InitFogFieldBlend()) {
        fieldFraction = std::clamp(volume.interpolation, 0.0f, 1.0f);
        fieldView = BlendFogField(fieldView, fieldWidth, fieldHeight, fieldFraction);
    }
    float extinction = volume.forward[3];
    if (fieldFraction < 1.0f) {
        const float previous = mFogExtinction[mFogFlowIndex ^ 1u];
        const float current = mFogExtinction[mFogFlowIndex];
        extinction = previous + (current - previous) * fieldFraction;
        if (!mFogExtinctionLogged && fieldFraction > 0.0f && previous != current) {
            mFogExtinctionLogged = true;
            SPDLOG_INFO("Fast3D fog fade interpolating {:.6f}->{:.6f} fraction={:.3f} extinction={:.6f}",
                        previous, current, fieldFraction, extinction);
        }
    }
    constants.parameters[1][3] = extinction;
    const bool useDetail = liveFlow && (flow->flags & FAST_FOG_FLOW_VORTICES);
    if (useDetail) {
        const float phase = (float(flow->tick % 2048u) + fieldFraction) * (6.2831853f / 2048.0f);
        constants.parameters[7][3] = 1.0f;
        if (mFogNoiseView) {
            constants.visual[1] = 1.0f;
            constants.visual[2] = 2.25f * std::cos(phase);
            constants.visual[3] = 2.25f * std::sin(phase);
        }
    }
    const bool useNoise = constants.visual[1] > 0.0f;
    const bool useSupport = liveFlow && mFogSupportView &&
                            std::max(volume.maximum[0] - volume.minimum[0], volume.maximum[2] - volume.minimum[2]) >
                                1024.0f;
    if (useSupport && action != FogFlowAction::Reuse) {
        UpdateFogSupportRanges(volume, *flow, fieldWidth, fieldHeight);
    }
    const bool useUpper = liveFlow && mFogUpperView && volume.upperCount != 0;
    if (liveFlow && mFogUpperView && action != FogFlowAction::Reuse) {
        UpdateFogUpper(volume, action, fieldWidth, fieldHeight);
    }
    if (liveFlow && mFogLiftView && action != FogFlowAction::Reuse) {
        UpdateFogLift(*flow, action, fieldWidth, fieldHeight);
    }
    const bool useLift = liveFlow && mFogLiftView && (flow->flags & FAST_FOG_FLOW_LIFT) &&
                         (mFogLiftCurrent || (fieldFraction < 1.0f && mFogLiftPrevious));
    auto* gpuLift = useLift && mFogVerticalActive
        ? (fieldFraction < 1.0f ? mFogBlendedVertical.view.Get() : mFogVerticalTextures[mFogFlowIndex].view.Get())
        : nullptr;
    constants.support[0] = useSupport ? 1.0f : 0.0f;
    constants.support[2] = fieldFraction;
    constants.support[3] = useUpper ? 1.0f : 0.0f;
    constants.visual[0] = gpuLift ? 1.0f : 0.0f;
    if (useLift) {
        const float liftOffset = 0.65f * volume.grid[2];
        constants.support[1] = liftOffset;
        constants.parameters[6][1] += liftOffset;
    }
    mContext->UpdateSubresource(mFogCb.Get(), 0, nullptr, &constants, 0, 0);
    if (!fieldView && volume.grid[0] >= 2.0f) {
        const auto width = static_cast<UINT>(volume.grid[0]);
        const auto height = static_cast<UINT>(volume.grid[1]);
        const D3D11_BOX rect{ 0, 0, 0, width, height, 1 };
        mContext->UpdateSubresource(mFogGridTexture.Get(), 0, &rect, volume.cells, width * sizeof(volume.cells[0]), 0);
        fieldView = mFogGridView.Get();
        if (!mFogGridLogged) {
            SPDLOG_INFO("Fast3D fog grid uploaded {}x{}", width, height);
            mFogGridLogged = true;
        }
    }
    const UINT width = static_cast<UINT>(std::ceil(viewport.Width));
    const UINT height = static_cast<UINT>(std::ceil(viewport.Height));
    int lod = 0;
    if (flow && action != FogFlowAction::Stale && volume.eye[3] == 1.0f &&
        (volume.grid[3] == 1.0f || volume.grid[3] == 2.0f) && width >= 320 && height >= 240 &&
        InitFogLod((width + 1) / 2, (height + 1) / 2, multisampled)) {
        lod = static_cast<int>(volume.grid[3]);
    }
    if (lod != mFogLodMode || width != mFogLodWidth || height != mFogLodHeight) {
        SPDLOG_INFO("Fast3D fog LOD mode={} full={}x{} low={}x{}", lod, width, height,
                    lod ? mFogHistoryWidth : width, lod ? mFogHistoryHeight : height);
        mFogLodMode = lod;
        mFogLodWidth = width;
        mFogLodHeight = height;
    }
    if (measurement) {
        measurement->advanced = action == FogFlowAction::Advance;
        measurement->particles = particles;
        measurement->reduced = lod != 0;
        mContext->End(measurement->fieldEnd.Get());
    }

    // This is a direct backend draw. Restore bindings so interpreter and backend caches stay valid.
    ComPtr<ID3D11VertexShader> oldVs;
    ComPtr<ID3D11PixelShader> oldPs;
    ComPtr<ID3D11InputLayout> oldLayout;
    ComPtr<ID3D11Buffer> oldCb;
    ComPtr<ID3D11Buffer> oldTemporalCb;
    ComPtr<ID3D11ShaderResourceView> oldSrv;
    ComPtr<ID3D11ShaderResourceView> oldField;
    ComPtr<ID3D11ShaderResourceView> oldReduced;
    ComPtr<ID3D11ShaderResourceView> oldSupport;
    ComPtr<ID3D11ShaderResourceView> oldLift;
    ComPtr<ID3D11ShaderResourceView> oldUpper;
    ComPtr<ID3D11ShaderResourceView> oldGpuLift;
    ComPtr<ID3D11ShaderResourceView> oldNoise;
    ComPtr<ID3D11SamplerState> oldFogSampler, oldNoiseSampler;
    ComPtr<ID3D11RasterizerState> oldRaster;
    ComPtr<ID3D11BlendState> oldBlend;
    ComPtr<ID3D11DepthStencilState> oldDepth;
    D3D11_PRIMITIVE_TOPOLOGY oldTopology{};
    UINT stencilRef = 0, sampleMask = 0;
    FLOAT blendFactor[4]{};
    mContext->VSGetShader(oldVs.GetAddressOf(), nullptr, nullptr);
    mContext->PSGetShader(oldPs.GetAddressOf(), nullptr, nullptr);
    mContext->PSGetConstantBuffers(0, 1, oldCb.GetAddressOf());
    mContext->PSGetShaderResources(0, 1, oldSrv.GetAddressOf());
    mContext->PSGetShaderResources(1, 1, oldField.GetAddressOf());
    if (useSupport) mContext->PSGetShaderResources(3, 1, oldSupport.GetAddressOf());
    if (useLift) mContext->PSGetShaderResources(4, 1, oldLift.GetAddressOf());
    if (useUpper) mContext->PSGetShaderResources(6, 1, oldUpper.GetAddressOf());
    if (gpuLift) mContext->PSGetShaderResources(7, 1, oldGpuLift.GetAddressOf());
    if (useNoise) {
        mContext->PSGetShaderResources(8, 1, oldNoise.GetAddressOf());
        mContext->PSGetSamplers(2, 1, oldNoiseSampler.GetAddressOf());
    }
    if (lod) {
        mContext->PSGetConstantBuffers(1, 1, oldTemporalCb.GetAddressOf());
        mContext->PSGetShaderResources(2, 1, oldReduced.GetAddressOf());
    }
    mContext->PSGetSamplers(1, 1, oldFogSampler.GetAddressOf());
    mContext->IAGetInputLayout(oldLayout.GetAddressOf());
    mContext->IAGetPrimitiveTopology(&oldTopology);
    mContext->RSGetState(oldRaster.GetAddressOf());
    mContext->OMGetBlendState(oldBlend.GetAddressOf(), blendFactor, &sampleMask);
    mContext->OMGetDepthStencilState(oldDepth.GetAddressOf(), &stencilRef);

    mContext->OMSetRenderTargets(1, fb.render_target_view.GetAddressOf(), nullptr);
    mContext->OMSetDepthStencilState(mFogDepth.Get(), 0);
    mContext->OMSetBlendState(mFogBlend.Get(), nullptr, 0xffffffffu);
    mContext->RSSetState(mFogRaster.Get());
    mContext->IASetInputLayout(nullptr);
    mContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mContext->VSSetShader(mFogVs.Get(), nullptr, 0);
    mContext->PSSetConstantBuffers(0, 1, mFogCb.GetAddressOf());
    mContext->PSSetShaderResources(0, 1, fb.depth_stencil_srv.GetAddressOf());
    mContext->PSSetShaderResources(1, 1, &fieldView);
    if (useSupport) mContext->PSSetShaderResources(3, 1, mFogSupportView.GetAddressOf());
    if (useLift) mContext->PSSetShaderResources(4, 1, mFogLiftView.GetAddressOf());
    if (useUpper) mContext->PSSetShaderResources(6, 1, mFogUpperView.GetAddressOf());
    if (gpuLift) mContext->PSSetShaderResources(7, 1, &gpuLift);
    if (useNoise) {
        mContext->PSSetShaderResources(8, 1, mFogNoiseView.GetAddressOf());
        mContext->PSSetSamplers(2, 1, mFogNoiseSampler.GetAddressOf());
    }
    mContext->PSSetSamplers(1, 1, mFogGridSampler.GetAddressOf());
    if (lod) {
        DrawFogLod(volume, *flow, action, viewport, lod, fieldFraction, extinction,
                   measurement ? measurement->lowEnd.Get() : nullptr);
        mContext->PSSetShaderResources(2, 1, oldReduced.GetAddressOf());
        mContext->PSSetConstantBuffers(1, 1, oldTemporalCb.GetAddressOf());
    } else {
        mFogLastFrame = 0;
        mContext->PSSetShader(multisampled ? mFogMsaaPs.Get() : mFogPs.Get(), nullptr, 0);
        mContext->Draw(3, 0);
    }

    ID3D11ShaderResourceView* nullSrv = nullptr;
    mContext->PSSetShaderResources(0, 1, &nullSrv);
    mContext->OMSetRenderTargets(1, fb.render_target_view.GetAddressOf(), fb.depth_stencil_view.Get());
    mContext->OMSetDepthStencilState(oldDepth.Get(), stencilRef);
    mContext->OMSetBlendState(oldBlend.Get(), blendFactor, sampleMask);
    mContext->RSSetState(oldRaster.Get());
    mContext->IASetInputLayout(oldLayout.Get());
    mContext->IASetPrimitiveTopology(oldTopology);
    mContext->VSSetShader(oldVs.Get(), nullptr, 0);
    mContext->PSSetShader(oldPs.Get(), nullptr, 0);
    mContext->PSSetConstantBuffers(0, 1, oldCb.GetAddressOf());
    mContext->PSSetShaderResources(0, 1, oldSrv.GetAddressOf());
    mContext->PSSetShaderResources(1, 1, oldField.GetAddressOf());
    if (useSupport) mContext->PSSetShaderResources(3, 1, oldSupport.GetAddressOf());
    if (useLift) mContext->PSSetShaderResources(4, 1, oldLift.GetAddressOf());
    if (useUpper) mContext->PSSetShaderResources(6, 1, oldUpper.GetAddressOf());
    if (gpuLift) mContext->PSSetShaderResources(7, 1, oldGpuLift.GetAddressOf());
    if (useNoise) {
        mContext->PSSetShaderResources(8, 1, oldNoise.GetAddressOf());
        mContext->PSSetSamplers(2, 1, oldNoiseSampler.GetAddressOf());
    }
    mContext->PSSetSamplers(1, 1, oldFogSampler.GetAddressOf());

    if (measurement) {
        mContext->End(measurement->end.Get());
        mContext->End(measurement->disjoint.Get());
        measurement->pending = true;
    }
}

void GfxRenderingAPIDX11::ClearFramebuffer(bool color, bool depth) {
    FramebufferDX11& fb = mFrameBuffers[mCurrentFramebuffer];
    if (color) {
        const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        mContext->ClearRenderTargetView(fb.render_target_view.Get(), clearColor);
    }
    if (depth && fb.has_depth_buffer) {
        // SOH [Enhancement] Stencil volumes: also clear the stencil plane (the combined D24S8 format always
        // has one). The z-fail masks assume stencil starts at 0 each frame, and the composite only
        // self-zeroes pixels it draws.
        mContext->ClearDepthStencilView(fb.depth_stencil_view.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
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
            res.emplace(coord, ((float*)ms.pData)[i++] * 65532.0f);
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
        { "o_toon", cc_features.opt_toon },
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
