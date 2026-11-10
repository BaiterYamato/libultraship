#pragma once

#include <array>
#include <cstdint>

namespace Fast {
inline constexpr uint32_t FogNoiseSide = 64;

inline constexpr auto FogNoise = [] {
    std::array<uint8_t, FogNoiseSide * FogNoiseSide> noise{};
    uint32_t state = 0xe52b7b51u;
    for (auto& value : noise) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        value = static_cast<uint8_t>(state >> 24);
    }
    return noise;
}();

inline constexpr char FogVolumeShader[] = R"hlsl(
#if FOG_MSAA
Texture2DMS<float> worldDepth : register(t0);
#else
Texture2D<float> worldDepth : register(t0);
#endif
Texture2D<float4> fogField : register(t1);
Texture2D<float4> reducedFog : register(t2);
Texture2D<float4> supportRanges : register(t3);
Texture2D<float4> raisedField : register(t4);
Texture2D<float4> upperField : register(t6);
Texture2D<float4> computedLiftField : register(t7);
Texture2D<float> noiseField : register(t8);
#if FOG_MSAA
Texture2D<float4> uniformFog : register(t5);
#endif

struct FogLight {
    float4 positionRadius;
    float4 color;
};
SamplerState fogSampler : register(s1);
SamplerState noiseSampler : register(s2);

cbuffer FogConstants : register(b0) {
    float4 cameraEye;
    float4 cameraForward;
    float4 cameraRight;
    float4 cameraUp;
    float4 projectionDepth;
    float4 boundsMin;
    float4 boundsMax;
    float4 fogColor;
    float4 grid;
    float4 viewport;
    float4 support; // x: support ranges, y: raised offset, z: interpolation, w: upper support
    float4 lighting; // x: number of point lights
    float4 atmosphere;
    float4 visual; // x: computed lift, y: texture detail, zw: wind offset
    FogLight lights[6];
};

cbuffer FogTemporalConstants : register(b1) {
    float4 low; // width, height, history weight, sample offset
    float4 previousEye;
    float4 previousForward;
    float4 previousRight;
    float4 previousUp;
};

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}

float NonZero(float v) {
    return abs(v) > 0.00001 ? v : (v < 0.0 ? -0.00001 : 0.00001);
}

float DecodeDepth(float rawDepth) {
    float ndcZ = rawDepth * 2.0 - 1.0;
    float zView = clamp(projectionDepth.z / NonZero(projectionDepth.x - ndcZ * projectionDepth.y),
                        0.0, projectionDepth.w);
    return rawDepth >= 0.999999 || cameraEye.w == 3.0 ? projectionDepth.w : zView;
}

float NearestDepth(int2 pixel) {
#if FOG_MSAA
    uint width, height, samples;
    worldDepth.GetDimensions(width, height, samples);
    float raw = 1.0;
    [loop]
    for (uint i = 0; i < samples; ++i) raw = min(raw, worldDepth.Load(pixel, i));
    return raw;
#else
    return worldDepth.Load(int3(pixel, 0));
#endif
}

#if FOG_MSAA
float2 SamplePosition(float2 position, uint sampleIndex) {
    return floor(position) + 0.5 + worldDepth.GetSamplePosition(sampleIndex);
}
#endif

float3 WorldRay(float2 uv) {
    float2 ndc = float2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y);
    // A unidade do parâmetro t é profundidade de câmera. O comprimento do
    // raio converte o passo em distância de mundo para a atenuação.
    return cameraForward.xyz + cameraRight.xyz * (cameraRight.w * ndc.x) +
           cameraUp.xyz * (cameraUp.w * ndc.y);
}

// Integrated smoothstep lobes. The profile is flat at its feet, crest and top.
float SmoothFogArea(float t) {
    return t * t * t * (1.0 - 0.5 * t);
}

float GroundFogArea(float height) {
    float h = saturate(height);
    const float peak = 0.25;
    return h < peak ? peak * SmoothFogArea(h / peak)
                    : 0.5 - (1.0 - peak) * SmoothFogArea((1.0 - h) / (1.0 - peak));
}

float GroundFogAverage(float height, float rise, float thickness) {
    float h = height / thickness;
    float span = abs(rise) / thickness;
    if (span < 0.001) return 0.76 * smoothstep(0.0, 1.0, min(4.0 * h, (1.0 - h) / 0.75));
    return 0.76 * (GroundFogArea(h + 0.5 * span) - GroundFogArea(h - 0.5 * span)) / span;
}

float WideFogProfile(float height, float rise, float raised, float thickness, float offset) {
    float ground = GroundFogAverage(height, rise, thickness);
    [branch]
    if (raised > 0.0 && offset > 0.0) ground = lerp(ground, GroundFogAverage(height - offset, rise, thickness), raised);
    return ground * (grid.z / thickness);
}

float InterpolateLift(float2 pair) {
    return lerp(pair.x, pair.y, support.z);
}

float SampleBaseLift(float2 uv) {
    [branch]
    if (visual.x > 0.0) return computedLiftField.SampleLevel(fogSampler, uv, 0.0).z;
    return InterpolateLift(raisedField.SampleLevel(fogSampler, uv, 0.0).xy);
}

float LoadBaseLift(int2 cell) {
    [branch]
    if (visual.x > 0.0) return computedLiftField.Load(int3(cell, 0)).z;
    return InterpolateLift(raisedField.Load(int3(cell, 0)).xy);
}

float2 InterpolateUpper(float4 pair) {
    return lerp(pair.xy, pair.zw, support.z);
}

float4 CornerWeights(float2 sampleCell) {
    float2 f = frac(sampleCell);
    float2 inv = 1.0 - f;
    return float4(inv.x * inv.y, f.x * inv.y, inv.x * f.y, f.x * f.y);
}

float WideFogDensity(float2 sampleCell, float y, float rise, float density, float floorY, float raised,
                     float thickness, bool limitLift) {
    int2 base = int2(sampleCell);
    int2 end = min(base + 1, int2(grid.xy) - 1);
    float2 a = fogField.Load(int3(base, 0)).xy;
    float2 b = fogField.Load(int3(end.x, base.y, 0)).xy;
    float2 c = fogField.Load(int3(base.x, end.y, 0)).xy;
    float2 d = fogField.Load(int3(end, 0)).xy;
    float4 densities = float4(a.x, b.x, c.x, d.x);
    float4 floors = float4(a.y, b.y, c.y, d.y) / max(densities, 0.0001);
    float4 weights = CornerWeights(sampleCell);
    float4 delta = (floors - floorY) / grid.z;
    float variance = dot(weights * densities, delta * delta) / max(density, 0.0001);
    // A distant corner gradually gains influence as the ray crosses a cell boundary.
    float splitWeight = limitLift ? 1.0 : smoothstep(0.18 * 0.18, 0.33 * 0.33, variance);
    if (splitWeight <= 0.0) {
        return density * WideFogProfile(y - floorY, rise, raised, thickness, support.y);
    }

    // Blend optical density after evaluating each support height. Blending the heights first
    // creates a sloped fog sheet between the lower ground and the platform.
    float4 raisedCorners = 0.0;
    [branch]
    if (raised > 0.0) {
        raisedCorners = float4(LoadBaseLift(base), LoadBaseLift(int2(end.x, base.y)),
                               LoadBaseLift(int2(base.x, end.y)), LoadBaseLift(end));
    }
    float4 layers;
    [unroll]
    for (int j = 0; j < 4; ++j) {
        float offset = support.y;
        [branch]
        if (limitLift && raisedCorners[j] > 0.0) {
            int2 corner = int2((j & 1) ? end.x : base.x, (j & 2) ? end.y : base.y);
            float2 upper = InterpolateUpper(upperField.Load(int3(corner, 0)));
            if (upper.x > 0.0001) {
                float available = upper.y / upper.x - floors[j] - thickness;
                offset = clamp(available, 0.0, support.y);
            }
        }
        layers[j] = clamp(densities[j], 0.0, 1.5) * WideFogProfile(y - floors[j], rise, raisedCorners[j],
                                                                 thickness, offset);
    }
    float splitDensity = dot(weights, layers);
    if (splitWeight >= 1.0) return splitDensity;
    float joinedDensity = density * WideFogProfile(y - floorY, rise, raised, thickness, support.y);
    return lerp(joinedDensity, splitDensity, splitWeight);
}

float UpperFogDensity(float2 sampleCell, float2 textureUv, float y, float rise, float thickness, bool split) {
    float raised = 0.0;
    [branch]
    if (support.y > 0.0) raised = InterpolateLift(raisedField.SampleLevel(fogSampler, textureUv, 0.0).zw);
    [branch]
    if (!split) {
        float2 cell = InterpolateUpper(upperField.SampleLevel(fogSampler, textureUv, 0.0));
        return cell.x * WideFogProfile(y - cell.y / max(cell.x, 0.0001), rise, raised, thickness, support.y);
    }
    int2 base = int2(sampleCell);
    int2 end = min(base + 1, int2(grid.xy) - 1);
    float2 a = InterpolateUpper(upperField.Load(int3(base, 0)));
    float2 b = InterpolateUpper(upperField.Load(int3(end.x, base.y, 0)));
    float2 c = InterpolateUpper(upperField.Load(int3(base.x, end.y, 0)));
    float2 d = InterpolateUpper(upperField.Load(int3(end, 0)));
    float4 density = float4(a.x, b.x, c.x, d.x);
    float4 floorY = float4(a.y, b.y, c.y, d.y) / max(density, 0.0001);
    float4 raisedCorners = 0.0;
    [branch]
    if (raised > 0.0) {
        raisedCorners = float4(InterpolateLift(raisedField.Load(int3(base, 0)).zw),
                               InterpolateLift(raisedField.Load(int3(end.x, base.y, 0)).zw),
                               InterpolateLift(raisedField.Load(int3(base.x, end.y, 0)).zw),
                               InterpolateLift(raisedField.Load(int3(end, 0)).zw));
    }
    float4 layers;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        layers[i] = density[i] * WideFogProfile(y - floorY[i], rise, raisedCorners[i], thickness, support.y);
    }
    return dot(CornerWeights(sampleCell), layers);
}

float FogDetail(float2 p) {
    float2 f = frac(p);
    float2 uv = (floor(p) + f * f * (3.0 - 2.0 * f) + 0.5) / 64.0;
    return 2.0 * noiseField.SampleLevel(noiseSampler, uv, 0.0) - 1.0;
}

float WideFogThickness(float2 world) {
    // A stationary envelope on a scale larger than the terrain grid. The column integral
    // stays constant as the height changes across the world.
    float bank = 0.5 + 0.5 * FogDetail(world * 0.0045 + float2(23.0, 19.0));
    return grid.z * lerp(0.77, 1.0, bank);
}

float CompactFogProfile(float height, float thickness, float scale, float topFeather) {
    float top = saturate((thickness - height) / topFeather);
    if (topFeather > 8.0) top = top * top * (3.0 - 2.0 * top);
    return exp(-max(height, 0.0) / scale) * min(saturate(height * 0.125), top);
}

)hlsl"
R"hlsl(
// Opacity, depth of the density-weighted sample, scene depth, and end of the visible segment.
float4 March(float3 ray, float zView, float offset, int steps) {
    float3 invRay = 1.0 / float3(NonZero(ray.x), NonZero(ray.y), NonZero(ray.z));
    float3 t0 = (boundsMin.xyz - cameraEye.xyz) * invRay;
    float3 t1 = (boundsMax.xyz - cameraEye.xyz) * invRay;
    float3 lo = min(t0, t1);
    float3 hi = max(t0, t1);
    float first = max(max(lo.x, lo.y), max(lo.z, 0.0));
    float last = min(min(hi.x, hi.y), min(hi.z, zView));
    if (last <= first) return float4(0.0, 0.0, zView, 0.0);

    float segment = last - first;
    float rayLength = length(ray);
    bool wide = grid.x >= 2.0 && max(boundsMax.x - boundsMin.x, boundsMax.z - boundsMin.z) > 1024.0;
    bool compact = !wide && grid.x >= 2.0 && fogColor.w > 0.0;
    if (wide || compact) {
        steps = wide ? 48 : 24;
        offset = 0.5;
    }
    float minimumStep = segment / steps;
    float stepSize = minimumStep;
    float integral = 0.0;
    float weightedDepth = 0.0;
    float travelled = 0.0;
    uint textureWidth, textureHeight;
    fogField.GetDimensions(textureWidth, textureHeight);
    [loop]
    for (int i = 0; i < steps; ++i) {
        if (travelled >= segment) break;
        if (wide || compact) {
            // The reduced pass and the full-resolution edge pass share the same intervals.
            float depth = (first + travelled) * rayLength;
            float stepLength = wide
                ? lerp(26.0, 48.0, fogColor.w > 0.0 ? smoothstep(420.0, 980.0, depth) : 0.0)
                : lerp(14.0, 32.0, smoothstep(150.0, 510.0, depth));
            stepSize = max(stepLength / rayLength, minimumStep);
        }
        float width = max(0.0, min(stepSize, segment - travelled));
        float t = first + travelled + offset * width;
        travelled += width;
        float3 samplePoint = cameraEye.xyz + ray * t;
        float3 inside = min(samplePoint - boundsMin.xyz, boundsMax.xyz - samplePoint);
        float edge = saturate(min(inside.x, inside.z) / boundsMax.w);
        float distance = t * rayLength;
        if (wide) edge = edge * edge * (3.0 - 2.0 * edge);
        float density = 1.0;
        float floorY = boundsMin.y;
        float thickness = boundsMax.y - boundsMin.y;
        float heightScale = boundsMin.w;
        float topFeather = 8.0;
        float wideDensity = 0.0;
        float upperDensity = 0.0;
        float raised = 0.0;
        if (grid.x >= 2.0) {
            float2 fieldUv = saturate((samplePoint.xz - boundsMin.xz) / (boundsMax.xz - boundsMin.xz));
            float2 sampleCell = fieldUv * (grid.xy - 1.0);
            bool split = wide;
            bool hasUpper = support.w > 0.0;
            bool upperInRange = hasUpper;
            bool upperSplit = true;
            if (wide && support.x > 0.0) {
                float4 range = supportRanges.Load(int3(int2(sampleCell), 0));
                float halfRise = 0.5 * abs(ray.y * width);
                hasUpper = hasUpper && range.z <= range.w;
                upperInRange = hasUpper && samplePoint.y + halfRise > range.z &&
                               samplePoint.y - halfRise < range.w + grid.z + support.y;
                upperSplit = range.w - range.z > 0.5 * grid.z;
                bool baseInRange = range.x <= range.y && samplePoint.y + halfRise > range.x &&
                                   samplePoint.y - halfRise < range.y + grid.z + support.y;
                if (!baseInRange && !upperInRange) continue;
                split = range.y - range.x > 0.36 * grid.z;
            }
            float2 textureUv = (sampleCell + 0.5) / float2(textureWidth, textureHeight);
            float4 cell = fogField.SampleLevel(fogSampler, textureUv, 0.0);
            density = clamp(cell.x, 0.0, 1.5);
            floorY = cell.y / max(cell.x, 0.0001);
            thickness = grid.z;
            [branch]
            if (support.y > 0.0) raised = SampleBaseLift(textureUv);
            if (wide) {
                if (visual.y > 0.0) thickness = WideFogThickness(samplePoint.xz);
                bool limitLift = hasUpper && raised > 0.0;
                [branch]
                if (split || limitLift) {
                    wideDensity = WideFogDensity(sampleCell, samplePoint.y, ray.y * width, density, floorY, raised,
                                                thickness, limitLift);
                } else {
                    wideDensity = density * WideFogProfile(samplePoint.y - floorY, ray.y * width, raised,
                                                           thickness, support.y);
                }
            }
            [branch]
            if (visual.y > 0.0) {
                // Stretch filaments along the velocity field and vary the thickness of the visible layer.
                float2 p = samplePoint.xz - cell.zw * 0.18;
                float compression = saturate((density - 0.60) * 2.5);
                if (wide) {
                    // Blend near-field structure into the broad layer without changing its mean opacity.
                    float detailWeight = 1.0 - smoothstep(420.0, 980.0, distance);
                    [branch]
                    if (detailWeight > 0.0) {
                        float cloud = FogDetail(p * 0.009 + visual.zw);
                        float detail = FogDetail(p.yx * 0.021 - visual.wz + float2(7.0, 15.0) + 0.4 * cloud);
                        wideDensity *= 1.0 + detailWeight * (0.12 * cloud + 0.08 * detail + 0.48 * compression);
                    }
                } else {
                    float detailWeight = 1.0 - smoothstep(160.0, 490.0, distance);
                    float cloud = 0.0, detail = 0.0;
                    [branch]
                    if (detailWeight > 0.0) {
                        cloud = FogDetail(p * 0.023 + 1.5 * visual.zw);
                        detail = FogDetail(p.yx * 0.053 - visual.wz + float2(7.0, 15.0) + 0.5 * cloud +
                                           float2(0.042, -0.027) * (samplePoint.y - floorY));
                    }
                    float ribbon = saturate(0.5 + detailWeight * (0.44 * cloud + 0.31 * detail));
                    // Vary the top across the floor and feather it over more than one sample.
                    float bank = 0.5 + 0.5 * detailWeight * cloud;
                    thickness *= lerp(0.69, 1.0, bank);
                    topFeather = max(8.0, thickness * 0.48);
                    float motion = saturate((abs(cell.z) + abs(cell.w)) / 190.0);
                    density *= lerp(0.46, 1.34, ribbon) * (1.0 + 0.60 * compression);
                    heightScale *= lerp(0.65, 1.36, ribbon) + (0.25 * motion + 0.45 * compression) * ribbon;
                }
            }
            [branch]
            if (upperInRange) {
                upperDensity = UpperFogDensity(sampleCell, textureUv, samplePoint.y, ray.y * width, thickness,
                                               upperSplit);
            }
        }
        float opticalDensity = wideDensity;
        [branch]
        if (!wide) {
            float height = samplePoint.y - floorY;
            float vertical = CompactFogProfile(height, thickness, heightScale, topFeather);
            [branch]
            if (raised > 0.0) {
                vertical = lerp(vertical, CompactFogProfile(height - support.y, thickness, heightScale, topFeather),
                                raised);
            }
            opticalDensity = density * vertical;
        }
        float sampleDensity = (opticalDensity + upperDensity) * edge * width;
        integral += sampleDensity;
        weightedDepth += sampleDensity * t;
    }
    float alpha = 1.0 - exp(-integral * rayLength * cameraForward.w);
    return float4(alpha, weightedDepth / max(integral, 0.00001), zView, last);
}

)hlsl"
R"hlsl(
float4 FogColor(float2 uv, float alpha, float anchor) {
    float3 color = fogColor.rgb;
    float3 ray = 0.0;
    [branch]
    if ((atmosphere.w > 0.0 || lighting.x > 0.0) && alpha > 0.002) ray = WorldRay(uv);
    [branch]
    if (atmosphere.w > 0.0 && alpha > 0.002) {
        float blend = atmosphere.w * smoothstep(540.0, 1120.0, anchor * length(ray));
        color = lerp(color, atmosphere.rgb, blend);
        alpha *= 1.0 - 0.14 * blend * alpha * alpha;
    }
    [branch]
    if (lighting.x > 0.0 && alpha > 0.002) {
        float3 world = cameraEye.xyz + ray * anchor;
        float3 radiance = 0.0;
        float strength = 0.0;
        [loop]
        for (int i = 0; i < int(lighting.x); ++i) {
            float3 distance = world - lights[i].positionRadius.xyz;
            float radius = lights[i].positionRadius.w;
            float fade = saturate(1.0 - dot(distance, distance) / (radius * radius));
            float weight = fade * fade;
            float3 light = lights[i].color.rgb;
            radiance += light * weight;
            strength += max(light.r, max(light.g, light.b)) * weight;
        }
        float3 tint = saturate(0.35 * color + radiance / max(strength, 0.0001));
        color = lerp(color, tint, saturate(0.58 * strength));
    }
    return float4(color * alpha, alpha);
}

float4 FullFog(float2 position, float zView) {
    if (cameraEye.w == 2.0) {
        float gray = log2(1.0 + zView) / log2(1.0 + projectionDepth.w);
        return float4(gray, gray, gray, 1.0);
    }
    float2 uv = (position - viewport.xy) / viewport.zw;
    float4 fog = March(WorldRay(uv), zView, 0.5, 16);
    return FogColor(uv, fog.x, fog.y);
}

#if FOG_MSAA
float4 PSMain(float4 position : SV_Position, uint sampleIndex : SV_SampleIndex) : SV_Target {
    return FullFog(SamplePosition(position.xy, sampleIndex),
                   DecodeDepth(worldDepth.Load(int2(position.xy), sampleIndex)));
}
#else
float4 PSMain(float4 position : SV_Position) : SV_Target {
    return FullFog(position.xy, DecodeDepth(NearestDepth(int2(position.xy))));
}
#endif

float HistoryOpacity(int2 pixel, float3 ray, float4 current) {
    float opacity = current.x;
    [branch]
    if (low.z > 0.0 && current.x >= 0.006) {
        float z = current.y;
        float oldEnd = current.w;
        int2 previousPixel = pixel;
        bool valid = true;
        [branch]
        if (previousForward.w > 0.0) {
            float3 relative = cameraEye.xyz + ray * current.y - previousEye.xyz;
            z = dot(relative, previousForward.xyz);
            float2 ndc = float2(dot(relative, previousRight.xyz) / previousRight.w,
                                dot(relative, previousUp.xyz) / previousUp.w) / max(z, 0.0001);
            float2 uv = float2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y);
            valid = z > 0.0 && all(uv >= 0.0) && all(uv < 1.0);
            previousPixel = min(int2(saturate(uv) * low.xy), int2(low.xy) - 1);
            oldEnd = dot(cameraEye.xyz + ray * current.w - previousEye.xyz, previousForward.xyz);
        }
        if (valid) {
            float4 old = reducedFog.Load(int3(previousPixel, 0));
            if (abs(old.y - z) <= max(10.0, 0.065 * z) && old.z >= z &&
                abs(old.w - oldEnd) <= max(12.0, 0.045 * oldEnd) &&
                abs(old.x - current.x) <= max(0.035, 0.13 * current.x)) {
                opacity = lerp(current.x, clamp(old.x, current.x - 0.035, current.x + 0.035), low.z);
            }
        }
    }
    return opacity;
}

float4 PSLow(float4 position : SV_Position) : SV_Target {
    float2 uv = position.xy / low.xy;
    float2 footprint = viewport.zw / low.xy;
    int2 base = int2(viewport.xy + (position.xy - 0.5) * footprint);
    int2 last = int2(ceil(viewport.xy + viewport.zw)) - 1;
    int2 end = min(base + 1, last);
    float depth = min(min(NearestDepth(base), NearestDepth(int2(end.x, base.y))),
                      min(NearestDepth(int2(base.x, end.y)), NearestDepth(end)));
    float3 ray = WorldRay(uv);
    float4 current = March(ray, DecodeDepth(depth), low.w, grid.w == 2.0 ? 8 : 12);
    current.x = HistoryOpacity(int2(position.xy), ray, current);
    return current;
}

float4 UpscaleFog(float2 position, float zView) {
    float2 uv = (position - viewport.xy) / viewport.zw;
    float tolerance = max(5.0, 0.018 * zView);
    float4 depths = reducedFog.GatherBlue(fogSampler, uv);
    float4 depthErrors = abs(depths - zView);
    [branch]
    if (all(depthErrors < tolerance * 0.5)) {
        float2 fog = reducedFog.SampleLevel(fogSampler, uv, 0.0).xy;
        return FogColor(uv, fog.x, fog.y);
    }

    float2 f = frac(uv * low.xy - 0.5);
    float2 inv = 1.0 - f;
    // Gather order is bottom-left, bottom-right, top-right, top-left.
    float4 spatial = float4(inv.x * f.y, f.x * f.y, f.x * inv.y, inv.x * inv.y);
    float4 mismatch = smoothstep(tolerance * 0.5, tolerance, depthErrors);
    float4 compatible = 1.0 - mismatch;
    float2 estimate = 0.0;
    float confidence = 0.0;
    [branch]
    if (any(spatial * compatible > 0.0)) {
        float4 anchors = reducedFog.GatherGreen(fogSampler, uv);
        float4 behind = step(zView + 1.0, depths) * smoothstep(zView, zView + 1.0, anchors);
        compatible *= 1.0 - behind * mismatch;
        float4 weights = spatial * compatible;
        float weightSum = dot(weights, 1.0);
        // One well-matched corner can resolve the pixel, even near the edge of its footprint.
        // The weight cap still fades that corner out as its spatial contribution vanishes.
        float best = max(max(compatible.x, compatible.y), max(compatible.z, compatible.w));
        confidence = min(best, 16.0 * weightSum);
        estimate = float2(dot(reducedFog.GatherRed(fogSampler, uv), weights), dot(anchors, weights)) /
                   max(weightSum, 0.0001);
        [branch]
        if (confidence >= 0.6) return FogColor(uv, estimate.x, estimate.y);
    }
    // Resolve thin foreground features against their actual depth. Fade the reduced estimate out
    // as its footprint becomes unreliable so edges remain continuous during camera motion.
    float2 exact = March(WorldRay(uv), zView, 0.5, 4).xy;
    float2 fog = lerp(exact, estimate, smoothstep(0.0, 0.6, confidence));
    return FogColor(uv, fog.x, fog.y);
}

#if FOG_MSAA
float4 PSUniformFog(float4 position : SV_Position) : SV_Target {
    float2 screen = position.xy + viewport.xy;
    int2 pixel = int2(screen);
    uint width, height, samples;
    worldDepth.GetDimensions(width, height, samples);
    float nearDepth = 1.0, farDepth = 0.0;
    [loop]
    for (uint i = 0; i < samples; ++i) {
        float depth = worldDepth.Load(pixel, i);
        nearDepth = min(nearDepth, depth);
        farDepth = max(farDepth, depth);
    }
    float nearView = DecodeDepth(nearDepth);
    float tolerance = clamp(0.002 * nearView, 0.12, 1.2);
    if (DecodeDepth(farDepth) - nearView > tolerance) return float4(0.0, 0.0, 0.0, -1.0);
    return UpscaleFog(screen, nearView);
}

float4 PSUpscaleCached(float4 position : SV_Position, uint sampleIndex : SV_SampleIndex) : SV_Target {
    float4 cached = uniformFog.Load(int3(int2(position.xy - viewport.xy), 0));
    [branch]
    if (cached.a >= 0.0) return cached;
    return UpscaleFog(SamplePosition(position.xy, sampleIndex),
                      DecodeDepth(worldDepth.Load(int2(position.xy), sampleIndex)));
}

float4 PSUpscale(float4 position : SV_Position, uint sampleIndex : SV_SampleIndex) : SV_Target {
    return UpscaleFog(SamplePosition(position.xy, sampleIndex),
                      DecodeDepth(worldDepth.Load(int2(position.xy), sampleIndex)));
}
#else
float4 PSUpscale(float4 position : SV_Position) : SV_Target {
    return UpscaleFog(position.xy, DecodeDepth(NearestDepth(int2(position.xy))));
}
#endif
)hlsl";
} // namespace Fast
