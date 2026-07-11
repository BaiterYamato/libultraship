#include "ship/window/Window.h"
#ifdef ENABLE_OPENGL

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>
#include <vector>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif

#ifdef __MINGW32__
#define FOR_WINDOWS 1
#else
#define FOR_WINDOWS 0
#endif

#include "fast/backends/gfx_opengl.h"
#include "ship/window/gui/Gui.h"
#include <prism/processor.h>
#include <fstream>
#include "ship/Context.h"
#include "ship/resource/factory/ShaderFactory.h"
#include "fast/interpreter.h"
#include "ship/config/ConsoleVariable.h"
#include <spdlog/spdlog.h>

namespace Fast {
namespace {
constexpr int32_t kAoFallbackNone = 0;
constexpr int32_t kAoFallbackBackendUnsupportedMetal = 1;
constexpr int32_t kAoFallbackShaderCompileFailed = 2;
constexpr int32_t kAoFallbackRuntimeUnsupported = 3;

GLuint CompileShader(GLenum shaderType, const char* source) {
    GLuint shader = glCreateShader(shaderType);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_TRUE) {
        return shader;
    }

    GLint logLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    std::vector<char> log(static_cast<size_t>(std::max(logLength, 1)));
    glGetShaderInfoLog(shader, logLength, nullptr, log.data());
    SPDLOG_ERROR("[FAST][AO] OpenGL shader compile failed: {}", log.data());
    glDeleteShader(shader);
    return 0;
}

GLuint LinkProgram(GLuint vertexShader, GLuint fragmentShader) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_TRUE) {
        return program;
    }

    GLint logLength = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
    std::vector<char> log(static_cast<size_t>(std::max(logLength, 1)));
    glGetProgramInfoLog(program, logLength, nullptr, log.data());
    SPDLOG_ERROR("[FAST][AO] OpenGL program link failed: {}", log.data());
    glDeleteProgram(program);
    return 0;
}

const char* kFullscreenTriangleVs = R"(
#version 330 core
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kAoGenerateFs = R"(
#version 330 core
uniform sampler2D uDepthTex;
uniform vec2 uInvResolution;
uniform float uRadiusPixels;
uniform float uIntensity;
uniform float uBias;
uniform float uPower;
uniform float uMaxDistance;
uniform int uSampleCount;
out float outAo;

float ReadDepth(vec2 uv) {
    return texture(uDepthTex, clamp(uv, vec2(0.0), vec2(1.0))).r;
}

void main() {
    vec2 uv = gl_FragCoord.xy * uInvResolution;
    float centerDepth = ReadDepth(uv);
    if (centerDepth >= 0.9999) {
        outAo = 1.0;
        return;
    }

    vec2 kernel[16] = vec2[16](
        vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0),
        vec2(0.707, 0.707), vec2(-0.707, 0.707), vec2(0.707, -0.707), vec2(-0.707, -0.707),
        vec2(2.0, 0.0), vec2(-2.0, 0.0), vec2(0.0, 2.0), vec2(0.0, -2.0),
        vec2(1.5, 0.5), vec2(-1.5, 0.5), vec2(1.5, -0.5), vec2(-1.5, -0.5)
    );

    float occlusion = 0.0;
    float maxDistanceDepth = uMaxDistance * 0.0001;
    int samples = clamp(uSampleCount, 4, 16);
    for (int i = 0; i < samples; i++) {
        vec2 offset = kernel[i] * uRadiusPixels * uInvResolution;
        float sampleDepth = ReadDepth(uv + offset);
        float delta = sampleDepth - centerDepth;
        if (delta > uBias && delta < maxDistanceDepth) {
            occlusion += 1.0;
        }
    }
    float ao = 1.0 - (occlusion / float(samples)) * uIntensity;
    outAo = pow(clamp(ao, 0.0, 1.0), max(uPower, 0.0001));
}
)";

const char* kAoBlurFs = R"(
#version 330 core
uniform sampler2D uAoTex;
uniform vec2 uInvResolution;
uniform vec2 uDirection;
out float outAo;

void main() {
    vec2 uv = gl_FragCoord.xy * uInvResolution;
    float w0 = 0.227027f;
    float w1 = 0.316216f;
    float w2 = 0.070270f;
    float result = texture(uAoTex, uv).r * w0;
    result += texture(uAoTex, uv + uDirection * 1.384615f * uInvResolution).r * w1;
    result += texture(uAoTex, uv - uDirection * 1.384615f * uInvResolution).r * w1;
    result += texture(uAoTex, uv + uDirection * 3.230769f * uInvResolution).r * w2;
    result += texture(uAoTex, uv - uDirection * 3.230769f * uInvResolution).r * w2;
    outAo = clamp(result, 0.0, 1.0);
}
)";

const char* kAoCompositeFs = R"(
#version 330 core
uniform sampler2D uAoTex;
uniform vec2 uInvResolution;
uniform int uDebugView;
out vec4 outColor;

void main() {
    vec2 uv = gl_FragCoord.xy * uInvResolution;
    float ao = texture(uAoTex, uv).r;
    if (uDebugView == 1) {
        outColor = vec4(vec3(ao), 1.0);
    } else {
        outColor = vec4(ao, ao, ao, 1.0);
    }
}
)";

constexpr int32_t kVolumetricsFallbackNone = 0;
constexpr int32_t kVolumetricsFallbackBackendUnsupportedMetal = 1;
constexpr int32_t kVolumetricsFallbackShaderCompileFailed = 2;
constexpr int32_t kVolumetricsFallbackRuntimeUnsupported = 3;

const char* kVolumetricsRaymarchFs = R"(
#version 330 core
#define MAX_LIGHTS 8
#define DEBUG_OFF 0
#define DEBUG_FOG_DENSITY 1
#define DEBUG_LIGHT_ENERGY 2
#define DEBUG_FINAL_VOLUME 3
uniform sampler2D uDepthTex;
uniform sampler2D uPrevVolumeTex;
uniform vec2 uInvFullResolution;
uniform vec2 uInvLowResolution;
uniform vec3 uCameraWorldPosition;
uniform float uDensity;
uniform vec3 uFogColor;
uniform float uAmbientIntensity;
uniform float uAnisotropy;
uniform float uStartDistance;
uniform float uMaxDistance;
uniform float uBaseHeight;
uniform float uHeightFalloff;
uniform float uLightShaftIntensity;
uniform float uShadowIntensity;
uniform float uTemporalBlend;
uniform float uJitterScale;
uniform int uStepCount;
uniform int uLightCount;
uniform int uShadowQuality;
uniform int uHistoryValid;
uniform int uHeightFogEnabled;
uniform int uDebugView;
uniform int uDebugMode;
uniform vec2 uDepthNdcParams;
uniform vec2 uNdcDepthParams;
uniform mat4 uCurrViewProj;
uniform mat4 uInvViewProj;
uniform mat4 uPrevViewProj;
uniform vec4 uLightPositionRadius[MAX_LIGHTS];
uniform vec4 uLightDirectionIntensity[MAX_LIGHTS];
uniform vec4 uLightColorVolumetric[MAX_LIGHTS];
uniform vec4 uLightConeShadow[MAX_LIGHTS];
uniform vec4 uLightShadowParams[MAX_LIGHTS];
layout(location = 0) out vec4 outColor;
layout(location = 1) out float outDepth;

float ReadDepth(vec2 uv) {
    return texture(uDepthTex, clamp(uv, vec2(0.0), vec2(1.0))).r;
}

float Noise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec3 ReconstructWorldPosition(vec2 uv, float depth) {
    float ndcZ = depth * uDepthNdcParams.x + uDepthNdcParams.y;
    vec4 clip = vec4(uv * 2.0 - 1.0, ndcZ, 1.0);
    vec4 world = uInvViewProj * clip;
    return world.xyz / max(world.w, 0.0001);
}

bool ProjectWorld(mat4 viewProj, vec3 worldPos, out vec2 uv, out float deviceDepth) {
    vec4 clip = viewProj * vec4(worldPos, 1.0);
    if (clip.w <= 0.0001) {
        uv = vec2(-1.0);
        deviceDepth = 1.0;
        return false;
    }
    vec3 ndc = clip.xyz / clip.w;
    uv = ndc.xy * 0.5 + 0.5;
    deviceDepth = ndc.z * uNdcDepthParams.x + uNdcDepthParams.y;
    return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
}

float PhaseHG(float cosTheta, float g) {
    float g2 = g * g;
    float denom = pow(max(1.0 + g2 - 2.0 * g * cosTheta, 0.001), 1.5);
    return (1.0 - g2) / max(12.5663706 * denom, 0.001);
}

float ComputeShadow(int lightIndex, vec3 sampleWorldPos) {
    if (uShadowQuality <= 0 || uLightShadowParams[lightIndex].w < 0.5) {
        return 1.0;
    }

    vec3 traceDirection = vec3(0.0);
    float maxTraceDistance = max(uLightConeShadow[lightIndex].w, 0.0);
    int lightType = int(uLightShadowParams[lightIndex].z + 0.5);
    if (lightType == 0) {
        traceDirection = normalize(-uLightDirectionIntensity[lightIndex].xyz);
    } else {
        vec3 toLight = uLightPositionRadius[lightIndex].xyz - sampleWorldPos;
        float lengthToLight = length(toLight);
        if (lengthToLight <= 0.001) {
            return 1.0;
        }
        traceDirection = toLight / lengthToLight;
        maxTraceDistance = min(maxTraceDistance, lengthToLight);
    }
    if (maxTraceDistance <= 0.001) {
        return 1.0;
    }

    float occlusion = 0.0;
    int shadowSamples = uShadowQuality == 1 ? 2 : (uShadowQuality == 2 ? 3 : 4);
    float shadowBias = uLightConeShadow[lightIndex].z + uLightShadowParams[lightIndex].x * 0.01;
    for (int sampleIndex = 1; sampleIndex <= shadowSamples; ++sampleIndex) {
        float travel = maxTraceDistance * (float(sampleIndex) / float(shadowSamples));
        vec3 probeWorldPos = sampleWorldPos + traceDirection * travel;
        vec2 probeUv;
        float probeDepth;
        if (!ProjectWorld(uCurrViewProj, probeWorldPos, probeUv, probeDepth)) {
            continue;
        }
        float sceneDepth = ReadDepth(probeUv);
        if (sceneDepth + shadowBias < probeDepth) {
            occlusion += 1.0;
        }
    }

    return clamp(1.0 - (occlusion / float(shadowSamples)) * uShadowIntensity, 0.0, 1.0);
}

float ComputeLightAttenuation(int lightIndex, vec3 sampleWorldPos, out vec3 lightDir) {
    int lightType = int(uLightShadowParams[lightIndex].z + 0.5);
    lightDir = normalize(-uLightDirectionIntensity[lightIndex].xyz);
    if (lightType == 0) {
        return 1.0;
    }

    vec3 toLight = uLightPositionRadius[lightIndex].xyz - sampleWorldPos;
    float distanceToLight = length(toLight);
    if (distanceToLight <= 0.001) {
        return 0.0;
    }
    lightDir = toLight / distanceToLight;
    float radius = max(uLightPositionRadius[lightIndex].w, 0.001);
    float attenuation = clamp(1.0 - distanceToLight / radius, 0.0, 1.0);
    attenuation *= attenuation;

    if (lightType == 2) {
        float coneDot = dot(normalize(sampleWorldPos - uLightPositionRadius[lightIndex].xyz),
                            normalize(uLightDirectionIntensity[lightIndex].xyz));
        float innerCos = uLightConeShadow[lightIndex].x;
        float outerCos = uLightConeShadow[lightIndex].y;
        float coneAttenuation = smoothstep(outerCos, innerCos, coneDot);
        attenuation *= coneAttenuation;
    }

    return attenuation;
}

vec3 BuildFogDensityDebugColor(float densityIntegral, float fogAmount) {
    float densityViz = clamp(densityIntegral * 5.0, 0.0, 1.0);
    float fogViz = clamp(max(fogAmount, densityViz), 0.0, 1.0);
    vec3 baseColor = max(uFogColor, vec3(0.08, 0.08, 0.08));
    return mix(vec3(0.02, 0.02, 0.025), clamp(baseColor * 1.35, 0.0, 1.0), fogViz);
}

vec3 BuildLightEnergyDebugColor(float lightingLuma, float fogAmount) {
    float lightingViz = clamp(lightingLuma * 4.0, 0.0, 1.0);
    vec3 energyColor = mix(vec3(0.02, 0.03, 0.07), vec3(1.0, 0.86, 0.22), lightingViz);
    return max(energyColor * max(fogAmount, 0.4), vec3(0.02, 0.02, 0.02));
}

void main() {
    vec2 uv = gl_FragCoord.xy * uInvLowResolution;
    float fullDepth = ReadDepth(uv);
    outDepth = fullDepth;
    if (fullDepth >= 0.9999) {
        outColor = vec4(0.0);
        return;
    }

    vec3 surfaceWorldPos = ReconstructWorldPosition(uv, fullDepth);
    vec3 viewVector = surfaceWorldPos - uCameraWorldPosition;
    float surfaceDistance = min(length(viewVector), uMaxDistance);
    if (surfaceDistance <= uStartDistance + 0.001) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 rayDirection = normalize(viewVector);
    int stepCount = clamp(uStepCount, 8, 64);
    float stepSize = (surfaceDistance - uStartDistance) / float(stepCount);
    float jitter = (Noise(gl_FragCoord.xy) - 0.5) * uJitterScale;
    float t = uStartDistance + stepSize * (0.5 + jitter);
    vec3 volumeAccum = vec3(0.0);
    float transmittance = 1.0;
    float densityIntegral = 0.0;
    float lightingLuma = 0.0;

    for (int stepIndex = 0; stepIndex < 64; ++stepIndex) {
        if (stepIndex >= stepCount || t >= surfaceDistance || t >= uMaxDistance) {
            break;
        }

        vec3 sampleWorldPos = uCameraWorldPosition + rayDirection * t;
        float sampleDensity = uDensity;
        if (uHeightFogEnabled == 1) {
            sampleDensity *= exp(-max(sampleWorldPos.y - uBaseHeight, 0.0) * uHeightFalloff);
        }
        sampleDensity = max(sampleDensity, 0.0);
        float mediumStep = sampleDensity * stepSize * 0.01;
        if (mediumStep > 0.0) {
            densityIntegral += mediumStep;
            vec3 stepLighting = vec3(0.0);
            for (int lightIndex = 0; lightIndex < MAX_LIGHTS; ++lightIndex) {
                if (lightIndex >= uLightCount) {
                    break;
                }
                vec3 lightDir;
                float attenuation = ComputeLightAttenuation(lightIndex, sampleWorldPos, lightDir);
                if (attenuation <= 0.0001) {
                    continue;
                }
                float visibility = ComputeShadow(lightIndex, sampleWorldPos);
                float phase = PhaseHG(dot(lightDir, -rayDirection), uAnisotropy);
                vec3 lightColor = uLightColorVolumetric[lightIndex].rgb;
                float lightIntensity = uLightDirectionIntensity[lightIndex].w * uLightColorVolumetric[lightIndex].a;
                stepLighting += lightColor * (lightIntensity * attenuation * visibility * phase);
            }

            float fogStepAlpha = clamp(1.0 - exp(-mediumStep), 0.0, 1.0);
            vec3 mediumFog = uFogColor * uAmbientIntensity * fogStepAlpha;
            vec3 scattering = stepLighting * (mediumStep * uLightShaftIntensity);
            lightingLuma += dot(scattering, vec3(0.2126, 0.7152, 0.0722));
            volumeAccum += (mediumFog + scattering) * transmittance;
            transmittance *= exp(-mediumStep * 1.35);
            if (transmittance <= 0.01) {
                break;
            }
        }
        t += stepSize;
    }

    int debugMode = uDebugMode;
    if (debugMode == DEBUG_OFF && uDebugView == 1) {
        debugMode = DEBUG_FINAL_VOLUME;
    }

    vec3 finalVolume = max(volumeAccum, vec3(0.0));
    if (debugMode == DEBUG_OFF && uHistoryValid == 1 && uTemporalBlend > 0.0) {
        vec2 prevUv;
        float prevDepth;
        if (ProjectWorld(uPrevViewProj, surfaceWorldPos, prevUv, prevDepth)) {
            vec3 prevColor = texture(uPrevVolumeTex, prevUv).rgb;
            finalVolume = mix(finalVolume, prevColor, uTemporalBlend);
        }
    }

    float fogAmount = clamp(1.0 - transmittance, 0.0, 1.0);
    if (debugMode == DEBUG_FOG_DENSITY) {
        outColor = vec4(BuildFogDensityDebugColor(densityIntegral, fogAmount), 1.0);
    } else if (debugMode == DEBUG_LIGHT_ENERGY) {
        outColor = vec4(BuildLightEnergyDebugColor(lightingLuma, fogAmount), 1.0);
    } else {
        outColor = vec4(finalVolume, 1.0);
    }
}
)";

const char* kVolumetricsCompositeFs = R"(
#version 330 core
#define DEBUG_OFF 0
#define DEBUG_FOG_DENSITY 1
#define DEBUG_LIGHT_ENERGY 2
#define DEBUG_FINAL_VOLUME 3
uniform sampler2D uVolumeTex;
uniform sampler2D uLowDepthTex;
uniform sampler2D uFullDepthTex;
uniform vec2 uInvFullResolution;
uniform vec2 uInvLowResolution;
uniform int uDebugView;
uniform int uDebugMode;
out vec4 outColor;

float ReadFullDepth(vec2 uv) {
    return texture(uFullDepthTex, clamp(uv, vec2(0.0), vec2(1.0))).r;
}

vec3 SampleUpscaled(vec2 uv, float fullDepth) {
    vec2 lowSize = vec2(1.0 / uInvLowResolution.x, 1.0 / uInvLowResolution.y);
    vec2 pixel = uv * lowSize - 0.5;
    vec2 base = floor(pixel);
    vec2 frac = fract(pixel);
    vec3 accum = vec3(0.0);
    float weightSum = 0.0;
    for (int oy = 0; oy < 2; ++oy) {
        for (int ox = 0; ox < 2; ++ox) {
            vec2 coord = base + vec2(float(ox), float(oy)) + 0.5;
            vec2 sampleUv = clamp(coord * uInvLowResolution, vec2(0.0), vec2(1.0));
            float bilinear = (ox == 0 ? 1.0 - frac.x : frac.x) * (oy == 0 ? 1.0 - frac.y : frac.y);
            float lowDepth = texture(uLowDepthTex, sampleUv).r;
            float depthWeight = exp(-abs(lowDepth - fullDepth) * 128.0);
            float weight = max(bilinear * depthWeight, 0.0001);
            accum += texture(uVolumeTex, sampleUv).rgb * weight;
            weightSum += weight;
        }
    }
    return accum / max(weightSum, 0.0001);
}

void main() {
    vec2 uv = gl_FragCoord.xy * uInvFullResolution;
    float fullDepth = ReadFullDepth(uv);
    vec3 volume = max(SampleUpscaled(uv, fullDepth), vec3(0.0));
    int debugMode = uDebugMode;
    if (debugMode == DEBUG_OFF && uDebugView == 1) {
        debugMode = DEBUG_FINAL_VOLUME;
    }
    if (debugMode != DEBUG_OFF || uDebugView == 1) {
        outColor = vec4(pow(clamp(volume, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2)), 1.0);
    } else {
        outColor = vec4(volume, 1.0);
    }
}
)";
} // namespace

int GfxRenderingAPIOGL::GetMaxTextureSize() {
    GLint max_texture_size;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    return max_texture_size;
}

const char* GfxRenderingAPIOGL::GetName() {
    return "OpenGL";
}

GfxClipParameters GfxRenderingAPIOGL::GetClipParameters() {
    return { false, mFrameBuffers[mCurrentFrameBuffer].invertY };
}

static void VertexArraySetAttribs(ShaderProgram* prg) {
    size_t numFloats = prg->numFloats;
    size_t pos = 0;

    for (int i = 0; i < prg->numAttribs; i++) {
        glEnableVertexAttribArray(prg->attribLocations[i]);
        glVertexAttribPointer(prg->attribLocations[i], prg->attribSizes[i], GL_FLOAT, GL_FALSE,
                              numFloats * sizeof(float), (void*)(pos * sizeof(float)));
        pos += prg->attribSizes[i];
    }
}

void GfxRenderingAPIOGL::SetUniforms(ShaderProgram* prg) const {
    glUniform1i(prg->frameCountLocation, mFrameCount);
    glUniform1f(prg->noiseScaleLocation, mCurrentNoiseScale);
}

void GfxRenderingAPIOGL::SetPerDrawUniforms() {
    if (mCurrentShaderProgram->usedTextures[0] || mCurrentShaderProgram->usedTextures[1]) {
        GLint filtering[2] = { textures[mCurrentTextureIds[0]].filtering, textures[mCurrentTextureIds[1]].filtering };
        glUniform1iv(mCurrentShaderProgram->texture_filtering_location, 2, filtering);

        GLint width[2] = { textures[mCurrentTextureIds[0]].width, textures[mCurrentTextureIds[1]].width };
        glUniform1iv(mCurrentShaderProgram->texture_width_location, 2, width);

        GLint height[2] = { textures[mCurrentTextureIds[0]].height, textures[mCurrentTextureIds[1]].height };
        glUniform1iv(mCurrentShaderProgram->texture_height_location, 2, height);
    }
}

void GfxRenderingAPIOGL::UnloadShader(ShaderProgram* old_prg) {
    if (old_prg != nullptr) {
        for (unsigned int i = 0; i < old_prg->numAttribs; i++) {
            glDisableVertexAttribArray(old_prg->attribLocations[i]);
        }
    }
}

void GfxRenderingAPIOGL::LoadShader(ShaderProgram* new_prg) {
    // if (!new_prg) return;
    mCurrentShaderProgram = new_prg;
    glUseProgram(new_prg->openglProgramId);
    VertexArraySetAttribs(new_prg);
    SetUniforms(new_prg);
}

#define RAND_NOISE "((random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + 1.0) / 2.0)"

static const char* shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha,
                                      bool first_cycle, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "vec4(1.0, 1.0, 1.0, 1.0)" : "vec3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return first_cycle ? (with_alpha ? "texVal0" : "texVal0.rgb")
                                   : (with_alpha ? "texVal1" : "texVal1.rgb");
            case SHADER_TEXEL0A:
                return first_cycle
                           ? (hint_single_element ? "texVal0.a"
                                                  : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                                : "vec3(texVal0.a, texVal0.a, texVal0.a)"))
                           : (hint_single_element ? "texVal1.a"
                                                  : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                                : "vec3(texVal1.a, texVal1.a, texVal1.a)"));
            case SHADER_TEXEL1A:
                return first_cycle
                           ? (hint_single_element ? "texVal1.a"
                                                  : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                                : "vec3(texVal1.a, texVal1.a, texVal1.a)"))
                           : (hint_single_element ? "texVal0.a"
                                                  : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                                : "vec3(texVal0.a, texVal0.a, texVal0.a)"));
            case SHADER_TEXEL1:
                return first_cycle ? (with_alpha ? "texVal1" : "texVal1.rgb")
                                   : (with_alpha ? "texVal0" : "texVal0.rgb");
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:
                return with_alpha ? "vec4(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")"
                                  : "vec3(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
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
    return "";
}

bool get_bool(prism::ContextTypes* value) {
    if (std::holds_alternative<int>(*value)) {
        return std::get<int>(*value) == 1;
    }
    return false;
}

prism::ContextTypes* append_formula(prism::ContextTypes* _, prism::ContextTypes* a_arg, prism::ContextTypes* a_single,
                                    prism::ContextTypes* a_mult, prism::ContextTypes* a_mix,
                                    prism::ContextTypes* a_with_alpha, prism::ContextTypes* a_only_alpha,
                                    prism::ContextTypes* a_alpha, prism::ContextTypes* a_first_cycle) {
    auto c = std::get<prism::MTDArray<int>>(*a_arg);
    bool do_single = get_bool(a_single);
    bool do_multiply = get_bool(a_mult);
    bool do_mix = get_bool(a_mix);
    bool with_alpha = get_bool(a_with_alpha);
    bool only_alpha = get_bool(a_only_alpha);
    bool opt_alpha = get_bool(a_alpha);
    bool first_cycle = get_bool(a_first_cycle);
    std::string out = "";
    if (do_single) {
        out += shader_item_to_str(c.at(only_alpha, 3), with_alpha, only_alpha, opt_alpha, first_cycle, false);
    } else if (do_multiply) {
        out += shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += " * ";
        out += shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
    } else if (do_mix) {
        out += "mix(";
        out += shader_item_to_str(c.at(only_alpha, 1), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ", ";
        out += shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ", ";
        out += shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
        out += ")";
    } else {
        out += "(";
        out += shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += " - ";
        out += shader_item_to_str(c.at(only_alpha, 1), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ") * ";
        out += shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
        out += " + ";
        out += shader_item_to_str(c.at(only_alpha, 3), with_alpha, only_alpha, opt_alpha, first_cycle, false);
    }
    return new prism::ContextTypes{ out };
}

std::optional<std::string> opengl_include_fs(const std::string& path) {
    auto init = std::make_shared<Ship::ResourceInitData>();
    init->Type = (uint32_t)Ship::ResourceType::Shader;
    init->ByteOrder = Ship::Endianness::Native;
    init->Format = RESOURCE_FORMAT_BINARY;
    auto res = std::static_pointer_cast<Ship::Shader>(
        Ship::Context::GetInstance()->GetResourceManager()->LoadResource(path, true, init));
    if (res == nullptr) {
        return std::nullopt;
    }
    auto inc = static_cast<std::string*>(res->GetRawPointer());
    return *inc;
}

std::string GfxRenderingAPIOGL::BuildFsShader(const CCFeatures& cc_features) {
    prism::Processor processor;
    prism::ContextItems mContext = {
        { "o_c", M_ARRAY(cc_features.c, int, 2, 2, 4) },
        { "o_alpha", cc_features.opt_alpha },
        { "o_fog", cc_features.opt_fog },
        { "o_texture_edge", cc_features.opt_texture_edge },
        { "o_noise", cc_features.opt_noise },
        { "o_2cyc", cc_features.opt_2cyc },
        { "o_alpha_threshold", cc_features.opt_alpha_threshold },
        { "o_invisible", cc_features.opt_invisible },
        { "o_grayscale", cc_features.opt_grayscale },
        { "o_textures", M_ARRAY(cc_features.usedTextures, bool, 2) },
        { "o_masks", M_ARRAY(cc_features.used_masks, bool, 2) },
        { "o_blend", M_ARRAY(cc_features.used_blend, bool, 2) },
        { "o_clamp", M_ARRAY(cc_features.clamp, bool, 2, 2) },
        { "o_inputs", cc_features.numInputs },
        { "o_do_mix", M_ARRAY(cc_features.do_mix, bool, 2, 2) },
        { "o_do_single", M_ARRAY(cc_features.do_single, bool, 2, 2) },
        { "o_do_multiply", M_ARRAY(cc_features.do_multiply, bool, 2, 2) },
        { "o_color_alpha_same", M_ARRAY(cc_features.color_alpha_same, bool, 2) },
        { "FILTER_THREE_POINT", FILTER_THREE_POINT },
        { "FILTER_LINEAR", FILTER_LINEAR },
        { "FILTER_NONE", FILTER_NONE },
        { "srgb_mode", mSrgbMode },
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
        { "o_three_point_filtering", mCurrentFilterMode == FILTER_THREE_POINT },
        { "append_formula", (InvokeFunc)append_formula },
#ifdef __APPLE__
        { "GLSL_VERSION", "#version 410 core" },
        { "attr", "in" },
        { "opengles", false },
        { "core_opengl", true },
        { "texture", "texture" },
        { "vOutColor", "vOutColor" },
#elif defined(USE_OPENGLES)
        { "GLSL_VERSION", "#version 300 es\nprecision mediump float;" },
        { "attr", "in" },
        { "opengles", true },
        { "core_opengl", false },
        { "texture", "texture" },
        { "vOutColor", "vOutColor" },
#else
        { "GLSL_VERSION", "#version 130" },
        { "attr", "varying" },
        { "opengles", false },
        { "core_opengl", false },
        { "texture", "texture2D" },
        { "vOutColor", "gl_FragColor" },
#endif
    };
    processor.populate(mContext);
    auto init = std::make_shared<Ship::ResourceInitData>();
    init->Type = (uint32_t)Ship::ResourceType::Shader;
    init->ByteOrder = Ship::Endianness::Native;
    init->Format = RESOURCE_FORMAT_BINARY;
    auto res = std::static_pointer_cast<Ship::Shader>(Ship::Context::GetInstance()->GetResourceManager()->LoadResource(
        "shaders/opengl/default.shader.fs", true, init));

    if (res == nullptr) {
        SPDLOG_ERROR("Failed to load default fragment shader, missing f3d.o2r?");
        abort();
    }

    auto shader = static_cast<std::string*>(res->GetRawPointer());
    processor.load(*shader);
    processor.bind_include_loader(opengl_include_fs);
    auto result = processor.process();
    // SPDLOG_INFO("=========== FRAGMENT SHADER ============");
    // SPDLOG_INFO(result);
    // SPDLOG_INFO("========================================");
    return result;
}

static size_t numFloats = 0;

static prism::ContextTypes* UpdateFloats(prism::ContextTypes* _, prism::ContextTypes* num) {
    numFloats += std::get<int>(*num);
    return nullptr;
}

static std::string BuildVsShader(const CCFeatures& cc_features) {
    numFloats = 4;
    prism::Processor processor;
    prism::ContextItems mContext = { { "o_textures", M_ARRAY(cc_features.usedTextures, bool, 2) },
                                     { "o_clamp", M_ARRAY(cc_features.clamp, bool, 2, 2) },
                                     { "o_fog", cc_features.opt_fog },
                                     { "o_grayscale", cc_features.opt_grayscale },
                                     { "o_alpha", cc_features.opt_alpha },
                                     { "o_inputs", cc_features.numInputs },
                                     { "update_floats", (InvokeFunc)UpdateFloats },
#ifdef __APPLE__
                                     { "GLSL_VERSION", "#version 410 core" },
                                     { "attr", "in" },
                                     { "out", "out" },
                                     { "opengles", false }
#elif defined(USE_OPENGLES)
                                     { "GLSL_VERSION", "#version 300 es" },
                                     { "attr", "in" },
                                     { "out", "out" },
                                     { "opengles", true }
#else
                                     { "GLSL_VERSION", "#version 110" },
                                     { "attr", "attribute" },
                                     { "out", "varying" },
                                     { "opengles", false }
#endif
    };
    processor.populate(mContext);

    auto init = std::make_shared<Ship::ResourceInitData>();
    init->Type = (uint32_t)Ship::ResourceType::Shader;
    init->ByteOrder = Ship::Endianness::Native;
    init->Format = RESOURCE_FORMAT_BINARY;
    auto res = std::static_pointer_cast<Ship::Shader>(Ship::Context::GetInstance()->GetResourceManager()->LoadResource(
        "shaders/opengl/default.shader.vs", true, init));

    if (res == nullptr) {
        SPDLOG_ERROR("Failed to load default vertex shader, missing f3d.o2r?");
        abort();
    }

    auto shader = static_cast<std::string*>(res->GetRawPointer());
    processor.load(*shader);
    processor.bind_include_loader(opengl_include_fs);
    auto result = processor.process();
    // SPDLOG_INFO("=========== VERTEX SHADER ============");
    // SPDLOG_INFO(result);
    // SPDLOG_INFO("========================================");
    return result;
}

ShaderProgram* GfxRenderingAPIOGL::CreateAndLoadNewShader(uint64_t shader_id0, uint32_t shader_id1) {
    CCFeatures cc_features;
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);
    const auto fs_buf = BuildFsShader(cc_features);
    const auto vs_buf = BuildVsShader(cc_features);
    const GLchar* sources[2] = { vs_buf.data(), fs_buf.data() };
    const GLint lengths[2] = { (GLint)vs_buf.size(), (GLint)fs_buf.size() };
    GLint success;

    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &sources[0], &lengths[0]);
    glCompileShader(vertex_shader);
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 0;
        glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        // fprintf(stderr, "Vertex shader compilation failed\n");
        glGetShaderInfoLog(vertex_shader, max_length, &max_length, &error_log[0]);
        // fprintf(stderr, "%s\n", &error_log[0]);
        abort();
    }

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &sources[1], &lengths[1]);
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 0;
        glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        fprintf(stderr, "Fragment shader compilation failed\n");
        glGetShaderInfoLog(fragment_shader, max_length, &max_length, &error_log[0]);
        fprintf(stderr, "%s\n", &error_log[0]);
        abort();
    }

    GLuint shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);

    size_t cnt = 0;

    struct ShaderProgram* prg = &mShaderProgramPool[std::make_pair(shader_id0, shader_id1)];
    prg->attribLocations[cnt] = glGetAttribLocation(shader_program, "aVtxPos");
    prg->attribSizes[cnt] = 4;
    ++cnt;

    for (int i = 0; i < 2; i++) {
        if (cc_features.usedTextures[i]) {
            char name[32];
            sprintf(name, "aTexCoord%d", i);
            prg->attribLocations[cnt] = glGetAttribLocation(shader_program, name);
            prg->attribSizes[cnt] = 2;
            ++cnt;

            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    sprintf(name, "aTexClamp%s%d", j == 0 ? "S" : "T", i);
                    prg->attribLocations[cnt] = glGetAttribLocation(shader_program, name);
                    prg->attribSizes[cnt] = 1;
                    ++cnt;
                }
            }
        }
    }

    if (cc_features.opt_fog) {
        prg->attribLocations[cnt] = glGetAttribLocation(shader_program, "aFog");
        prg->attribSizes[cnt] = 4;
        ++cnt;
    }

    if (cc_features.opt_grayscale) {
        prg->attribLocations[cnt] = glGetAttribLocation(shader_program, "aGrayscaleColor");
        prg->attribSizes[cnt] = 4;
        ++cnt;
    }

    for (int i = 0; i < cc_features.numInputs; i++) {
        char name[16];
        sprintf(name, "aInput%d", i + 1);
        prg->attribLocations[cnt] = glGetAttribLocation(shader_program, name);
        prg->attribSizes[cnt] = cc_features.opt_alpha ? 4 : 3;
        ++cnt;
    }

    prg->openglProgramId = shader_program;
    prg->numInputs = cc_features.numInputs;
    prg->usedTextures[0] = cc_features.usedTextures[0];
    prg->usedTextures[1] = cc_features.usedTextures[1];
    prg->usedTextures[2] = cc_features.used_masks[0];
    prg->usedTextures[3] = cc_features.used_masks[1];
    prg->usedTextures[4] = cc_features.used_blend[0];
    prg->usedTextures[5] = cc_features.used_blend[1];
    prg->numFloats = numFloats;
    prg->numAttribs = cnt;

    prg->frameCountLocation = glGetUniformLocation(shader_program, "frame_count");
    prg->noiseScaleLocation = glGetUniformLocation(shader_program, "noise_scale");
    prg->texture_width_location = glGetUniformLocation(shader_program, "texture_width");
    prg->texture_height_location = glGetUniformLocation(shader_program, "texture_height");
    prg->texture_filtering_location = glGetUniformLocation(shader_program, "texture_filtering");

    LoadShader(prg);

    if (cc_features.usedTextures[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex0");
        glUniform1i(sampler_location, 0);
    }
    if (cc_features.usedTextures[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex1");
        glUniform1i(sampler_location, 1);
    }
    if (cc_features.used_masks[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTexMask0");
        glUniform1i(sampler_location, 2);
    }
    if (cc_features.used_masks[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTexMask1");
        glUniform1i(sampler_location, 3);
    }
    if (cc_features.used_blend[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTexBlend0");
        glUniform1i(sampler_location, 4);
    }
    if (cc_features.used_blend[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTexBlend1");
        glUniform1i(sampler_location, 5);
    }

    return prg;
}

struct ShaderProgram* GfxRenderingAPIOGL::LookupShader(uint64_t shader_id0, uint32_t shader_id1) {
    auto it = mShaderProgramPool.find(std::make_pair(shader_id0, shader_id1));
    return it == mShaderProgramPool.end() ? nullptr : &it->second;
}

void GfxRenderingAPIOGL::ShaderGetInfo(struct ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    *numInputs = prg->numInputs;
    usedTextures[0] = prg->usedTextures[0];
    usedTextures[1] = prg->usedTextures[1];
}

GLuint GfxRenderingAPIOGL::NewTexture() {
    GLuint ret;
    glGenTextures(1, &ret);
    return ret;
}

void GfxRenderingAPIOGL::DeleteTexture(uint32_t texID) {
    glDeleteTextures(1, &texID);
}

void GfxRenderingAPIOGL::SelectTexture(int tile, GLuint texture_id) {
    glActiveTexture(GL_TEXTURE0 + tile);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    mCurrentTextureIds[tile] = texture_id;
    mCurrentTile = tile;
}

void GfxRenderingAPIOGL::UploadTexture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
    textures[mCurrentTextureIds[mCurrentTile]].width = width;
    textures[mCurrentTextureIds[mCurrentTile]].height = height;
}

#ifdef USE_OPENGLES
#define GL_MIRROR_CLAMP_TO_EDGE 0x8743
#endif

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    switch (val) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            return GL_CLAMP_TO_EDGE;
        case G_TX_MIRROR | G_TX_WRAP:
            return GL_MIRRORED_REPEAT;
        case G_TX_MIRROR | G_TX_CLAMP:
            return GL_MIRROR_CLAMP_TO_EDGE;
        case G_TX_NOMIRROR | G_TX_WRAP:
            return GL_REPEAT;
    }
    return 0;
}

void GfxRenderingAPIOGL::SetSamplerParameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    glActiveTexture(GL_TEXTURE0 + tile);
    const GLint filter = linear_filter && mCurrentFilterMode == FILTER_LINEAR ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    textures[mCurrentTextureIds[tile]].filtering = !linear_filter ? FILTER_LINEAR : FILTER_THREE_POINT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gfx_cm_to_opengl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gfx_cm_to_opengl(cmt));
}

void GfxRenderingAPIOGL::SetDepthTestAndMask(bool depth_test, bool z_upd) {
    mCurrentDepthTest = depth_test;
    mCurrentDepthMask = z_upd;
}

void GfxRenderingAPIOGL::SetZmodeDecal(bool zmode_decal) {
    mCurrentZmodeDecal = zmode_decal;
}

void GfxRenderingAPIOGL::SetViewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
}

void GfxRenderingAPIOGL::SetScissor(int x, int y, int width, int height) {
    glScissor(x, y, width, height);
}

void GfxRenderingAPIOGL::SetUseAlpha(bool use_alpha) {
    if (use_alpha) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}

void GfxRenderingAPIOGL::DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (mCurrentDepthTest != mLastDepthTest || mCurrentDepthMask != mLastDepthMask) {
        mLastDepthTest = mCurrentDepthTest;
        mLastDepthMask = mCurrentDepthMask;

        if (mCurrentDepthTest || mLastDepthMask) {
            glEnable(GL_DEPTH_TEST);
            glDepthMask(mLastDepthMask ? GL_TRUE : GL_FALSE);
            glDepthFunc(mCurrentDepthTest ? (mCurrentZmodeDecal ? GL_LEQUAL : GL_LESS) : GL_ALWAYS);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
    }

    if (mCurrentZmodeDecal != mLastZmodeDecal) {
        mLastZmodeDecal = mCurrentZmodeDecal;
        if (mCurrentZmodeDecal) {
            // SSDB = SlopeScaledDepthBias 120 leads to -2 at 240p which is the same as N64 mode which has very little
            // fighting
            const int n64modeFactor = 120;
            const int noVanishFactor = 100;
            GLfloat SSDB = -2;
            switch (Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(CVAR_Z_FIGHTING_MODE, 0)) {
                // scaled z-fighting (N64 mode like)
                case 1:
                    if (mFrameBuffers.size() >
                        mCurrentFrameBuffer) { // safety check for vector size can probably be removed
                        SSDB = -1.0f * (GLfloat)mFrameBuffers[mCurrentFrameBuffer].height / n64modeFactor;
                    }
                    break;
                // no vanishing paths
                case 2:
                    if (mFrameBuffers.size() >
                        mCurrentFrameBuffer) { // safety check for vector size can probably be removed
                        SSDB = -1.0f * (GLfloat)mFrameBuffers[mCurrentFrameBuffer].height / noVanishFactor;
                    }
                    break;
                // disabled
                case 0:
                default:
                    SSDB = -2;
            }
            glPolygonOffset(SSDB, -2);
            glEnable(GL_POLYGON_OFFSET_FILL);
        } else {
            glPolygonOffset(0, 0);
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    SetPerDrawUniforms();

    // printf("flushing %d tris\n", buf_vbo_num_tris);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_vbo_len, buf_vbo, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);
}

void GfxRenderingAPIOGL::Init() {
#ifndef __linux__
    glewInit();
#endif

    glGenBuffers(1, &mOpenglVbo);
    glBindBuffer(GL_ARRAY_BUFFER, mOpenglVbo);

#if defined(__APPLE__) || defined(USE_OPENGLES)
    glGenVertexArrays(1, &mOpenglVao);
    glBindVertexArray(mOpenglVao);
#endif

#ifndef USE_OPENGLES // not supported on gles
    glEnable(GL_DEPTH_CLAMP);
#endif
    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    mFrameBuffers.resize(1); // for the default screen buffer

    glGenRenderbuffers(1, &mPixelDepthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, mPixelDepthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1, 1);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &mPixelDepthFb);
    glBindFramebuffer(GL_FRAMEBUFFER, mPixelDepthFb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, mPixelDepthRb);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    mPixelDepthRbSize = 1;

    glGetIntegerv(GL_MAX_SAMPLES, &mMaxMsaaLevel);
}

void GfxRenderingAPIOGL::OnResize() {
}

void GfxRenderingAPIOGL::StartFrame() {
    mFrameCount++;
}

bool GfxRenderingAPIOGL::EnsureAmbientOcclusionPrograms() {
    if (mAmbientOcclusion.aoProgram != 0 && mAmbientOcclusion.blurProgram != 0 && mAmbientOcclusion.compositeProgram != 0) {
        return true;
    }

    GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, kFullscreenTriangleVs);
    if (vertexShader == 0) {
        mAmbientOcclusion.fallbackCode = kAoFallbackShaderCompileFailed;
        return false;
    }

    GLuint aoFs = CompileShader(GL_FRAGMENT_SHADER, kAoGenerateFs);
    GLuint blurFs = CompileShader(GL_FRAGMENT_SHADER, kAoBlurFs);
    GLuint compositeFs = CompileShader(GL_FRAGMENT_SHADER, kAoCompositeFs);
    if (aoFs == 0 || blurFs == 0 || compositeFs == 0) {
        if (aoFs != 0) {
            glDeleteShader(aoFs);
        }
        if (blurFs != 0) {
            glDeleteShader(blurFs);
        }
        if (compositeFs != 0) {
            glDeleteShader(compositeFs);
        }
        glDeleteShader(vertexShader);
        mAmbientOcclusion.fallbackCode = kAoFallbackShaderCompileFailed;
        return false;
    }

    mAmbientOcclusion.aoProgram = LinkProgram(vertexShader, aoFs);
    mAmbientOcclusion.blurProgram = LinkProgram(vertexShader, blurFs);
    mAmbientOcclusion.compositeProgram = LinkProgram(vertexShader, compositeFs);

    glDeleteShader(aoFs);
    glDeleteShader(blurFs);
    glDeleteShader(compositeFs);
    glDeleteShader(vertexShader);

    if (mAmbientOcclusion.aoProgram == 0 || mAmbientOcclusion.blurProgram == 0 || mAmbientOcclusion.compositeProgram == 0) {
        if (mAmbientOcclusion.aoProgram != 0) {
            glDeleteProgram(mAmbientOcclusion.aoProgram);
            mAmbientOcclusion.aoProgram = 0;
        }
        if (mAmbientOcclusion.blurProgram != 0) {
            glDeleteProgram(mAmbientOcclusion.blurProgram);
            mAmbientOcclusion.blurProgram = 0;
        }
        if (mAmbientOcclusion.compositeProgram != 0) {
            glDeleteProgram(mAmbientOcclusion.compositeProgram);
            mAmbientOcclusion.compositeProgram = 0;
        }
        mAmbientOcclusion.fallbackCode = kAoFallbackShaderCompileFailed;
        return false;
    }

    return true;
}

bool GfxRenderingAPIOGL::EnsureAmbientOcclusionResources(uint32_t width, uint32_t height) {
    if (!EnsureAmbientOcclusionPrograms()) {
        return false;
    }

    const bool resize = mAmbientOcclusion.width != width || mAmbientOcclusion.height != height;
    if (mAmbientOcclusion.aoTexture == 0) {
        glGenTextures(1, &mAmbientOcclusion.aoTexture);
    }
    if (mAmbientOcclusion.blurTexture == 0) {
        glGenTextures(1, &mAmbientOcclusion.blurTexture);
    }
    if (mAmbientOcclusion.aoFbo == 0) {
        glGenFramebuffers(1, &mAmbientOcclusion.aoFbo);
    }
    if (mAmbientOcclusion.blurFbo == 0) {
        glGenFramebuffers(1, &mAmbientOcclusion.blurFbo);
    }

    if (resize || mAmbientOcclusion.width == 0 || mAmbientOcclusion.height == 0) {
        const std::array<GLuint, 2> aoTextures = { mAmbientOcclusion.aoTexture, mAmbientOcclusion.blurTexture };
        for (const auto tex : aoTextures) {
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, mAmbientOcclusion.aoFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mAmbientOcclusion.aoTexture, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, mAmbientOcclusion.blurFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mAmbientOcclusion.blurTexture, 0);
    }

    mAmbientOcclusion.width = width;
    mAmbientOcclusion.height = height;
    return true;
}

void GfxRenderingAPIOGL::CleanupAmbientOcclusionResources() {
    if (mAmbientOcclusion.aoProgram != 0) {
        glDeleteProgram(mAmbientOcclusion.aoProgram);
        mAmbientOcclusion.aoProgram = 0;
    }
    if (mAmbientOcclusion.blurProgram != 0) {
        glDeleteProgram(mAmbientOcclusion.blurProgram);
        mAmbientOcclusion.blurProgram = 0;
    }
    if (mAmbientOcclusion.compositeProgram != 0) {
        glDeleteProgram(mAmbientOcclusion.compositeProgram);
        mAmbientOcclusion.compositeProgram = 0;
    }
    if (mAmbientOcclusion.aoFbo != 0) {
        glDeleteFramebuffers(1, &mAmbientOcclusion.aoFbo);
        mAmbientOcclusion.aoFbo = 0;
    }
    if (mAmbientOcclusion.blurFbo != 0) {
        glDeleteFramebuffers(1, &mAmbientOcclusion.blurFbo);
        mAmbientOcclusion.blurFbo = 0;
    }
    if (mAmbientOcclusion.aoTexture != 0) {
        glDeleteTextures(1, &mAmbientOcclusion.aoTexture);
        mAmbientOcclusion.aoTexture = 0;
    }
    if (mAmbientOcclusion.blurTexture != 0) {
        glDeleteTextures(1, &mAmbientOcclusion.blurTexture);
        mAmbientOcclusion.blurTexture = 0;
    }
    mAmbientOcclusion.width = 0;
    mAmbientOcclusion.height = 0;
}

void GfxRenderingAPIOGL::ApplyAmbientOcclusion() {
    if (!mAmbientOcclusionConfig.enabled || mAmbientOcclusionConfig.quality == 0) {
        return;
    }
    mAmbientOcclusion.fallbackCode = kAoFallbackNone;

    if (mCurrentFrameBuffer >= mFrameBuffers.size()) {
        mAmbientOcclusion.fallbackCode = kAoFallbackRuntimeUnsupported;
        return;
    }

    auto& target = mFrameBuffers[mCurrentFrameBuffer];
    if (!target.has_depth_buffer || !target.can_extract_depth || target.depthTex == 0 || target.width == 0 || target.height == 0) {
        mAmbientOcclusion.fallbackCode = kAoFallbackRuntimeUnsupported;
        return;
    }

    if (!EnsureAmbientOcclusionResources(target.width, target.height)) {
        return;
    }

    std::array<float, 3> qualityRadiusMul = { 0.0f, 1.0f, 1.6f };
    std::array<int32_t, 3> qualitySamples = { 8, 12, 16 };
    const uint8_t qualityIndex = std::clamp<uint8_t>(mAmbientOcclusionConfig.quality, 1, 3) - 1;
    const float radiusPixels = std::clamp(mAmbientOcclusionConfig.radius * qualityRadiusMul[qualityIndex] * 12.0f, 0.5f, 64.0f);
    const int32_t sampleCount = qualitySamples[qualityIndex];

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, static_cast<GLsizei>(target.width), static_cast<GLsizei>(target.height));

    glBindFramebuffer(GL_FRAMEBUFFER, mAmbientOcclusion.aoFbo);
    glUseProgram(mAmbientOcclusion.aoProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, target.depthTex);
    glUniform1i(glGetUniformLocation(mAmbientOcclusion.aoProgram, "uDepthTex"), 0);
    glUniform2f(glGetUniformLocation(mAmbientOcclusion.aoProgram, "uInvResolution"), 1.0f / target.width, 1.0f / target.height);
    glUniform1f(glGetUniformLocation(mAmbientOcclusion.aoProgram, "uRadiusPixels"), radiusPixels);
    glUniform1f(glGetUniformLocation(mAmbientOcclusion.aoProgram, "uIntensity"), mAmbientOcclusionConfig.intensity);
    glUniform1f(glGetUniformLocation(mAmbientOcclusion.aoProgram, "uBias"), mAmbientOcclusionConfig.bias);
    glUniform1f(glGetUniformLocation(mAmbientOcclusion.aoProgram, "uPower"), mAmbientOcclusionConfig.power);
    glUniform1f(glGetUniformLocation(mAmbientOcclusion.aoProgram, "uMaxDistance"), mAmbientOcclusionConfig.maxDistance);
    glUniform1i(glGetUniformLocation(mAmbientOcclusion.aoProgram, "uSampleCount"), sampleCount);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    const int32_t blurPasses = std::clamp(mAmbientOcclusionConfig.blurPasses, 0, 4);
    for (int32_t pass = 0; pass < blurPasses; ++pass) {
        glUseProgram(mAmbientOcclusion.blurProgram);
        glUniform2f(glGetUniformLocation(mAmbientOcclusion.blurProgram, "uInvResolution"), 1.0f / target.width, 1.0f / target.height);
        glUniform1i(glGetUniformLocation(mAmbientOcclusion.blurProgram, "uAoTex"), 0);

        glBindFramebuffer(GL_FRAMEBUFFER, mAmbientOcclusion.blurFbo);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mAmbientOcclusion.aoTexture);
        glUniform2f(glGetUniformLocation(mAmbientOcclusion.blurProgram, "uDirection"), 1.0f, 0.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glBindFramebuffer(GL_FRAMEBUFFER, mAmbientOcclusion.aoFbo);
        glBindTexture(GL_TEXTURE_2D, mAmbientOcclusion.blurTexture);
        glUniform2f(glGetUniformLocation(mAmbientOcclusion.blurProgram, "uDirection"), 0.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    glUseProgram(mAmbientOcclusion.compositeProgram);
    glUniform2f(glGetUniformLocation(mAmbientOcclusion.compositeProgram, "uInvResolution"), 1.0f / target.width,
                1.0f / target.height);
    glUniform1i(glGetUniformLocation(mAmbientOcclusion.compositeProgram, "uAoTex"), 0);
    glUniform1i(glGetUniformLocation(mAmbientOcclusion.compositeProgram, "uDebugView"), mAmbientOcclusionConfig.debugView ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mAmbientOcclusion.aoTexture);
    if (mAmbientOcclusionConfig.debugView) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        glBlendFunc(GL_DST_COLOR, GL_ZERO);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glDepthMask(mCurrentDepthMask ? GL_TRUE : GL_FALSE);
    if (mCurrentDepthTest) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
}

bool GfxRenderingAPIOGL::IsAmbientOcclusionSupported() const {
    return true;
}

int32_t GfxRenderingAPIOGL::GetAmbientOcclusionFallbackCode() const {
    return mAmbientOcclusion.fallbackCode;
}

void GfxRenderingAPIOGL::ClearAmbientOcclusionFallbackCode() {
    mAmbientOcclusion.fallbackCode = kAoFallbackNone;
}

bool GfxRenderingAPIOGL::EnsureVolumetricsPrograms() {
    if (mVolumetrics.raymarchProgram != 0 && mVolumetrics.compositeProgram != 0) {
        return true;
    }

    GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, kFullscreenTriangleVs);
    GLuint raymarchShader = CompileShader(GL_FRAGMENT_SHADER, kVolumetricsRaymarchFs);
    GLuint compositeShader = CompileShader(GL_FRAGMENT_SHADER, kVolumetricsCompositeFs);
    if (vertexShader == 0 || raymarchShader == 0 || compositeShader == 0) {
        mVolumetrics.fallbackCode = kVolumetricsFallbackShaderCompileFailed;
        if (vertexShader != 0) {
            glDeleteShader(vertexShader);
        }
        if (raymarchShader != 0) {
            glDeleteShader(raymarchShader);
        }
        if (compositeShader != 0) {
            glDeleteShader(compositeShader);
        }
        return false;
    }

    mVolumetrics.raymarchProgram = LinkProgram(vertexShader, raymarchShader);
    mVolumetrics.compositeProgram = LinkProgram(vertexShader, compositeShader);
    glDeleteShader(vertexShader);
    glDeleteShader(raymarchShader);
    glDeleteShader(compositeShader);

    if (mVolumetrics.raymarchProgram == 0 || mVolumetrics.compositeProgram == 0) {
        mVolumetrics.fallbackCode = kVolumetricsFallbackShaderCompileFailed;
        CleanupVolumetricsResources();
        return false;
    }

    return true;
}

bool GfxRenderingAPIOGL::EnsureVolumetricsResources(uint32_t fullWidth, uint32_t fullHeight) {
    if (!EnsureVolumetricsPrograms()) {
        return false;
    }

    const float resolutionScale = mVolumetricsConfig.quality >= 3 ? 0.5f : 0.25f;
    const uint32_t lowWidth = std::max<uint32_t>(1, static_cast<uint32_t>(std::round(fullWidth * resolutionScale)));
    const uint32_t lowHeight = std::max<uint32_t>(1, static_cast<uint32_t>(std::round(fullHeight * resolutionScale)));

    if (mVolumetrics.fullWidth == fullWidth && mVolumetrics.fullHeight == fullHeight && mVolumetrics.lowWidth == lowWidth &&
        mVolumetrics.lowHeight == lowHeight && mVolumetrics.volumeTextures[0] != 0 && mVolumetrics.volumeTextures[1] != 0 &&
        mVolumetrics.depthTextures[0] != 0 && mVolumetrics.depthTextures[1] != 0) {
        return true;
    }

    if (mVolumetrics.fbos[0] == 0) {
        glGenFramebuffers(2, mVolumetrics.fbos);
    }
    glGenTextures(2, mVolumetrics.volumeTextures);
    glGenTextures(2, mVolumetrics.depthTextures);

    for (size_t i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, mVolumetrics.volumeTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, static_cast<GLsizei>(lowWidth), static_cast<GLsizei>(lowHeight), 0, GL_RGBA,
                     GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, mVolumetrics.depthTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, static_cast<GLsizei>(lowWidth), static_cast<GLsizei>(lowHeight), 0, GL_RED,
                     GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_FRAMEBUFFER, mVolumetrics.fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mVolumetrics.volumeTextures[i], 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, mVolumetrics.depthTextures[i], 0);
        const GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, drawBuffers);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            mVolumetrics.fallbackCode = kVolumetricsFallbackRuntimeUnsupported;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return false;
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    mVolumetrics.fullWidth = fullWidth;
    mVolumetrics.fullHeight = fullHeight;
    mVolumetrics.lowWidth = lowWidth;
    mVolumetrics.lowHeight = lowHeight;
    mVolumetrics.historyIndex = 0;
    mVolumetrics.historyValid = false;
    return true;
}

void GfxRenderingAPIOGL::CleanupVolumetricsResources() {
    if (mVolumetrics.raymarchProgram != 0) {
        glDeleteProgram(mVolumetrics.raymarchProgram);
        mVolumetrics.raymarchProgram = 0;
    }
    if (mVolumetrics.compositeProgram != 0) {
        glDeleteProgram(mVolumetrics.compositeProgram);
        mVolumetrics.compositeProgram = 0;
    }
    glDeleteFramebuffers(2, mVolumetrics.fbos);
    glDeleteTextures(2, mVolumetrics.volumeTextures);
    glDeleteTextures(2, mVolumetrics.depthTextures);
    mVolumetrics.fbos[0] = mVolumetrics.fbos[1] = 0;
    mVolumetrics.volumeTextures[0] = mVolumetrics.volumeTextures[1] = 0;
    mVolumetrics.depthTextures[0] = mVolumetrics.depthTextures[1] = 0;
    mVolumetrics.fullWidth = 0;
    mVolumetrics.fullHeight = 0;
    mVolumetrics.lowWidth = 0;
    mVolumetrics.lowHeight = 0;
    mVolumetrics.historyValid = false;
}

void GfxRenderingAPIOGL::ApplyVolumetrics() {
    mVolumetricsRuntimeStats = {};
    if (!mVolumetricsConfig.enabled || mVolumetricsConfig.quality == 0) {
        return;
    }
    mVolumetrics.fallbackCode = kVolumetricsFallbackNone;

    if (mCurrentFrameBuffer >= mFrameBuffers.size()) {
        mVolumetrics.fallbackCode = kVolumetricsFallbackRuntimeUnsupported;
        return;
    }

    auto& target = mFrameBuffers[mCurrentFrameBuffer];
    if (!target.has_depth_buffer || !target.can_extract_depth || target.depthTex == 0 || target.width == 0 || target.height == 0) {
        mVolumetrics.fallbackCode = kVolumetricsFallbackRuntimeUnsupported;
        return;
    }

    if (!EnsureVolumetricsResources(target.width, target.height)) {
        return;
    }

    if (mVolumetricsConfig.resetHistory) {
        mVolumetrics.historyValid = false;
    }

    const uint8_t writeIndex = mVolumetrics.historyIndex;
    const uint8_t readIndex = 1 - writeIndex;
    const int32_t stepCount = mVolumetricsConfig.quality == 1 ? 24 : (mVolumetricsConfig.quality == 2 ? 32 : 48);

    std::array<GLfloat, Fast::GFX_MAX_VOLUMETRIC_LIGHTS * 4> lightPositionRadius{};
    std::array<GLfloat, Fast::GFX_MAX_VOLUMETRIC_LIGHTS * 4> lightDirectionIntensity{};
    std::array<GLfloat, Fast::GFX_MAX_VOLUMETRIC_LIGHTS * 4> lightColorVolumetric{};
    std::array<GLfloat, Fast::GFX_MAX_VOLUMETRIC_LIGHTS * 4> lightConeShadow{};
    std::array<GLfloat, Fast::GFX_MAX_VOLUMETRIC_LIGHTS * 4> lightShadowParams{};
    for (size_t i = 0; i < mVolumetricLightCount && i < Fast::GFX_MAX_VOLUMETRIC_LIGHTS; ++i) {
        const auto& light = mVolumetricLights[i];
        lightPositionRadius[i * 4 + 0] = light.position[0];
        lightPositionRadius[i * 4 + 1] = light.position[1];
        lightPositionRadius[i * 4 + 2] = light.position[2];
        lightPositionRadius[i * 4 + 3] = light.radius;
        lightDirectionIntensity[i * 4 + 0] = light.direction[0];
        lightDirectionIntensity[i * 4 + 1] = light.direction[1];
        lightDirectionIntensity[i * 4 + 2] = light.direction[2];
        lightDirectionIntensity[i * 4 + 3] = light.intensity;
        lightColorVolumetric[i * 4 + 0] = light.color[0];
        lightColorVolumetric[i * 4 + 1] = light.color[1];
        lightColorVolumetric[i * 4 + 2] = light.color[2];
        lightColorVolumetric[i * 4 + 3] = light.volumetricIntensity;
        lightConeShadow[i * 4 + 0] = light.innerConeCos;
        lightConeShadow[i * 4 + 1] = light.outerConeCos;
        lightConeShadow[i * 4 + 2] = light.shadowBias;
        lightConeShadow[i * 4 + 3] = light.shadowRange;
        lightShadowParams[i * 4 + 0] = light.shadowNormalBias;
        lightShadowParams[i * 4 + 1] = light.shadowResolution;
        lightShadowParams[i * 4 + 2] = static_cast<float>(light.type);
        lightShadowParams[i * 4 + 3] = static_cast<float>(light.castShadows);
    }

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, static_cast<GLsizei>(mVolumetrics.lowWidth), static_cast<GLsizei>(mVolumetrics.lowHeight));

    glBindFramebuffer(GL_FRAMEBUFFER, mVolumetrics.fbos[writeIndex]);
    const GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, drawBuffers);
    const GLfloat clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const GLfloat clearDepthValue[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    glClearBufferfv(GL_COLOR, 0, clearColor);
    glClearBufferfv(GL_COLOR, 1, clearDepthValue);
    glUseProgram(mVolumetrics.raymarchProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, target.depthTex);
    glUniform1i(glGetUniformLocation(mVolumetrics.raymarchProgram, "uDepthTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mVolumetrics.volumeTextures[readIndex]);
    glUniform1i(glGetUniformLocation(mVolumetrics.raymarchProgram, "uPrevVolumeTex"), 1);
    glUniform2f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uInvFullResolution"), 1.0f / target.width,
                1.0f / target.height);
    glUniform2f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uInvLowResolution"), 1.0f / mVolumetrics.lowWidth,
                1.0f / mVolumetrics.lowHeight);
    glUniform3fv(glGetUniformLocation(mVolumetrics.raymarchProgram, "uCameraWorldPosition"), 1,
                 mVolumetricsConfig.cameraWorldPosition);
    glUniform1f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uDensity"), mVolumetricsConfig.density);
    glUniform3f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uFogColor"), mVolumetricsConfig.color[0],
                mVolumetricsConfig.color[1], mVolumetricsConfig.color[2]);
    glUniform1f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uAmbientIntensity"),
                mVolumetricsConfig.ambientIntensity);
    glUniform1f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uAnisotropy"), mVolumetricsConfig.anisotropy);
    glUniform1f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uStartDistance"), mVolumetricsConfig.startDistance);
    glUniform1f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uMaxDistance"), mVolumetricsConfig.maxDistance);
    glUniform1f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uBaseHeight"), mVolumetricsConfig.baseHeight);
    glUniform1f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uHeightFalloff"), mVolumetricsConfig.heightFalloff);
    glUniform1f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uLightShaftIntensity"),
                mVolumetricsConfig.lightShaftIntensity);
    glUniform1f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uShadowIntensity"), mVolumetricsConfig.shadowIntensity);
    glUniform1f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uTemporalBlend"), mVolumetricsConfig.temporalBlend);
    glUniform1f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uJitterScale"), mVolumetricsConfig.jitterScale);
    glUniform1i(glGetUniformLocation(mVolumetrics.raymarchProgram, "uStepCount"), stepCount);
    glUniform1i(glGetUniformLocation(mVolumetrics.raymarchProgram, "uLightCount"), static_cast<GLint>(mVolumetricLightCount));
    glUniform1i(glGetUniformLocation(mVolumetrics.raymarchProgram, "uShadowQuality"), mVolumetricsConfig.shadowQuality);
    glUniform1i(glGetUniformLocation(mVolumetrics.raymarchProgram, "uHistoryValid"),
                (mVolumetrics.historyValid && !mVolumetricsConfig.resetHistory) ? 1 : 0);
    glUniform1i(glGetUniformLocation(mVolumetrics.raymarchProgram, "uHeightFogEnabled"),
                mVolumetricsConfig.heightFogEnabled ? 1 : 0);
    glUniform1i(glGetUniformLocation(mVolumetrics.raymarchProgram, "uDebugView"), mVolumetricsConfig.debugView ? 1 : 0);
    glUniform1i(glGetUniformLocation(mVolumetrics.raymarchProgram, "uDebugMode"), mVolumetricsConfig.debugMode);
    glUniform2f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uDepthNdcParams"), 2.0f, -1.0f);
    glUniform2f(glGetUniformLocation(mVolumetrics.raymarchProgram, "uNdcDepthParams"), 0.5f, 0.5f);
    glUniformMatrix4fv(glGetUniformLocation(mVolumetrics.raymarchProgram, "uCurrViewProj"), 1, GL_FALSE,
                       mVolumetricsConfig.currentViewProjection);
    glUniformMatrix4fv(glGetUniformLocation(mVolumetrics.raymarchProgram, "uInvViewProj"), 1, GL_FALSE,
                       mVolumetricsConfig.inverseViewProjection);
    glUniformMatrix4fv(glGetUniformLocation(mVolumetrics.raymarchProgram, "uPrevViewProj"), 1, GL_FALSE,
                       mVolumetricsConfig.previousViewProjection);
    glUniform4fv(glGetUniformLocation(mVolumetrics.raymarchProgram, "uLightPositionRadius"),
                 static_cast<GLsizei>(Fast::GFX_MAX_VOLUMETRIC_LIGHTS), lightPositionRadius.data());
    glUniform4fv(glGetUniformLocation(mVolumetrics.raymarchProgram, "uLightDirectionIntensity"),
                 static_cast<GLsizei>(Fast::GFX_MAX_VOLUMETRIC_LIGHTS), lightDirectionIntensity.data());
    glUniform4fv(glGetUniformLocation(mVolumetrics.raymarchProgram, "uLightColorVolumetric"),
                 static_cast<GLsizei>(Fast::GFX_MAX_VOLUMETRIC_LIGHTS), lightColorVolumetric.data());
    glUniform4fv(glGetUniformLocation(mVolumetrics.raymarchProgram, "uLightConeShadow"),
                 static_cast<GLsizei>(Fast::GFX_MAX_VOLUMETRIC_LIGHTS), lightConeShadow.data());
    glUniform4fv(glGetUniformLocation(mVolumetrics.raymarchProgram, "uLightShadowParams"),
                 static_cast<GLsizei>(Fast::GFX_MAX_VOLUMETRIC_LIGHTS), lightShadowParams.data());
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    glViewport(0, 0, static_cast<GLsizei>(target.width), static_cast<GLsizei>(target.height));
    glUseProgram(mVolumetrics.compositeProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mVolumetrics.volumeTextures[writeIndex]);
    glUniform1i(glGetUniformLocation(mVolumetrics.compositeProgram, "uVolumeTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mVolumetrics.depthTextures[writeIndex]);
    glUniform1i(glGetUniformLocation(mVolumetrics.compositeProgram, "uLowDepthTex"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, target.depthTex);
    glUniform1i(glGetUniformLocation(mVolumetrics.compositeProgram, "uFullDepthTex"), 2);
    glUniform2f(glGetUniformLocation(mVolumetrics.compositeProgram, "uInvFullResolution"), 1.0f / target.width,
                1.0f / target.height);
    glUniform2f(glGetUniformLocation(mVolumetrics.compositeProgram, "uInvLowResolution"), 1.0f / mVolumetrics.lowWidth,
                1.0f / mVolumetrics.lowHeight);
    glUniform1i(glGetUniformLocation(mVolumetrics.compositeProgram, "uDebugView"), mVolumetricsConfig.debugView ? 1 : 0);
    glUniform1i(glGetUniformLocation(mVolumetrics.compositeProgram, "uDebugMode"), mVolumetricsConfig.debugMode);
    if (mVolumetricsConfig.debugMode != GFX_VOLUMETRICS_DEBUG_OFF || mVolumetricsConfig.debugView) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glDepthMask(mCurrentDepthMask ? GL_TRUE : GL_FALSE);
    if (mCurrentDepthTest) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);

    mVolumetrics.historyValid = true;
    mVolumetrics.historyIndex = readIndex;
    mVolumetricsRuntimeStats.executed = true;
    mVolumetricsRuntimeStats.historyValid = mVolumetrics.historyValid;
    mVolumetricsRuntimeStats.lowWidth = static_cast<uint32_t>(mVolumetrics.lowWidth);
    mVolumetricsRuntimeStats.lowHeight = static_cast<uint32_t>(mVolumetrics.lowHeight);
    mVolumetricsRuntimeStats.stepCount = static_cast<uint32_t>(stepCount);
}

bool GfxRenderingAPIOGL::IsVolumetricsSupported() const {
    return true;
}

int32_t GfxRenderingAPIOGL::GetVolumetricsFallbackCode() const {
    return mVolumetrics.fallbackCode;
}

void GfxRenderingAPIOGL::ClearVolumetricsFallbackCode() {
    mVolumetrics.fallbackCode = kVolumetricsFallbackNone;
}

void GfxRenderingAPIOGL::EndFrame() {
    glFlush();
}

void GfxRenderingAPIOGL::FinishRender() {
}

int GfxRenderingAPIOGL::CreateFramebuffer() {
    GLuint clrbuf;
    glGenTextures(1, &clrbuf);
    glBindTexture(GL_TEXTURE_2D, clrbuf);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    GLuint clrbufMsaa;
    glGenRenderbuffers(1, &clrbufMsaa);

    GLuint rbo;
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1, 1);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    GLuint fbo;
    glGenFramebuffers(1, &fbo);

    size_t i = mFrameBuffers.size();
    mFrameBuffers.resize(i + 1);

    mFrameBuffers[i].fbo = fbo;
    mFrameBuffers[i].clrbuf = clrbuf;
    mFrameBuffers[i].clrbufMsaa = clrbufMsaa;
    mFrameBuffers[i].rbo = rbo;
    mFrameBuffers[i].depthTex = 0;
    mFrameBuffers[i].can_extract_depth = false;

    return i;
}

void GfxRenderingAPIOGL::UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                     bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                                     bool can_extract_depth) {
    FramebufferOGL& fb = mFrameBuffers[fb_id];

    width = std::max(width, 1U);
    height = std::max(height, 1U);
    msaa_level = std::min(msaa_level, (uint32_t)mMaxMsaaLevel);

    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);

    if (fb_id != 0) {
        if (fb.width != width || fb.height != height || fb.msaa_level != msaa_level) {
            if (msaa_level <= 1) {
                glBindTexture(GL_TEXTURE_2D, fb.clrbuf);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
                glBindTexture(GL_TEXTURE_2D, 0);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb.clrbuf, 0);
            } else {
                glBindRenderbuffer(GL_RENDERBUFFER, fb.clrbufMsaa);
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_level, GL_RGB8, width, height);
                glBindRenderbuffer(GL_RENDERBUFFER, 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fb.clrbufMsaa);
            }
        }

        const bool useExtractableDepth = has_depth_buffer && can_extract_depth && msaa_level <= 1;
        if (useExtractableDepth) {
            if (fb.depthTex == 0) {
                glGenTextures(1, &fb.depthTex);
            }
            if (fb.width != width || fb.height != height || fb.msaa_level != msaa_level || !fb.has_depth_buffer ||
                !fb.can_extract_depth) {
                glBindTexture(GL_TEXTURE_2D, fb.depthTex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0, GL_DEPTH_STENCIL,
                             GL_UNSIGNED_INT_24_8, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, fb.depthTex, 0);
        } else {
            if (has_depth_buffer &&
                (fb.width != width || fb.height != height || fb.msaa_level != msaa_level || !fb.has_depth_buffer ||
                 fb.can_extract_depth)) {
                glBindRenderbuffer(GL_RENDERBUFFER, fb.rbo);
                if (msaa_level <= 1) {
                    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
                } else {
                    glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_level, GL_DEPTH24_STENCIL8, width, height);
                }
                glBindRenderbuffer(GL_RENDERBUFFER, 0);
            }

            if (has_depth_buffer) {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fb.rbo);
            } else {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
            }
        }
    }

    fb.width = width;
    fb.height = height;
    fb.has_depth_buffer = has_depth_buffer;
    fb.msaa_level = msaa_level;
    fb.can_extract_depth = has_depth_buffer && can_extract_depth && msaa_level <= 1;
    fb.invertY = opengl_invertY;
}

void GfxRenderingAPIOGL::StartDrawToFramebuffer(int fb_id, float noise_scale) {
    FramebufferOGL& fb = mFrameBuffers[fb_id];

    if (noise_scale != 0.0f) {
        mCurrentNoiseScale = 1.0f / noise_scale;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
    mCurrentFrameBuffer = fb_id;
}

void GfxRenderingAPIOGL::ClearFramebuffer(bool color, bool depth) {
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear((color ? GL_COLOR_BUFFER_BIT : 0) | (depth ? GL_DEPTH_BUFFER_BIT : 0));
    glDepthMask(mCurrentDepthMask ? GL_TRUE : GL_FALSE);
    glEnable(GL_SCISSOR_TEST);
}

void GfxRenderingAPIOGL::ResolveMSAAColorBuffer(int fb_id_target, int fb_id_source) {
    FramebufferOGL& fb_dst = mFrameBuffers[fb_id_target];
    FramebufferOGL& fb_src = mFrameBuffers[fb_id_source];
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb_dst.fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb_src.fbo);

    // Disabled for blit
    glDisable(GL_SCISSOR_TEST);

    glBlitFramebuffer(0, 0, fb_src.width, fb_src.height, 0, 0, fb_dst.width, fb_dst.height, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[mCurrentFrameBuffer].fbo);

    glEnable(GL_SCISSOR_TEST);
}

void* GfxRenderingAPIOGL::GetFramebufferTextureId(int fb_id) {
    return (void*)(uintptr_t)mFrameBuffers[fb_id].clrbuf;
}

void GfxRenderingAPIOGL::SelectTextureFb(int fb_id) {
    // glDisable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, mFrameBuffers[fb_id].clrbuf);
}

void GfxRenderingAPIOGL::CopyFramebuffer(int fb_dst_id, int fb_src_id, int srcX0, int srcY0, int srcX1, int srcY1,
                                         int dstX0, int dstY0, int dstX1, int dstY1) {
    if (fb_dst_id >= (int)mFrameBuffers.size() || fb_src_id >= (int)mFrameBuffers.size()) {
        return;
    }

    FramebufferOGL src = mFrameBuffers[fb_src_id];
    const FramebufferOGL& dst = mFrameBuffers[fb_dst_id];

    // Adjust y values for non-inverted source frame buffers because opengl uses bottom left for origin
    if (!src.invertY) {
        int temp = srcY1 - srcY0;
        srcY1 = src.height - srcY0;
        srcY0 = srcY1 - temp;
    }

    // Flip the y values
    if (src.invertY != dst.invertY) {
        std::swap(srcY0, srcY1);
    }

    // Disabled for blit
    glDisable(GL_SCISSOR_TEST);

    // For msaa enabled buffers we can't perform a scaled blit to a simple sample buffer
    // First do an unscaled blit to a msaa resolved buffer
    if (src.height != dst.height && src.width != dst.width && src.msaa_level > 1) {
        // Start with the main buffer (0) as the msaa resolved buffer
        int fb_resolve_id = 0;
        FramebufferOGL fb_resolve = mFrameBuffers[fb_resolve_id];

        // If the size doesn't match our source, then we need to use our separate color msaa resolved buffer (2)
        if (fb_resolve.height != src.height || fb_resolve.width != src.width) {
            fb_resolve_id = 2;
            fb_resolve = mFrameBuffers[fb_resolve_id];
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, src.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb_resolve.fbo);

        glBlitFramebuffer(0, 0, src.width, src.height, 0, 0, src.width, src.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // Switch source buffer to the resolved sample
        fb_src_id = fb_resolve_id;
        src = fb_resolve;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst.fbo);

    // The 0 buffer is a double buffer so we need to choose the back to avoid imgui elements
    if (fb_src_id == 0) {
        glReadBuffer(GL_BACK);
    } else {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }

    glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[mCurrentFrameBuffer].fbo);

    glReadBuffer(GL_BACK);

    glEnable(GL_SCISSOR_TEST);
}

void GfxRenderingAPIOGL::ReadFramebufferToCPU(int fb_id, uint32_t width, uint32_t height, uint16_t* rgba16_buf) {
    if (fb_id >= (int)mFrameBuffers.size()) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[fb_id].fbo);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, (void*)rgba16_buf);
    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[mCurrentFrameBuffer].fbo);
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIOGL::GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res;

    FramebufferOGL& fb = mFrameBuffers[fb_id];

    // When looking up one value and the framebuffer is single-sampled, we can read pixels directly
    // Otherwise we need to blit first to a new buffer then read it
    if (coordinates.size() == 1 && fb.msaa_level <= 1) {
        uint32_t depth_stencil_value;
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
        int x = coordinates.begin()->first;
        int y = coordinates.begin()->second;
#ifndef USE_OPENGLES // not supported on gles. Runs fine without it, but this may cause issues
        glReadPixels(x, fb.invertY ? fb.height - y : y, 1, 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8,
                     &depth_stencil_value);
#endif
        res.emplace(*coordinates.begin(), (depth_stencil_value >> 18) << 2);
    } else {
        if (mPixelDepthRbSize < coordinates.size()) {
            // Resizing a renderbuffer seems broken with Intel's driver, so recreate one instead.
            glBindFramebuffer(GL_FRAMEBUFFER, mPixelDepthFb);
            glDeleteRenderbuffers(1, &mPixelDepthRb);
            glGenRenderbuffers(1, &mPixelDepthRb);
            glBindRenderbuffer(GL_RENDERBUFFER, mPixelDepthRb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, coordinates.size(), 1);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, mPixelDepthRb);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);

            mPixelDepthRbSize = coordinates.size();
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mPixelDepthFb);

        glDisable(GL_SCISSOR_TEST); // needed for the blit operation

        {
            size_t i = 0;
            for (const auto& coord : coordinates) {
                int x = coord.first;
                int y = coord.second;
                if (fb.invertY) {
                    y = fb.height - y;
                }
                glBlitFramebuffer(x, y, x + 1, y + 1, i, 0, i + 1, 1, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                                  GL_NEAREST);
                ++i;
            }
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, mPixelDepthFb);
        std::vector<uint32_t> depth_stencil_values(coordinates.size());
#ifndef USE_OPENGLES // not supported on gles. Runs fine without it, but this may cause issues
        glReadPixels(0, 0, coordinates.size(), 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, depth_stencil_values.data());
#endif
        {
            size_t i = 0;
            for (const auto& coord : coordinates) {
                res.emplace(coord, (depth_stencil_values[i++] >> 18) << 2);
            }
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, mCurrentFrameBuffer);

    return res;
}

void GfxRenderingAPIOGL::SetTextureFilter(FilteringMode mode) {
    gfx_texture_cache_clear();
    mCurrentFilterMode = mode;
}

FilteringMode GfxRenderingAPIOGL::GetTextureFilter() {
    return mCurrentFilterMode;
}

void GfxRenderingAPIOGL::SetSrgbMode() {
    mSrgbMode = true;
}

ImTextureID GfxRenderingAPIOGL::GetTextureById(int id) {
    return reinterpret_cast<ImTextureID>(id);
}
} // namespace Fast
#endif

#pragma clang diagnostic pop
