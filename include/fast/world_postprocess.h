#pragma once

// w0: opcode nos bits 24..31, subcomando nos bits 0..7; w1: ticket uint32_t do snapshot.
// 0 mantém o comando legado de preparação+desenho. 1..4 capturam geometria,
// 5 apresenta o mundo antes do HUD (world_presentation.h). 6/7 formam um par:
// preparação -> névoa -> composição, com o MESMO ticket não nulo em ambos.
// O ticket identifica um snapshot imutável e a instância do pack; tickets vencidos
// ou de outro pack ativo são ignorados. O par executa novamente em cada replay.
#define FAST_GFX_WORLD_POSTPROCESS 0x50
#define FAST_WORLD_DRAW 0
#define FAST_WORLD_GEOMETRY_BEGIN 1
#define FAST_WORLD_GEOMETRY_END 2
// w1 on GEOMETRY_BEGIN: restrict this range to inferred XLU cutouts (never solid geometry).
#define FAST_WORLD_GEOMETRY_CUTOUT_ONLY 1
#define FAST_WORLD_TRANSMISSION_BEGIN 3
#define FAST_WORLD_TRANSMISSION_END 4
#define FAST_WORLD_PREPARE 6
#define FAST_WORLD_COMPOSE 7
// M12: escopo exclusivo da geometria de sala XLU; w1=ticket imutavel.
#define FAST_WORLD_WATER_BEGIN 8
#define FAST_WORLD_WATER_END 9

#ifdef __cplusplus
#include <array>
#include "fast/water_displace_math.h"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>
#endif

namespace Fast {
class Interpreter;
// Hyrule Field com a vegetação dinâmica passa de 300 mil vértices (GFX-UNIFY-001, 11/11): 180 mil não bastavam.
inline constexpr unsigned MaxWorldGeometryVertices = 540000;
static_assert(MaxWorldGeometryVertices % 3 == 0);
enum class WorldPostProcessPhase : uint8_t {
    Draw = FAST_WORLD_DRAW,
    Prepare = FAST_WORLD_PREPARE,
    Compose = FAST_WORLD_COMPOSE,
};
using WorldPostProcess = void (*)(Interpreter&, uint32_t ticket, WorldPostProcessPhase phase);
void SetWorldPostProcess(WorldPostProcess callback);
// Ponte livre, sem vtable/membro novo em Interpreter/GfxRenderingAPI/RSP.
using WorldWaterReader = bool (*)(Interpreter&, uint32_t, WaterDisplace::Frame&);
void SetWorldWaterReader(WorldWaterReader callback);
// Muda a cada Interpreter::Run, inclusive nos replays interpolados da mesma display list.
// Fica fora da classe Interpreter para nao mudar o layout dela.
uint64_t WorldPostProcessReplay();
bool TryReadInterpolatedMatrix(const Interpreter& interpreter, const void* key, float (&matrix)[4][4]);
// The opaque display-list ranges accumulate vertices for the current interpreter replay only.
std::span<const std::array<float, 3>> ReadWorldGeometry();
// Verdadeiro quando a captura do replay atual encheu; ReadWorldGeometry devolve os triângulos já guardados.
bool WorldGeometryOverflowed();
inline constexpr unsigned MaxWorldTransmissionTriangles = 48;
struct WorldTransmissionTriangle {
    std::string texture;
    std::array<std::array<float, 3>, 3> vertices;
};
using WorldMaterialMatcher = bool (*)(uint16_t scene, std::string_view texture);
void SetWorldMaterialMatcher(WorldMaterialMatcher matcher);
std::span<const WorldTransmissionTriangle> ReadWorldTransmission();
#ifdef _WIN32
inline constexpr unsigned WorldCutoutTextureSlots = 6;
inline constexpr unsigned MaxWorldCutoutFloats = 1024 * 1024;
struct WorldCutoutBatch {
    unsigned offset, count, stride; // Offsets and stride in floats, count in vertices.
    bool foliage = false;
    std::array<Microsoft::WRL::ComPtr<ID3D11SamplerState>, 2> pointSamplers;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, WorldCutoutTextureSlots> views;
    std::array<Microsoft::WRL::ComPtr<ID3D11SamplerState>, WorldCutoutTextureSlots> samplers;
    std::array<std::array<uint32_t, 4>, 2> filter{};
};
struct WorldCutoutGeometry {
    std::span<const float> vertices;
    std::span<const WorldCutoutBatch> batches;
};
WorldCutoutGeometry ReadWorldCutouts();

// Valid only for the current display-list replay on the publishing context.
struct WorldFogSun {
    ID3D11DeviceContext* context = nullptr;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth;
    std::array<std::array<float, 4>, 4> projection{};
    std::array<float, 4> radiance{};
    bool unoccluded = false;
};
inline thread_local WorldFogSun worldFogSun;

// Publicado somente entre Prepare/Compose. O dono fraco e o replay impedem
// captura em recursos vencidos, inclusive ao desativar/trocar o pack sem Compose.
// Ponteiros emprestados: o WorldPass possui todos os recursos abaixo.
struct WorldFogLayer {
    std::weak_ptr<const void> owner;
    ID3D11DeviceContext* context = nullptr;
    uint64_t replay = 0;
    ID3D11RenderTargetView* scene = nullptr;
    ID3D11DepthStencilView* attachment = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    ID3D11ShaderResourceView* depth = nullptr; // Sempre Texture2D, minimo MSAA como scene.depth.
};
inline thread_local WorldFogLayer worldFogLayer;
#endif
} // namespace Fast
#endif
