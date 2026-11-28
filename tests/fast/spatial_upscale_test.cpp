// Headless software D3D11 check of the actual embedded HLSL, without MSVC/WRL.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include "spatial_upscale_shader.h"

template<class T> struct Ptr {
    T* p = nullptr;
    ~Ptr() { if (p) p->Release(); }
    T** out() { assert(!p); return &p; }
};
static void Check(HRESULT hr) {
    if (FAILED(hr)) { std::cerr << "HRESULT=" << std::hex << unsigned(hr) << '\n'; std::exit(1); }
}
int main() {
    Ptr<ID3D11Device> device; Ptr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                           D3D11_SDK_VERSION, device.out(), &level, context.out()));
    auto shader = [](const char* entry, const char* profile, ID3DBlob** blob) {
        Ptr<ID3DBlob> errors;
        const auto hr = D3DCompile(SpatialUpscaleShader, std::strlen(SpatialUpscaleShader), "spatial",
                                  nullptr, nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, blob, errors.out());
        if (errors.p) std::cerr << static_cast<const char*>(errors.p->GetBufferPointer());
        Check(hr);
    };
    Ptr<ID3DBlob> vsCode, reconstructionCode, sharpenCode;
    shader("VSMain", "vs_4_0", vsCode.out());
    shader("PSReconstruct", "ps_4_0", reconstructionCode.out());
    shader("PSSharpen", "ps_4_0", sharpenCode.out());
    Ptr<ID3D11VertexShader> vs; Ptr<ID3D11PixelShader> reconstruction, sharpen;
    Check(device.p->CreateVertexShader(vsCode.p->GetBufferPointer(), vsCode.p->GetBufferSize(), nullptr, vs.out()));
    Check(device.p->CreatePixelShader(reconstructionCode.p->GetBufferPointer(), reconstructionCode.p->GetBufferSize(), nullptr, reconstruction.out()));
    Check(device.p->CreatePixelShader(sharpenCode.p->GetBufferPointer(), sharpenCode.p->GetBufferSize(), nullptr, sharpen.out()));
    D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 16; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    Ptr<ID3D11Buffer> cb; Check(device.p->CreateBuffer(&cbd, nullptr, cb.out()));
    const UINT inputWidth = 17, inputHeight = 13, width = 37, height = 29;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Ptr<ID3D11Texture2D> intermediate, output;
    Check(device.p->CreateTexture2D(&desc, nullptr, intermediate.out()));
    Check(device.p->CreateTexture2D(&desc, nullptr, output.out()));
    Ptr<ID3D11ShaderResourceView> intermediateView;
    Ptr<ID3D11RenderTargetView> intermediateTarget, outputTarget;
    Check(device.p->CreateShaderResourceView(intermediate.p, nullptr, intermediateView.out()));
    Check(device.p->CreateRenderTargetView(intermediate.p, nullptr, intermediateTarget.out()));
    Check(device.p->CreateRenderTargetView(output.p, nullptr, outputTarget.out()));
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Ptr<ID3D11Texture2D> staging; Check(device.p->CreateTexture2D(&desc, nullptr, staging.out()));
    desc.Width = inputWidth; desc.Height = inputHeight; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Ptr<ID3D11Texture2D> input; Check(device.p->CreateTexture2D(&desc, nullptr, input.out()));
    Ptr<ID3D11ShaderResourceView> inputView; Check(device.p->CreateShaderResourceView(input.p, nullptr, inputView.out()));
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE; Ptr<ID3D11RasterizerState> raster;
    Check(device.p->CreateRasterizerState(&rd, raster.out()));
    D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    context.p->RSSetViewports(1, &viewport); context.p->RSSetState(raster.p);
    context.p->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context.p->VSSetShader(vs.p, nullptr, 0); context.p->PSSetConstantBuffers(0, 1, &cb.p);
    std::vector<uint32_t> pixels(inputWidth * inputHeight);
    for (unsigned pattern = 0; pattern < 3; ++pattern) {
        for (UINT y = 0; y < inputHeight; ++y) for (UINT x = 0; x < inputWidth; ++x) {
            const uint32_t gray = pattern == 0 ? 96 : ((pattern == 1 ? x >= 8 : x >= y) ? 210 : 32);
            pixels[y * inputWidth + x] = gray | (gray << 8) | (gray << 16) | (173u << 24);
        }
        context.p->UpdateSubresource(input.p, 0, nullptr, pixels.data(), inputWidth * 4, 0);
        float sizes[4] = {float(inputWidth),float(inputHeight),float(width),float(height)};
        context.p->UpdateSubresource(cb.p, 0, nullptr, sizes, 0, 0);
        context.p->OMSetRenderTargets(1, &intermediateTarget.p, nullptr);
        context.p->PSSetShaderResources(0, 1, &inputView.p);
        context.p->PSSetShader(reconstruction.p, nullptr, 0); context.p->Draw(3, 0);
        ID3D11ShaderResourceView* nullView = nullptr;
        context.p->PSSetShaderResources(0, 1, &nullView);
        context.p->OMSetRenderTargets(1, &outputTarget.p, nullptr);
        sizes[0] = float(width); sizes[1] = float(height);
        context.p->UpdateSubresource(cb.p, 0, nullptr, sizes, 0, 0);
        context.p->PSSetShaderResources(0, 1, &intermediateView.p);
        context.p->PSSetShader(sharpen.p, nullptr, 0); context.p->Draw(3, 0);
        context.p->PSSetShaderResources(0, 1, &nullView);
        context.p->OMSetRenderTargets(0, nullptr, nullptr);
        context.p->CopyResource(staging.p, output.p);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context.p->Map(staging.p, 0, D3D11_MAP_READ, 0, &mapped));
        for (UINT y = 0; y < height; ++y) {
            const auto* row = reinterpret_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
            for (UINT x = 0; x < width; ++x) {
                const auto* p = row + x * 4;
                assert(std::abs(int(p[3]) - 173) <= 1);
                assert(p[0] == p[1] && p[1] == p[2]);
                if (pattern == 0) assert(std::abs(int(p[0]) - 96) <= 1);
                else assert(p[0] >= 31 && p[0] <= 211);
                if (pattern == 1 && x < 10) assert(p[0] <= 33);
                if (pattern == 1 && x > 27) assert(p[0] >= 209);
            }
        }
        context.p->Unmap(staging.p, 0);
        std::cout << "WARP pattern=" << pattern << ": uniformidade/bordas/alpha/limites OK\n";
    }
}
