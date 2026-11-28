#pragma once

#include <cstdint>

// RFC 0034 (emenda de 22/11): grade fina de até FAST_FOG_DETAIL_SIDE por lado, para solvers finos no mod
// (Stable Fluids). As células ficam fora do FastFogVolume, cujo layout outros TUs já compilados conhecem;
// o volume continua levando bounds/dimensões em detailBounds/detailGrid. Escrito na thread do jogo pelo
// submit da névoa. gFastFogDetailCells é só o canal de publicação do host: FastQueueFogVolume copia as células
// no snapshot do ticket e gfx_draw_fog_volume as restaura em gFastFogDetailReplay, que é o que o backend lê.
#define FAST_FOG_DETAIL_SIDE 128

struct FastFogDetailCells {
    uint32_t width = 0, height = 0;
    float cells[FAST_FOG_DETAIL_SIDE * FAST_FOG_DETAIL_SIDE][4]{}; // density, velocity x, velocity z, 0
};

inline FastFogDetailCells gFastFogDetailCells;
inline FastFogDetailCells gFastFogDetailReplay; // width/height 0 = o ticket consumido não tem detalhe
