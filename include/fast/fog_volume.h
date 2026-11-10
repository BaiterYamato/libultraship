#ifndef FAST_FOG_VOLUME_H
#define FAST_FOG_VOLUME_H

#include <stdint.h>

/* Render-thread input referenced by a ticket in the display list. Arrays match HLSL float4. */
#define FAST_GFX_FOG_VOLUME 0x4f
#define FAST_FOG_GRID_SIDE 32
#define FAST_FOG_FLOW_PARTICLES 1u
#define FAST_FOG_FLOW_VORTICES 4u
#define FAST_FOG_FLOW_LIFT 8u
#define FAST_FOG_FLOW_VERTICAL_COMPUTE 256u
#define FAST_FOG_FLOW_CPU_FIELD 512u
#define FAST_FOG_VORTEX_REST_DENSITY 0.60f
#define FAST_FOG_PARTICLE_COUNT 96
#define FAST_FOG_LIGHT_COUNT 6

typedef struct FastFogLight {
    float positionRadius[4];
    float color[4];
} FastFogLight;

typedef struct FastFogVolume {
    float eye[4];       /* xyz; debug mode in w */
    float forward[4];   /* xyz; density in w */
    float right[4];     /* xyz; projection ray scale X in w */
    float up[4];        /* xyz; projection ray scale Y in w */
    float depth[4];     /* projection zz, zw, wz; maximum view depth */
    float minimum[4];   /* world box xyz; vertical falloff scale in w */
    float maximum[4];   /* world box xyz; edge feather in w */
    float color[4];
    float grid[4];      /* width, depth, layer height, quality (0 full, 1 half, 2 temporal) */
    /* Row-major density and density-weighted ground height. Width zero selects static fog. */
    float cells[FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE][2];
    uint32_t audit;
    float interpolation; /* fraction between gameplay ticks used by the current world render */
    uint32_t lightCount;
    FastFogLight lights[FAST_FOG_LIGHT_COUNT];
    uint32_t upperCount;
    float upperCells[FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE][2];
    float atmosphere[4]; /* scene fog RGB and distant blend strength */
} FastFogVolume;

typedef struct FastFogImpulse {
    float scale;
    float vx, vz;
    float minX, maxX, minZ, maxZ;
} FastFogImpulse;

typedef struct FastFogFlowCell {
    float initial[4]; /* density, floor, velocity X, velocity Z */
    uint32_t open; /* bit 0: open; bits 1–4: blocked edges in -X, +X, -Z, +Z order */
    FastFogImpulse before, after;
    float vertical[3]; /* raised density, vertical speed, before-step lift */
} FastFogFlowCell;

typedef struct FastFogFlow {
    uint64_t stream;
    uint32_t tick;
    float dt;
    uint32_t flags;
    FastFogFlowCell cells[FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE];
    float raisedFraction[FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE];
    float upperRaisedFraction[FAST_FOG_GRID_SIDE * FAST_FOG_GRID_SIDE];
    float maxRaisedFraction;
} FastFogFlow;

#ifdef __cplusplus
enum class FogFlowAction { Seed, Advance, Reuse, Stale };

struct FogFlowConstants {
    uint32_t width, height, seed;
    float layerHeight;
    float dt, stepX, stepZ, damping;
    uint32_t tick, particleCount;
    float restDensity, recovery;
    float particleShiftX, particleShiftZ;
    uint32_t particleSeed;
    float particleDt;
};

struct FogTemporalConstants {
    float low[4];
    float previousEye[4], previousForward[4], previousRight[4], previousUp[4];
};

struct FogFieldBlendConstants {
    uint32_t width, height;
    float fraction;
    uint32_t vertical;
};

struct FogFlowCursor {
    uint64_t stream = 0;
    uint32_t tick = 0, width = 0, depth = 0;

    FogFlowAction Next(FogFlowCursor incoming) const {
        if (!stream || incoming.stream > stream) return FogFlowAction::Seed;
        const uint32_t elapsed = incoming.tick - tick;
        if (incoming.stream < stream || elapsed > 0x80000000u) return FogFlowAction::Stale;
        if (incoming.width != width || incoming.depth != depth || elapsed > 1u) return FogFlowAction::Seed;
        return elapsed == 1u ? FogFlowAction::Advance : FogFlowAction::Reuse;
    }
};

extern "C" {
#endif
/* Copies the snapshot. A zero ticket means no volume. */
uint32_t FastQueueFogVolume(const FastFogVolume* volume, const FastFogFlow* flow,
                            const void* viewing, const void* projection);
#ifdef __cplusplus
}
#endif

#endif
