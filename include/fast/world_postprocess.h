#pragma once

#define FAST_GFX_WORLD_POSTPROCESS 0x50
#define FAST_WORLD_GEOMETRY_BEGIN 1
#define FAST_WORLD_GEOMETRY_END 2
#define FAST_WORLD_TRANSMISSION_BEGIN 3
#define FAST_WORLD_TRANSMISSION_END 4

#ifdef __cplusplus
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>
#endif

namespace Fast {
class Interpreter;
inline constexpr unsigned MaxWorldGeometryVertices = 180000;
using WorldPostProcess = void (*)(Interpreter&, uint32_t ticket);
void SetWorldPostProcess(WorldPostProcess callback);
bool TryReadInterpolatedMatrix(const Interpreter& interpreter, const void* key, float (&matrix)[4][4]);
// The opaque display-list ranges accumulate vertices for the current interpreter replay only.
std::span<const std::array<float, 3>> ReadWorldGeometry();
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
#endif
} // namespace Fast
#endif
