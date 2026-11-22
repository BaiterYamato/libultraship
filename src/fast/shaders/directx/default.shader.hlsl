@prism(type='hlsl', name='Fast3D HLSL Shader', version='1.0.0', description='Ported shader to prism', author='Emill & Prism Team')

@if(o_root_signature)
    @if(o_textures[0])
        #define RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | DENY_VERTEX_SHADER_ROOT_ACCESS), CBV(b0, visibility = SHADER_VISIBILITY_PIXEL), DescriptorTable(SRV(t0), visibility = SHADER_VISIBILITY_PIXEL), DescriptorTable(Sampler(s0), visibility = SHADER_VISIBILITY_PIXEL)"
    @end
    @if(o_textures[1])
        #define RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | DENY_VERTEX_SHADER_ROOT_ACCESS), CBV(b0, visibility = SHADER_VISIBILITY_PIXEL), DescriptorTable(SRV(t1), visibility = SHADER_VISIBILITY_PIXEL), DescriptorTable(Sampler(s1), visibility = SHADER_VISIBILITY_PIXEL)"
    @end
@end

struct PSInput {
#ifdef LINKSPAN_LABPBR_MSAA
    sample noperspective
#endif
    float4 position : SV_POSITION;
@for(i in 0..2)
    @if(o_textures[i])
        float2 uv@{i} : TEXCOORD@{i};
        @{update_floats(2)}
        @for(j in 0..2)
            @if(o_clamp[i][j])
                @if(j == 0)
                    float texClampS@{i} : TEXCLAMPS@{i};
                @else
                    float texClampT@{i} : TEXCLAMPT@{i};
                @end
                @{update_floats(1)}
            @end
        @end
    @end
@end

@if(o_fog)
float4 fog : FOG;
@{update_floats(4)}
@end
@if(o_grayscale)
float4 grayscale : GRAYSCALE;
@{update_floats(4)}
@end
@if(o_toon)
float3 normal : NORMAL;
@{update_floats(3)}
@end

@for(i in 0..o_inputs)
    @if(o_alpha)
        float4 input@{i + 1} : INPUT@{i};
        @{update_floats(4)}
    @else
        float3 input@{i + 1} : INPUT@{i};
        @{update_floats(3)}
    @end
@end
@if(o_labpbr)
    float3 materialPos : MATERIAL_POS;
    @{update_floats(3)}
@end
};

@if(o_textures[0]) 
    Texture2D g_texture0 : register(t0);
    SamplerState g_sampler0 : register(s0);
    SamplerState g_pointSampler0 : register(s8); // R018: exact 3-point magnification
@end
@if(o_textures[1]) 
    Texture2D g_texture1 : register(t1);
    SamplerState g_sampler1 : register(s1);
    SamplerState g_pointSampler1 : register(s9);
@end

@if(o_masks[0]) Texture2D g_textureMask0 : register(t2);
@if(o_masks[1]) Texture2D g_textureMask1 : register(t3);

@if(o_blend[0]) Texture2D g_textureBlend0 : register(t4);
@if(o_blend[1]) Texture2D g_textureBlend1 : register(t5);

cbuffer PerFrameCB : register(b0) {
    uint noise_frame;
    float noise_scale;
}

// Toon lighting uses b3 because the Unbound primitive-depth buffer owns b2.
// PerFrameCB stays frame-global; only the toon pixel shader declares/reads it. Layout matches the
// PerToonCB C++ struct; each float3 followed by a float fills a 16-byte register per HLSL packing.
@if(o_toon)
cbuffer PerToonCB : register(b3) {
    float3 toon_light_dir;
    float toon_ramp_center;
    float3 toon_light_color;
    float toon_ramp_softness;
    float3 toon_ambient;
    float toon_highlight_intensity;
    float toon_shadow_intensity;
    float toon_debug;
    float2 _toon_pad;
}
@end

float random(in float3 value) {
    float random = dot(value, float3(12.9898, 78.233, 37.719));
    return frac(sin(random) * 143758.5453);
}

// 3 point texture filtering
// Original author: ArthurCarvalho
// Based on GLSL implementation by twinaphex, mupen64plus-libretro project.

@if(o_three_point_filtering && (o_textures[0] || o_textures[1]))
cbuffer PerDrawCB : register(b1) {
    struct {
        uint width;
        uint height;
        bool linear_filtering;
        bool mipmaps; // occupies the existing C++ padding, 16 bytes per texture
    } textures[2];
}

#define TEX_OFFSET(tex, tSampler, texCoord, off, texSize) tex.Sample(tSampler, texCoord - off / texSize)

float4 tex2D3PointFilter(in Texture2D tex, in SamplerState tSampler, in SamplerState pointSampler,
                       in float2 texCoord, in float2 texSize, in bool mipmaps) {
    // Sample before coordinate snapping/branching: implicit derivatives select
    // the correct mip footprint, including anisotropic footprints on slopes.
    // Minification intentionally uses hardware filtering; 3-point is retained
    // for magnification, HUD and the legacy switch-off path.
    float4 reduced = tex.Sample(tSampler, texCoord);
    float2 dx = ddx(texCoord) * texSize;
    float2 dy = ddy(texCoord) * texSize;
    if (mipmaps && max(dot(dx, dx), dot(dy, dy)) > 1.0) {
        return reduced;
    }
    float2 offset = frac(texCoord * texSize - float2(0.5, 0.5));
    offset -= step(1.0, offset.x + offset.y);
    float4 c0 = TEX_OFFSET(tex, pointSampler, texCoord, offset, texSize);
    float4 c1 = TEX_OFFSET(tex, pointSampler, texCoord, float2(offset.x - sign(offset.x), offset.y), texSize);
    float4 c2 = TEX_OFFSET(tex, pointSampler, texCoord, float2(offset.x, offset.y - sign(offset.y)), texSize);
    return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);
}
@end

@if(o_prim_depth)
cbuffer PerPrimDepthCB : register(b2) {
    float prim_depth;
    float3 _pad; // pad to 16-byte cbuffer alignment
}
@end

PSInput VSMain(
    float4 position : POSITION
@for(i in 0..2)
    @if(o_textures[i])
        , float2 uv@{i} : TEXCOORD@{i}
    @end
    @for(j in 0..2)
        @if(o_clamp[i][j])
            @if(j == 0)
                , float texClampS@{i} : TEXCLAMPS@{i}
            @else
                , float texClampT@{i} : TEXCLAMPT@{i}
            @end
        @end
    @end
@end
@if(o_fog)
    , float4 fog : FOG
@end
@if(o_grayscale)
    , float4 grayscale : GRAYSCALE
@end
@if(o_toon)
    , float3 normal : NORMAL
@end
@for(i in 0..o_inputs)
    @if(o_alpha)
        , float4 input@{i + 1} : INPUT@{i}
    @else
        , float3 input@{i + 1} : INPUT@{i}
    @end
@end
@if(o_labpbr)
    , float3 materialPos : MATERIAL_POS
@end
) {
    PSInput result;
    result.position = position;
    @for(i in 0..2)
        @if(o_textures[i])
            result.uv@{i} = uv@{i};
            @for(j in 0..2)
                @if(o_clamp[i][j])
                    @if(j == 0)
                        result.texClampS@{i} = texClampS@{i};
                    @else
                        result.texClampT@{i} = texClampT@{i};
                    @end
                @end
            @end
        @end
    @end

    @if(o_fog)
        result.fog = fog;
    @end

    @if(o_grayscale)
        result.grayscale = grayscale;
    @end

    @if(o_toon)
        result.normal = normal;
    @end

    @for(i in 0..o_inputs)
        @if(o_alpha)
            result.input@{i + 1} = input@{i + 1};
        @else
            result.input@{i + 1} = float4(input@{i + 1}, 1.0);
        @end
    @end
    @if(o_labpbr)
        result.materialPos = materialPos;
    @end

    return result;
}

@if(o_root_signature)
    [RootSignature(RS)]
@end

@if(srgb_mode)
    float4 fromLinear(float4 linearRGB){
        bool3 cutoff = linearRGB.rgb < float3(0.0031308, 0.0031308, 0.0031308);
        float3 higher = 1.055 * pow(linearRGB.rgb, float3(1.0 / 2.4, 1.0 / 2.4, 1.0 / 2.4)) - float3(0.055, 0.055, 0.055);
        float3 lower = linearRGB.rgb * float3(12.92, 12.92, 12.92);
        return float4(lerp(higher, lower, cutoff), linearRGB.a);
    }
@end

#define MOD(x, y) ((x) - (y) * floor((x)/(y)))
#define WRAP(x, low, high) MOD((x)-(low), (high)-(low)) + (low)

@if(o_labpbr)
Texture2D labPbrNormalMap : register(t6);
Texture2D labPbrSpecularMap : register(t7);
SamplerState labPbrSampler : register(s6);

cbuffer PerLabPbrCB : register(b4) {
    float4 labPbrEye;
    float4 labPbrSun;
};

#ifndef LINKSPAN_LABPBR_HLSLI
#define LINKSPAN_LABPBR_HLSLI

struct LabPbrMaterial
{
    float3 normalTS;
    float3 f0;
    float perceptualRoughness;
    float ambientOcclusion;
    float emission;
    float metalness;
};

LabPbrMaterial DecodeLabPbr(float3 albedoLinear, float4 normalAo, float4 specular)
{
    LabPbrMaterial material;

    // O eixo Y segue a tangente do chamador, com V crescente para baixo.
    float2 normalXY = saturate(normalAo.rg) * 2.0 - 1.0;
    float normalXYSquared = dot(normalXY, normalXY);
    material.normalTS = float3(
        normalXY * rsqrt(max(normalXYSquared, 1.0)),
        sqrt(saturate(1.0 - normalXYSquared)));
    material.ambientOcclusion = saturate(normalAo.b);

    material.perceptualRoughness = 1.0 - saturate(specular.r);

    // Os limites ficam entre os códigos dos texels UNORM de 8 bits.
    float f0Code = saturate(specular.g) * 255.0;
    bool isMetal = f0Code >= 229.5;
    float dielectricF0 = min(f0Code, 229.0) / 255.0;
    material.f0 = isMetal ? saturate(albedoLinear) : dielectricF0.xxx;
    material.metalness = isMetal ? 1.0 : 0.0;

    float emissionCode = saturate(specular.a) * 255.0;
    material.emission = emissionCode >= 254.5 ? 0.0 : saturate(emissionCode / 254.0);
    return material;
}

float3 EvaluateLabPbrDirect(
    LabPbrMaterial material, float3 albedoLinear, float3 N, float3 V, float3 L)
{
    float NoL = saturate(dot(N, L));
    float NoV = saturate(dot(N, V));
    if (NoL <= 0.0 || NoV <= 0.0)
    {
        return 0.0;
    }

    float3 halfVector = V + L;
    halfVector *= rsqrt(max(dot(halfVector, halfVector), 1.0e-12));
    float NoH = saturate(dot(N, halfVector));
    float VoH = saturate(dot(V, halfVector));

    static const float Pi = 3.141592653589793;
    float roughness = max(material.perceptualRoughness, 0.045);
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float NoHSquared = NoH * NoH;

    // GGX. A soma preserva alphaSquared quando o meio vetor coincide com N.
    float distributionDenominator = (1.0 - NoHSquared) + NoHSquared * alphaSquared;
    float distribution = alphaSquared / (Pi * distributionDenominator * distributionDenominator);

    // Visibilidade de Smith correlacionada, com o fator 1 / (4 NoL NoV) incluso.
    float viewTerm = NoL * sqrt(lerp(alphaSquared, 1.0, NoV * NoV));
    float lightTerm = NoV * sqrt(lerp(alphaSquared, 1.0, NoL * NoL));
    float visibility = 0.5 / max(viewTerm + lightTerm, 1.0e-6);

    float fresnelBase = 1.0 - VoH;
    float fresnelSquared = fresnelBase * fresnelBase;
    float3 fresnel = material.f0 + (1.0 - material.f0) * (fresnelSquared * fresnelSquared * fresnelBase);
    float3 diffuse = (1.0 - fresnel) * (1.0 - material.metalness) * saturate(albedoLinear) / Pi;

    return (diffuse + distribution * visibility * fresnel) * NoL;
}

#endif

float3 LabPbrToLinear(float3 color) {
    return lerp(pow((color + 0.055) / 1.055, 2.4), color / 12.92, color <= 0.04045);
}

float3 LabPbrToSrgb(float3 color) {
    return lerp(1.055 * pow(color, 1.0 / 2.4) - 0.055, color * 12.92, color <= 0.0031308);
}

// Octahedral encoding of a signed world-space XYZ unit normal. Fold the Z<0
// hemisphere at the XY diagonals, then remap the signed oct coordinates to [0,1].
float2 EncodeKokiriLabPbrNormal(float3 normal) {
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
    float2 oct = normal.xy;
    if (normal.z < 0.0) {
        float2 signs = float2(oct.x < 0.0 ? -1.0 : 1.0, oct.y < 0.0 ? -1.0 : 1.0);
        oct = (1.0 - abs(oct.yx)) * signs;
    }
    return oct * 0.5 + 0.5;
}

float3 ShadeKokiriLabPbr(float3 original, float3 world, float2 uv, float2 sampleUv, float deviceDepth,
                         out float4 materialMark, out float4 materialSpecular) {
    materialMark = 0.0;
    materialSpecular = 0.0;
    float3 dx = ddx(world), dy = ddy(world);
    float2 duvDx = ddx(uv), duvDy = ddy(uv);
    float det = duvDx.x * duvDy.y - duvDx.y * duvDy.x;
    float3 N = cross(dy, dx);
    float3 toEye = labPbrEye.xyz - world;
    float sunLengthSq = dot(labPbrSun.xyz, labPbrSun.xyz);
    if (abs(det) < 1e-8 || dot(N, N) < 1e-12 || dot(toEye, toEye) < 1e-8 || sunLengthSq < 1e-8)
        return original;

    float3 V = normalize(toEye);
    N = normalize(N);
    if (dot(N, V) < 0.0)
        N = -N;
    float orientation = det < 0.0 ? -1.0 : 1.0;
    float3 T = (dx * duvDy.y - dy * duvDx.y) * orientation;
    float3 B = (dy * duvDx.x - dx * duvDy.x) * orientation;
    T -= N * dot(T, N);
    if (dot(T, T) < 1e-16 || dot(B, B) < 1e-16)
        return original;
    T = normalize(T);
    float3 orthogonalB = cross(N, T);
    B = dot(orthogonalB, B) < 0.0 ? -orthogonalB : orthogonalB;

    uint2 specSize, normalSize;
    labPbrSpecularMap.GetDimensions(specSize.x, specSize.y);
    labPbrNormalMap.GetDimensions(normalSize.x, normalSize.y);
    if (any(specSize == 0) || any(normalSize == 0))
        return original;

    float2 normalUv = sampleUv;
    float2 specUv = frac(sampleUv);
    if (labPbrSun.w != 0.0) {
        // Clamp to texel centers so the wrap sampler never blends across the T edge.
        float halfTexelT = 0.5 / normalSize.y;
        normalUv.y = clamp(normalUv.y, halfTexelT, 1.0 - halfTexelT);
        specUv.y = saturate(sampleUv.y);
    }
    float4 normalAo = labPbrNormalMap.SampleLevel(labPbrSampler, normalUv, 0);
    int2 specCoord = min(int2(specUv * specSize), int2(specSize) - 1);
    float4 specular = labPbrSpecularMap.Load(int3(specCoord, 0));
    float footprint = max(length(duvDx * normalSize), length(duvDy * normalSize));
    normalAo.rg = lerp(normalAo.rg, 0.5, smoothstep(1.0, 4.0, footprint));

    float3 color = LabPbrToLinear(saturate(original));
    LabPbrMaterial material = DecodeLabPbr(color, normalAo, specular);
    float3 mappedN = normalize(material.normalTS.x * T + material.normalTS.y * B + material.normalTS.z * N);
    if (labPbrEye.w != 0.0) {
        materialMark = float4(EncodeKokiriLabPbrNormal(mappedN), material.ambientOcclusion, deviceDepth + 1.0);
        materialSpecular = float4(material.f0, material.perceptualRoughness);
    }
    float3 L = labPbrSun.xyz * rsqrt(sunLengthSq);
    float daylight = smoothstep(0.01, 0.25, L.y);
    float3 mappedLight = EvaluateLabPbrDirect(material, color, mappedN, V, L);
    float3 flatLight = EvaluateLabPbrDirect(material, color, N, V, L);
    // The combiner output is already lit. Keep the extra relief relative to that base
    // so grazing GGX peaks cannot burn in white dots or subtract the surface to black.
    float3 base = color * lerp(1.0, material.ambientOcclusion, 0.35);
    float3 response = (mappedLight - flatLight) * (0.35 * daylight);
    float3 darkLimit = 0.22 * base;
    float3 lightLimit = min(0.32 * base, 0.26 * (1.0 - base));
    float3 headroom = lerp(darkLimit, lightLimit, step(0.0, response));
    float3 correction = response * (headroom / max(headroom + abs(response), 1e-6));
    float3 relit = base + correction + color * material.emission;
    return LabPbrToSrgb(saturate(relit));
}
@end

struct PSOutput {
    float4 color : SV_TARGET;
    @if(o_labpbr)
    float4 material : SV_Target1;
    float4 materialSpecular : SV_Target2;
    @end
    @if(o_prim_depth)
    float depth : SV_Depth;
    @end
};

#ifdef LINKSPAN_LABPBR_MSAA
PSOutput PSMain(PSInput input) {
    const float4 screenSpace = input.position;
#else
PSOutput PSMain(PSInput input, float4 screenSpace : SV_Position) {
#endif
    @for(i in 0..2)
        @if(o_textures[i])
            float2 tc@{i} = input.uv@{i};
            @{s = o_clamp[i][0]}
            @{t = o_clamp[i][1]}
            @if(s || t)
                int2 texSize@{i};
                g_texture@{i}.GetDimensions(texSize@{i}.x, texSize@{i}.y);
                @if(s && t)
                    tc@{i} = clamp(tc@{i}, 0.5 / texSize@{i}, float2(input.texClampS@{i}, input.texClampT@{i}));
                @elseif(s)
                    tc@{i} = float2(clamp(tc@{i}.x, 0.5 / texSize@{i}.x, input.texClampS@{i}), tc@{i}.y);
                @else
                    tc@{i} = float2(tc@{i}.x, clamp(tc@{i}.y, 0.5 / texSize@{i}.y, input.texClampT@{i}));
                @end
            @end

            @if(o_three_point_filtering)
                float4 texVal@{i};
                if (textures[@{i}].linear_filtering) {
                    @if(o_masks[i])
                        texVal@{i} = tex2D3PointFilter(g_texture@{i}, g_sampler@{i}, g_pointSampler@{i}, tc@{i}, float2(textures[@{i}].width, textures[@{i}].height), textures[@{i}].mipmaps);
                        float2 maskSize@{i};
                        g_textureMask@{i}.GetDimensions(maskSize@{i}.x, maskSize@{i}.y);
                        float4 maskVal@{i} = tex2D3PointFilter(g_textureMask@{i}, g_sampler@{i}, g_pointSampler@{i}, tc@{i}, maskSize@{i}, textures[@{i}].mipmaps);
                        @if(o_blend[i])
                            float4 blendVal@{i} = tex2D3PointFilter(g_textureBlend@{i}, g_sampler@{i}, g_pointSampler@{i}, tc@{i}, float2(textures[@{i}].width, textures[@{i}].height), textures[@{i}].mipmaps);
                        @else
                            float4 blendVal@{i} = float4(0, 0, 0, 0);
                        @end

                        texVal@{i} = lerp(texVal@{i}, blendVal@{i}, maskVal@{i}.a);
                    @else
                        texVal@{i} = tex2D3PointFilter(g_texture@{i}, g_sampler@{i}, g_pointSampler@{i}, tc@{i}, float2(textures[@{i}].width, textures[@{i}].height), textures[@{i}].mipmaps);
                    @end
                } else {
                    texVal@{i} = g_texture@{i}.Sample(g_sampler@{i}, tc@{i});
                    @if(o_masks[i])
                        @if(o_blend[i])
                            float4 blendVal@{i} = g_textureBlend@{i}.Sample(g_sampler@{i}, tc@{i});
                        @else
                            float4 blendVal@{i} = float4(0, 0, 0, 0);
                        @end
                        texVal@{i} = lerp(texVal@{i}, blendVal@{i}, g_textureMask@{i}.Sample(g_sampler@{i}, tc@{i}).a);
                    @end
                }
            @else
                float4 texVal@{i} = g_texture@{i}.Sample(g_sampler@{i}, tc@{i});
                @if(o_masks[i])
                    @if(o_blend[i])
                        float4 blendVal@{i} = g_textureBlend@{i}.Sample(g_sampler@{i}, tc@{i});
                    @else
                        float4 blendVal@{i} = float4(0, 0, 0, 0);
                    @end
                    texVal@{i} = lerp(texVal@{i}, blendVal@{i}, g_textureMask@{i}.Sample(g_sampler@{i}, tc@{i}).a);
                @end
            @end
        @end
    @end

    @if(o_alpha) 
        float4 texel;
    @else
        float3 texel;
    @end

    @if(o_2cyc)
        @{f_range = 2}
    @else
        @{f_range = 1}
    @end

    @for(c in 0..f_range)
        @if(c == 1)
            @if(o_alpha)
                @if(o_c[c][1][2] == SHADER_COMBINED)
                    texel.a = WRAP(texel.a, -1.01, 1.01);
                @else
                    texel.a = WRAP(texel.a, -0.51, 1.51);
                @end
            @end

            @if(o_c[c][0][2] == SHADER_COMBINED)
                texel.rgb = WRAP(texel.rgb, -1.01, 1.01);
            @else
                texel.rgb = WRAP(texel.rgb, -0.51, 1.51);
            @end
        @end

        @if(!o_color_alpha_same[c] && o_alpha)
            texel = float4(@{
            append_formula(o_c[c], o_do_single[c][0],
                           o_do_multiply[c][0], o_do_mix[c][0], false, false, true, c == 0)
            }, @{append_formula(o_c[c], o_do_single[c][1],
                           o_do_multiply[c][1], o_do_mix[c][1], true, true, true, c == 0)
            });
        @else
            texel = @{append_formula(o_c[c], o_do_single[c][0],
                           o_do_multiply[c][0], o_do_mix[c][0], o_alpha, false,
                           o_alpha, c == 0)};
        @end
    @end

    @if(o_texture_edge && o_alpha)
        if (texel.a > 0.19) texel.a = 1.0; else discard;
    @end

    texel = WRAP(texel, -0.51, 1.51);
    texel = clamp(texel, 0.0, 1.0);
    // TODO discard if alpha is 0?

    @if(o_labpbr)
        float4 materialMark, materialSpecular;
        @if(o_prim_depth)
            float materialDepth = prim_depth;
        @else
            float materialDepth = screenSpace.z;
        @end
        texel.rgb = ShadeKokiriLabPbr(texel.rgb, input.materialPos, input.uv0, tc0, saturate(materialDepth),
                                     materialMark, materialSpecular);
    @end

    // SOH [Enhancement] Toon lighting: re-light the (white-shaded) albedo with the single dominant
    // light through a soft half-Lambert ramp.
    @if(o_toon)
        float3 toonN = normalize(input.normal);
        float toonNL = dot(toonN, normalize(toon_light_dir)) * 0.5 + 0.5;
        float toonRamp = smoothstep(toon_ramp_center - toon_ramp_softness, toon_ramp_center + toon_ramp_softness, toonNL);
        float3 toonLit = toon_ambient + toon_light_color * toon_highlight_intensity;
        float3 toonShadow = lerp(toonLit, toon_ambient, toon_shadow_intensity);
        if (toon_debug > 0.5) {
            // Diagnostic view: flat white on the lit side of the ramp, flat black in shadow, albedo
            // discarded — makes it obvious which draws are receiving toon lighting.
            texel.rgb = float3(toonRamp, toonRamp, toonRamp);
        } else {
            texel.rgb = clamp(texel.rgb * lerp(toonShadow, toonLit, toonRamp), 0.0, 1.0);
        }
    @end

    @if(o_fog)
        @if(o_alpha)
            texel = float4(lerp(texel.rgb, input.fog.rgb, input.fog.a), texel.a);
        @else
            texel = lerp(texel, input.fog.rgb, input.fog.a);
        @end
    @end

    @if(o_grayscale)
        float intensity = (texel.r + texel.g + texel.b) / 3.0;
        float3 new_texel = input.grayscale.rgb * intensity;
        texel.rgb = lerp(texel.rgb, new_texel, input.grayscale.a);
    @end

    @if(o_alpha && o_noise)
        float2 coords = screenSpace.xy * noise_scale;
        texel.a *= round(saturate(random(float3(floor(coords), noise_frame)) + texel.a - 0.5));
    @end

    @if(o_alpha)
        @if(o_alpha_threshold)
            if (texel.a < 8.0 / 256.0) discard;
        @end
        @if(o_invisible)
            texel.a = 0.0;
        @end
    @end

    PSOutput output;
    @if(o_labpbr)
        output.material = materialMark;
        output.materialSpecular = materialSpecular;
    @end
    @if(o_alpha)
        @if(srgb_mode)
            output.color = fromLinear(texel);
        @else
            output.color = texel;
        @end
    @else
        @if(srgb_mode)
            output.color = fromLinear(float4(texel, 1.0));
        @else
            output.color = float4(texel, 1.0);
        @end
    @end
    @if(o_prim_depth)
        output.depth = prim_depth;
    @end
    return output;
}
