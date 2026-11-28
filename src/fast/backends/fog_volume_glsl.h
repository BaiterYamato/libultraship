#pragma once

namespace Fast {
inline constexpr char FogVertexGLSL[] = R"glsl(
void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

inline constexpr char FogFragmentGLSL[] = R"glsl(
#ifdef FOG_MSAA
uniform sampler2DMS worldDepth;
#else
uniform sampler2D worldDepth;
#endif
uniform sampler2D fogField;
uniform vec4 parameters[9];
uniform vec4 viewport;
uniform float flipY;
out vec4 fog;

#define cameraEye      parameters[0]
#define cameraForward  parameters[1]
#define cameraRight    parameters[2]
#define cameraUp       parameters[3]
#define projection     parameters[4]
#define boundsMin      parameters[5]
#define boundsMax      parameters[6]
#define fogColor       parameters[7]
#define grid           parameters[8]

float nonZero(float v) {
    return abs(v) > 0.00001 ? v : (v < 0.0 ? -0.00001 : 0.00001);
}

void main() {
    vec2 position = gl_FragCoord.xy;
#ifdef FOG_MSAA
    float raw = texelFetch(worldDepth, ivec2(position), gl_SampleID).r;
    position = floor(position) + gl_SamplePosition;
#else
    float raw = texelFetch(worldDepth, ivec2(position), 0).r;
#endif
    float ndcZ = 2.0 * raw - 1.0;
    float sceneZ = raw >= 0.999999 || cameraEye.w == 3.0 ? projection.w :
        clamp(projection.z / nonZero(projection.x - ndcZ * projection.y), 0.0, projection.w);
    if (cameraEye.w == 2.0) {
        fog = vec4(vec3(log2(1.0 + sceneZ) / log2(1.0 + projection.w)), 1.0);
        return;
    }

    vec2 ndc = 2.0 * (position - viewport.xy) / viewport.zw - 1.0;
    vec3 ray = cameraForward.xyz + cameraRight.xyz * (cameraRight.w * ndc.x) +
               cameraUp.xyz * (cameraUp.w * ndc.y * flipY);
    vec3 inverse = 1.0 / vec3(nonZero(ray.x), nonZero(ray.y), nonZero(ray.z));
    vec3 t0 = (boundsMin.xyz - cameraEye.xyz) * inverse;
    vec3 t1 = (boundsMax.xyz - cameraEye.xyz) * inverse;
    vec3 lo = min(t0, t1);
    vec3 hi = max(t0, t1);
    float first = max(max(lo.x, lo.y), max(lo.z, 0.0));
    float last = min(min(hi.x, hi.y), min(hi.z, sceneZ));
    if (last <= first) {
        fog = vec4(0.0);
        return;
    }

    const int steps = 16;
    float stepSize = (last - first) / float(steps);
    float integral = 0.0;
    for (int i = 0; i < steps; ++i) {
        vec3 p = cameraEye.xyz + ray * (first + (float(i) + 0.5) * stepSize);
        vec2 inside = min(p.xz - boundsMin.xz, boundsMax.xz - p.xz);
        float edge = clamp(min(inside.x, inside.y) / boundsMax.w, 0.0, 1.0);
        float density = 1.0;
        float floorY = boundsMin.y;
        float ceiling = boundsMax.y;
        if (grid.x >= 2.0) {
            vec2 uv = clamp((p.xz - boundsMin.xz) / (boundsMax.xz - boundsMin.xz), 0.0, 1.0);
            vec2 texel = uv * (grid.xy - 1.0) + 0.5;
            vec2 cell = textureLod(fogField, texel / vec2(textureSize(fogField, 0)), 0.0).rg;
            density = clamp(cell.x, 0.0, 1.5);
            floorY = cell.y / max(cell.x, 0.0001);
            ceiling = floorY + grid.z;
        }
        float height = p.y - floorY;
        float vertical = exp(-max(height, 0.0) / boundsMin.w) *
                         clamp(min(height, ceiling - p.y) / 8.0, 0.0, 1.0);
        integral += density * edge * vertical;
    }
    float alpha = 1.0 - exp(-integral * stepSize * length(ray) * cameraForward.w);
    fog = vec4(fogColor.rgb * alpha, alpha);
}
)glsl";
} // namespace Fast
