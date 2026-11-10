#pragma once

// Original implementation: directional normalized 12-tap reconstruction and
// contrast-limited sharpening. No AMD source, coefficients or headers used.
static constexpr const char* SpatialUpscaleShader = R"hlsl(
Texture2D<float4> Source : register(t0);
SamplerState ClampLinear : register(s0);
cbuffer Spatial : register(b0) {
    float2 InputSize;
    float2 OutputSize;
};
struct Vertex { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Vertex VSMain(uint id : SV_VertexID) {
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.pos = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
float4 Fetch(int2 p) { return Source.Load(int3(clamp(p, int2(0,0), int2(InputSize)-1), 0)); }
float Luma(float3 c) { return dot(c, float3(0.25, 0.5, 0.25)); }
float4 PSLinear(Vertex v) : SV_Target { return Source.SampleLevel(ClampLinear, v.uv, 0); }
float4 PSReconstruct(Vertex v) : SV_Target {
    float2 p = v.uv * InputSize - 0.5;
    int2 base = int2(floor(p));
    // Symmetric 4x4 support with the four corners removed (exactly 12 loads).
    const int2 offsets[12] = {
        int2(0,-1), int2(1,-1), int2(-1,0), int2(0,0), int2(1,0), int2(2,0),
        int2(-1,1), int2(0,1), int2(1,1), int2(2,1), int2(0,2), int2(1,2)
    };
    float4 c[12]; float l[12];
    [unroll] for (int i=0; i<12; ++i) { c[i]=Fetch(base+offsets[i]); l[i]=Luma(c[i].rgb); }
    // Estimate normal across the central cell; tangent follows the edge.
    float2 gradient = float2((l[4]+l[8])-(l[3]+l[7]), (l[7]+l[8])-(l[3]+l[4]));
    float magnitude = length(gradient);
    float2 normal = magnitude > 1e-5 ? gradient / magnitude : float2(1,0);
    float2 tangent = float2(-normal.y, normal.x);
    float lo = min(min(l[3],l[4]), min(l[7],l[8]));
    float hi = max(max(l[3],l[4]), max(l[7],l[8]));
    float edge = saturate(magnitude / max(hi-lo, 0.04));
    float4 sum = 0; float weights = 0;
    [unroll] for (int j=0; j<12; ++j) {
        float2 d = float2(base+offsets[j])-p;
        float across = dot(d,normal), along = dot(d,tangent);
        float radius = across*across*lerp(1.0,2.0,edge) + along*along*lerp(1.0,0.65,edge);
        // Smooth compact kernel; positive weights avoid new ringing extrema.
        float w = max(0.0,1.0-radius/4.0); w *= w;
        sum += c[j]*w; weights += w;
    }
    float4 result = sum / max(weights,1e-6);
    float3 minC = min(min(c[3].rgb,c[4].rgb),min(c[7].rgb,c[8].rgb));
    float3 maxC = max(max(c[3].rgb,c[4].rgb),max(c[7].rgb,c[8].rgb));
    result.rgb = clamp(result.rgb,minC,maxC);
    return result;
}
float4 PSSharpen(Vertex v) : SV_Target {
    int2 p = int2(v.pos.xy);
    float4 c = Fetch(p);
    float3 n = Fetch(p+int2(0,-1)).rgb, s = Fetch(p+int2(0,1)).rgb;
    float3 e = Fetch(p+int2(1,0)).rgb, w = Fetch(p+int2(-1,0)).rgb;
    float3 lo = min(c.rgb,min(min(n,s),min(e,w)));
    float3 hi = max(c.rgb,max(max(n,s),max(e,w)));
    float contrast = max(max(hi.r-lo.r,hi.g-lo.g),hi.b-lo.b);
    // Suppress sharpening in flat/noisy areas; limit overshoot to local range.
    float gain = 0.2 * saturate((contrast-0.02)/0.15) / (1.0+2.0*contrast);
    c.rgb = clamp(c.rgb+gain*(4.0*c.rgb-n-s-e-w),lo,hi);
    return c; // preserve reconstructed alpha, never sharpen it
}
)hlsl";
