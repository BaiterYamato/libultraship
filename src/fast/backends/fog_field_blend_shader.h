#pragma once

namespace Fast {
inline constexpr char FogFieldBlendShader[] = R"hlsl(
Texture2D<float4> previousField : register(t0);
Texture2D<float4> currentField : register(t1);
Texture2D<float4> previousVertical : register(t2);
Texture2D<float4> currentVertical : register(t3);
RWTexture2D<float4> displayField : register(u0);
RWTexture2D<float4> displayVertical : register(u1);
cbuffer BlendConstants : register(b0) {
    uint2 fieldSize;
    float fraction;
    uint vertical;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= fieldSize)) return;
    float4 previous = previousField.Load(int3(id.xy, 0));
    float4 current = currentField.Load(int3(id.xy, 0));
    displayField[id.xy] = lerp(previous, current, fraction);
    [branch]
    if (vertical != 0) {
        float4 from = previousVertical.Load(int3(id.xy, 0));
        float4 to = currentVertical.Load(int3(id.xy, 0));
        displayVertical[id.xy] = lerp(from, to, fraction);
    }
}
)hlsl";
} // namespace Fast
