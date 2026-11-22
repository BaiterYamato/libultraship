#pragma once

#define FAST_KOKIRI_LABPBR_CVAR "gDeveloperTools.KokiriLabPbr"
#define FAST_KOKIRI_LABPBR_BUFFER_CVAR "gDeveloperTools.KokiriLabPbrMaterialBuffer"
// Subcommands of FAST_GFX_WORLD_POSTPROCESS. BEGIN carries signed 10-bit sun components.
#define FAST_WORLD_KOKIRI_LABPBR_BEGIN 10
#define FAST_WORLD_KOKIRI_LABPBR_END 11

#if defined(__cplusplus) && defined(_WIN32) && defined(ENABLE_DX11)
#include <array>
#include <cstddef>
#include <cstdint>

struct ID3D11DepthStencilView;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;

namespace Fast {
class GfxRenderingAPIDX11;

inline constexpr uint64_t KokiriLabPbrShaderBit = uint64_t{1} << 35;

enum class KokiriLabPbrMaterial : uint8_t { Floor, Stone, Wood, Vegetation, None };

struct KokiriLabPbrSurface {
    const char* texture;
    const char* maps;
    uint32_t width;
    uint32_t height;
    bool clampT = false;
    bool textureEdge = false;
};

inline constexpr std::array<KokiriLabPbrSurface, static_cast<std::size_t>(KokiriLabPbrMaterial::None)>
    KokiriLabPbrSurfaces{{
        { "scenes/shared/spot04_scene/spot04_sceneTex_010618", "kokiri-floor", 32, 32 },
        { "scenes/shared/spot04_scene/spot04_room_0Tex_00C708", "kokiri-stone", 32, 16 },
        { "scenes/shared/spot04_scene/spot04_room_0Tex_00D508", "kokiri-wood", 16, 32, true, true },
        { "scenes/shared/spot04_scene/spot04_room_0Tex_010D08", "kokiri-vegetation", 32, 64, true, true },
    }};

struct KokiriLabPbrLighting {
    float eye[4];
    // w is set at draw time to select clamp on the material maps' T axis.
    float sun[4];
};

bool PrepareKokiriLabPbr(GfxRenderingAPIDX11& renderer, KokiriLabPbrMaterial material);
void SetKokiriLabPbrDraw(KokiriLabPbrMaterial material, const KokiriLabPbrLighting& lighting);
struct KokiriLabPbrMaterialViews {
    // RG oct world normal [0,1], B ambient occlusion, A = raster depth + 1; zero means no sample.
    ID3D11ShaderResourceView* normal = nullptr;
    // RGB linear F0, A perceptual roughness. Check normal.a for validity.
    ID3D11ShaderResourceView* specular = nullptr;
};

// Borrowed for the current replay and matching framebuffer attachments.
KokiriLabPbrMaterialViews ReadKokiriLabPbrMaterialBuffer(ID3D11DeviceContext& context,
                                                      ID3D11RenderTargetView* color,
                                                      ID3D11DepthStencilView* depth);
}
#endif
