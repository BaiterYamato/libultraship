#pragma once

namespace Fast {
inline constexpr char FogFlowShader[] = R"hlsl(
#ifndef FOG_VERTICAL
#define FOG_VERTICAL 0
#endif

struct Impulse {
    float scale;
    float vx, vz;
    float minX, maxX, minZ, maxZ;
};

struct Cell {
    float4 initial; // density, floor, velocity X and Z
    uint open;
    Impulse beforeStep, afterStep;
    float3 vertical;
};

Texture2D<float4> previousField : register(t0);
StructuredBuffer<Cell> cells : register(t1);
RWTexture2D<float4> nextField : register(u0);
#if FOG_VERTICAL
Texture2D<float4> previousVertical : register(t2);
RWTexture2D<float4> nextVertical : register(u1);
#endif

cbuffer FlowConstants : register(b0) {
    uint width, height, seed;
    float layerHeight;
    float dt, stepX, stepZ, damping;
    uint tick, particleCount;
    float restDensity, recovery;
};

float3 Apply(float3 value, Impulse impulse) {
    return float3(value.x * impulse.scale,
                  clamp(value.y + impulse.vx, impulse.minX, impulse.maxX),
                  clamp(value.z + impulse.vz, impulse.minZ, impulse.maxZ));
}

float3 ReadState(int2 p, Cell cell) {
    return Apply(previousField.Load(int3(p, 0)).xzw, cell.beforeStep);
}

#if FOG_VERTICAL
float2 ReadVertical(int2 p, Cell cell) {
    float2 motion = previousVertical.Load(int3(p, 0)).xy;
    return float2(motion.x * cell.beforeStep.scale, clamp(motion.y + cell.vertical.z, -84.0, 84.0));
}
#endif

float3 Advect(int2 p, float floorY, float3 current, float weight) {
    if (any(p < 0) || any(p >= int2(width, height))) return 0;
    Cell neighbor = cells[p.y * width + p.x];
    if ((neighbor.open & 1u) == 0 || abs(neighbor.initial.y - floorY) >= layerHeight) return 0;
    return (ReadState(p, neighbor) - current) * min(weight, 0.45);
}

static const int2 offsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
static const float faceWind[4] = { -16.0, 16.0, -5.0, 5.0 };

bool OpenNeighbor(int2 p, int direction, Cell cell, out int2 next, out Cell neighbor) {
    next = p + offsets[direction];
    neighbor = cell;
    if (any(next < 0) || any(next >= int2(width, height)) || (cell.open & (2u << direction)) != 0) return false;
    neighbor = cells[next.y * width + next.x];
    return (neighbor.open & 1u) != 0 && (neighbor.open & (2u << (direction ^ 1))) == 0 &&
           abs(neighbor.initial.y - cell.initial.y) < layerHeight;
}

float CurlAt(int2 p, Cell cell, float2 center) {
    float2 velocity[4];
    [unroll]
    for (int d = 0; d < 4; ++d) {
        int2 next;
        Cell neighbor;
        velocity[d] = center;
        if (OpenNeighbor(p, d, cell, next, neighbor)) velocity[d] = ReadState(next, neighbor).yz;
    }
    return 0.5 * (velocity[1].y - velocity[0].y - (velocity[3].x - velocity[2].x));
}

float3 MoveFog(int2 p, Cell cell, float3 src, out float2 nextLift) {
    float density = src.x;
    nextLift = 0.0;
#if FOG_VERTICAL
    float2 srcLift = ReadVertical(p, cell);
    float raised = srcLift.x;
    float verticalAround[4];
#endif
    float2 ratio = dt / float2(stepX, stepZ);
    float curl = CurlAt(p, cell, src.yz);
    float curlAround[4];
    float2 velocityAround[4];
    [unroll]
    for (int d = 0; d < 4; ++d) {
        curlAround[d] = abs(curl);
        velocityAround[d] = src.yz;
#if FOG_VERTICAL
        verticalAround[d] = srcLift.y;
#endif
        int2 next;
        Cell neighbor;
        if (OpenNeighbor(p, d, cell, next, neighbor)) {
            float3 other = ReadState(next, neighbor);
            float sum = d < 2 ? src.y + other.y : src.z + other.z;
            float speed = (d & 1 ? 0.5 : -0.5) * sum + faceWind[d];
            float timeOverCell = d < 2 ? ratio.x : ratio.y;
            float flux = clamp(speed * timeOverCell, -0.24, 0.24);
            float background = clamp(faceWind[d] * timeOverCell, -0.24, 0.24) * restDensity;
            density -= flux * (flux >= 0.0 ? src.x : other.x) - background;
#if FOG_VERTICAL
            float2 otherLift = ReadVertical(next, neighbor);
            raised -= flux * (flux >= 0.0 ? srcLift.x : otherLift.x);
            verticalAround[d] = otherLift.y;
#endif
            curlAround[d] = abs(CurlAt(next, neighbor, other.yz));
            velocityAround[d] = other.yz;
        }
    }
    float2 flow = src.yz + float2(16.0, 5.0);
    float2 weights = min(abs(flow) * ratio, 0.45);
    float2 gradient = float2(curlAround[1] - curlAround[0], curlAround[3] - curlAround[2]);
    float confinement = 3.8 * dt * curl / max(length(gradient), 1.0);
    float2 velocity = src.yz + (velocityAround[flow.x >= 0.0 ? 0 : 1] - src.yz) * weights.x +
                      (velocityAround[flow.y >= 0.0 ? 2 : 3] - src.yz) * weights.y +
                      float2(gradient.y, -gradient.x) * confinement;
    float nextDensity = lerp(saturate(density), restDensity, min(dt * recovery, 1.0));
#if FOG_VERTICAL
    float vertical = srcLift.y + (verticalAround[flow.x >= 0.0 ? 0 : 1] - srcLift.y) * weights.x +
                     (verticalAround[flow.y >= 0.0 ? 2 : 3] - srcLift.y) * weights.y;
    if (raised > 0.0 || vertical > 0.0) {
        raised = clamp(raised, 0.0, nextDensity);
        float fraction = raised / max(nextDensity, 0.0001);
        float speed = (vertical - dt * (24.0 + 112.0 * fraction)) * exp(-2.4 * dt);
        float available = speed > 0.0 ? nextDensity - raised : min(10.0 * raised, nextDensity);
        raised = clamp(raised + speed * dt / (0.65 * layerHeight) * available, 0.0, 0.70 * nextDensity);
        if (raised > 0.0005 || speed > 0.0) nextLift = float2(raised, speed);
    }
#endif
    return float3(nextDensity, clamp(velocity * damping, -180.0, 180.0));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    Cell cell = cells[id.y * width + id.x];
    if ((cell.open & 1u) == 0) {
        nextField[id.xy] = 0;
#if FOG_VERTICAL
        nextVertical[id.xy] = 0;
#endif
        return;
    }

    float3 value = cell.initial.xzw;
    float2 lifted = cell.vertical.xy;
    if (seed == 0) {
        int2 p = int2(id.xy);
        float3 src = ReadState(p, cell);
        if (restDensity > 0.0 && restDensity < 1.0) {
            value = MoveFog(p, cell, src, lifted);
        } else {
            float2 wind = src.yz + float2(16.0, 5.0);
            value = src;
            if ((cell.open & (wind.x >= 0.0 ? 2u : 4u)) == 0) {
                value += Advect(p + int2(wind.x >= 0.0 ? -1 : 1, 0), cell.initial.y, src, abs(wind.x) * dt / stepX);
            }
            if ((cell.open & (wind.y >= 0.0 ? 8u : 16u)) == 0) {
                value += Advect(p + int2(0, wind.y >= 0.0 ? -1 : 1), cell.initial.y, src, abs(wind.y) * dt / stepZ);
            }
            value.x = lerp(value.x, 1.0, min(dt * 0.68, 1.0));
            value.yz *= damping;
        }
        value = Apply(value, cell.afterStep);
        lifted.x *= cell.afterStep.scale;
    }
    // Preserve weighted floor in the second channel for the volume's bilinear sampling.
    nextField[id.xy] = float4(value.x, value.x * cell.initial.y, value.y, value.z);
#if FOG_VERTICAL
    nextVertical[id.xy] = float4(lifted, min(0.70, lifted.x / max(value.x, 0.0001)), 0.0);
#endif
}
)hlsl";
} // namespace Fast
