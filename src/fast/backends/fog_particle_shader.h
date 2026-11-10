#pragma once

namespace Fast {
inline constexpr char FogParticleShader[] = R"hlsl(
struct Particle {
    float2 position;
    float age, life;
    float radius, strength, phase, padding;
};

Texture2D<float4> field : register(t0);
// Match FastFogFlowCell. Only the initial floor and edge mask are used here.
struct FogCell {
    float4 initial;
    uint open;
    float4 beforeA;
    float3 beforeB;
    float4 afterA;
    float3 afterB;
    float3 vertical;
};
StructuredBuffer<FogCell> topology : register(t2);
cbuffer FlowConstants : register(b0) {
    uint width, height, seed;
    float layerHeight;
    float dt, stepX, stepZ, damping;
    uint tick, particleCount;
    float restDensity, recovery;
    float2 particleShift;
    uint particleSeed;
    float particleDt;
};

bool InsideField(int2 p) {
    return all(p >= 0) && all(p < int2(width, height));
}

FogCell CellAt(int2 p) {
    return topology[p.y * width + p.x];
}

bool CanCrossFace(int2 from, int2 to, uint direction) {
    if (!InsideField(to)) return false;
    FogCell a = CellAt(from), b = CellAt(to);
    return (a.open & 1u) && (b.open & 1u) && (a.open & (2u << direction)) == 0 &&
           (b.open & (2u << (direction ^ 1u))) == 0 && abs(a.initial.y - b.initial.y) < layerHeight;
}

#if FOG_PARTICLE_DEPOSIT == 2
RWStructuredBuffer<uint2> pathMasks : register(u0);

bool CanDeposit(int2 from, int2 target) {
    [loop]
    for (uint step = 0; step < 5; ++step) {
        int2 delta = target - from;
        if (all(delta == 0)) return true;
        bool alongX = abs(delta.x) >= abs(delta.y);
        int2 movement = alongX ? int2(sign(delta.x), 0) : int2(0, sign(delta.y));
        uint direction = alongX ? (delta.x > 0 ? 1u : 0u) : (delta.y > 0 ? 3u : 2u);
        if (!CanCrossFace(from, from + movement, direction)) return false;
        from += movement;
    }
    return false;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    int2 target = int2(id.xy);
    uint2 mask = 0;
    if (CellAt(target).open & 1u) {
        [loop]
        for (int z = 0; z < 7; ++z) {
            [loop]
            for (int x = 0; x < 7; ++x) {
                int2 from = target + int2(x - 3, z - 3);
                if (InsideField(from) && CanDeposit(from, target)) {
                    uint bit = uint(z * 7 + x);
                    if (bit < 32u) mask.x |= 1u << bit;
                    else mask.y |= 1u << (bit - 32u);
                }
            }
        }
    }
    pathMasks[id.y * width + id.x] = mask;
}
#elif FOG_PARTICLE_DEPOSIT == 1
StructuredBuffer<Particle> particles : register(t1);
StructuredBuffer<uint2> pathMasks : register(t3);
RWTexture2D<float4> volumeField : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    float4 state = field.Load(int3(id.xy, 0));
    if (state.x < 0.0001) {
        volumeField[id.xy] = state;
        return;
    }
    float2 wind = state.zw + float2(16.0, 5.0);
    float2 direction = wind * rsqrt(max(dot(wind, wind), 0.001));
    uint2 mask = pathMasks[id.y * width + id.x];
    float wisps = 0.0;
    [loop]
    for (uint i = 0; i < particleCount; ++i) {
        Particle particle = particles[i];
        float2 offset = float2(id.xy) - particle.position;
        float along = dot(offset, direction);
        float distance2 = dot(offset, offset) - 0.45 * along * along;
        float shape = saturate(1.0 - distance2 / (particle.radius * particle.radius));
        float fade = saturate(particle.age * 4.0) * saturate((particle.life - particle.age) * 2.0);
        [branch]
        if (shape > 0.0 && fade > 0.0) {
            int2 origin = int2(round(particle.position)) - int2(id.xy) + 3;
            if (all(origin >= 0) && all(origin < 7)) {
                uint bit = uint(origin.y * 7 + origin.x);
                uint bits = bit < 32u ? mask.x : mask.y;
                if (bits & (1u << (bit & 31u))) wisps += shape * shape * fade * particle.strength;
            }
        }
    }
    float motion = saturate(length(state.zw) / 140.0);
    float gain = min(0.78 + (0.32 + 0.50 * motion) * saturate(wisps), 1.5 / state.x);
    // Only the display texture changes. The simulation keeps its original density and velocities.
    volumeField[id.xy] = float4(state.xy * gain, state.zw);
}
#else
RWStructuredBuffer<Particle> particles : register(u0);

float Random(uint hash) {
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    hash *= 0x846ca68bu;
    hash ^= hash >> 16;
    return float(hash & 0x00ffffffu) / 16777216.0;
}

Particle Spawn(uint index) {
    uint key = (index + 1u) * 6151u + tick * 1009u;
    Particle particle;
    bool placed = false;
    [loop]
    for (uint attempt = 0; attempt < 4; ++attempt) {
        uint sampleKey = key + 103u * attempt;
        particle.position = float2(Random(sampleKey), Random(sampleKey + 1u)) * float2(width - 1u, height - 1u);
        int2 p = int2(round(particle.position));
        if ((CellAt(p).open & 1u) && field.Load(int3(p, 0)).x > 0.02) {
            placed = true;
            break;
        }
    }
    particle.life = lerp(1.5, 4.0, Random(key + 2u));
    particle.age = placed && particleSeed != 0 ? Random(key + 3u) * particle.life : 0.0;
    particle.radius = lerp(0.9, 2.1, Random(key + 4u));
    particle.strength = lerp(0.35, 0.8, Random(key + 5u));
    particle.phase = Random(key + 6u) * 6.2831853;
    particle.padding = 0.0;
    return particle;
}

[numthreads(32, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= particleCount) return;
    if (particleSeed != 0) {
        particles[id.x] = Spawn(id.x);
        return;
    }
    Particle particle = particles[id.x];
    particle.position -= particleShift;
    const float2 limit = float2(width - 1u, height - 1u);
    if (any(particle.position < 0.0) || any(particle.position > limit)) {
        particles[id.x] = Spawn(id.x);
        return;
    }
    int2 cell = int2(round(particle.position));
    if ((CellAt(cell).open & 1u) == 0) {
        particles[id.x] = Spawn(id.x);
        return;
    }
    float4 state = field.Load(int3(cell, 0));
    float phase = particle.phase + particle.age * 2.0;
    float2 wind = state.zw + float2(16.0, 5.0) + float2(cos(phase), sin(phase)) * 8.0;
    float2 movement = clamp(wind * particleDt / float2(stepX, stepZ), -0.49, 0.49);
    float2 next = particle.position + movement;
    particle.age += particleDt;
    if (particle.age >= particle.life || any(next < 0.0) || any(next > limit)) {
        particle = Spawn(id.x);
    } else {
        int2 target = int2(round(next));
        if (target.x != cell.x) {
            if (CanCrossFace(cell, int2(target.x, cell.y), movement.x > 0.0 ? 1u : 0u)) cell.x = target.x;
            else next.x = clamp(next.x, cell.x - 0.499, cell.x + 0.499);
        }
        if (target.y != cell.y && !CanCrossFace(cell, int2(cell.x, target.y), movement.y > 0.0 ? 3u : 2u)) {
            next.y = clamp(next.y, cell.y - 0.499, cell.y + 0.499);
        }
        particle.position = next;
    }
    particles[id.x] = particle;
}
#endif
)hlsl";
} // namespace Fast
