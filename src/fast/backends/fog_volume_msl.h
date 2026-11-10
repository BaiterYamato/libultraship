#pragma once

namespace Fast {
inline constexpr char FogVolumeMSL[] = R"metal(
#include <metal_stdlib>
using namespace metal;

struct FogParameters {
    float4 eye;
    float4 forward;
    float4 right;
    float4 up;
    float4 depth;
    float4 minimum;
    float4 maximum;
    float4 color;
    float4 grid;
    float4 viewport;
    float2 samplePositions[8];
};

vertex float4 fogVertex(uint id [[vertex_id]]) {
    float2 p = float2((id << 1u) & 2u, id & 2u);
    return float4(p.x * 2.0f - 1.0f, 1.0f - p.y * 2.0f, 0.0f, 1.0f);
}

static float nonZero(float v) {
    return abs(v) > 0.00001f ? v : (v < 0.0f ? -0.00001f : 0.00001f);
}

static float2 readFog(device const float2* cells, float2 uv, uint2 size) {
    float2 p = saturate(uv) * float2(size - 1u);
    uint2 first = uint2(p);
    uint2 last = min(first + 1u, size - 1u);
    float2 weight = fract(p);
    float2 a = mix(cells[first.y * size.x + first.x], cells[first.y * size.x + last.x], weight.x);
    float2 b = mix(cells[last.y * size.x + first.x], cells[last.y * size.x + last.x], weight.x);
    return mix(a, b, weight.y);
}

static float4 shadeFog(float2 position, float rawDepth, constant FogParameters& p, device const float2* cells) {
    float ndcZ = rawDepth * 2.0f - 1.0f;
    float sceneZ = clamp(p.depth.z / nonZero(p.depth.x - ndcZ * p.depth.y), 0.0f, p.depth.w);
    if (rawDepth >= 0.999999f || p.eye.w == 3.0f) sceneZ = p.depth.w;
    if (p.eye.w == 2.0f) {
        return float4(float3(log2(1.0f + sceneZ) / log2(1.0f + p.depth.w)), 1.0f);
    }

    float2 uv = (position - p.viewport.xy) / p.viewport.zw;
    float2 ndc = float2(2.0f * uv.x - 1.0f, 1.0f - 2.0f * uv.y);
    float3 ray = p.forward.xyz + p.right.xyz * (p.right.w * ndc.x) + p.up.xyz * (p.up.w * ndc.y);
    float3 inverse = 1.0f / float3(nonZero(ray.x), nonZero(ray.y), nonZero(ray.z));
    float3 t0 = (p.minimum.xyz - p.eye.xyz) * inverse;
    float3 t1 = (p.maximum.xyz - p.eye.xyz) * inverse;
    float3 lo = min(t0, t1);
    float3 hi = max(t0, t1);
    float first = max(max(lo.x, lo.y), max(lo.z, 0.0f));
    float last = min(min(hi.x, hi.y), min(hi.z, sceneZ));
    if (last <= first) return float4(0.0f);

    constexpr int steps = 16;
    float stepSize = (last - first) / float(steps);
    float integral = 0.0f;
    for (int i = 0; i < steps; ++i) {
        float3 point = p.eye.xyz + ray * (first + (float(i) + 0.5f) * stepSize);
        float2 inside = min(point.xz - p.minimum.xz, p.maximum.xz - point.xz);
        float edge = saturate(min(inside.x, inside.y) / p.maximum.w);
        float density = 1.0f;
        float floorY = p.minimum.y;
        float ceiling = p.maximum.y;
        if (p.grid.x >= 2.0f) {
            float2 gridUv = (point.xz - p.minimum.xz) / (p.maximum.xz - p.minimum.xz);
            float2 cell = readFog(cells, gridUv, uint2(p.grid.xy));
            density = clamp(cell.x, 0.0f, 1.5f);
            floorY = cell.y / max(cell.x, 0.0001f);
            ceiling = floorY + p.grid.z;
        }
        float height = point.y - floorY;
        float vertical = exp(-max(height, 0.0f) / p.minimum.w) *
                         saturate(min(height, ceiling - point.y) / 8.0f);
        integral += density * edge * vertical;
    }
    float alpha = 1.0f - exp(-integral * stepSize * length(ray) * p.forward.w);
    return float4(p.color.rgb * alpha, alpha);
}

fragment float4 fogFragment(float4 position [[position]],
                            depth2d<float, access::read> worldDepth [[texture(0)]],
                            constant FogParameters& parameters [[buffer(0)]],
                            device const float2* cells [[buffer(1)]]) {
    return shadeFog(position.xy, worldDepth.read(uint2(position.xy)), parameters, cells);
}

fragment float4 fogFragmentMSAA(float4 position [[position]], uint sampleId [[sample_id]],
                                depth2d_ms<float, access::read> worldDepth [[texture(0)]],
                                constant FogParameters& parameters [[buffer(0)]],
                                device const float2* cells [[buffer(1)]]) {
    float2 pixel = floor(position.xy);
    return shadeFog(pixel + parameters.samplePositions[sampleId], worldDepth.read(uint2(pixel), sampleId),
                    parameters, cells);
}
)metal";
} // namespace Fast
