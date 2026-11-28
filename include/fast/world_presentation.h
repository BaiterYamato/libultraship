#pragma once

#include "ship/Api.h"

// Subcomando de FAST_GFX_WORLD_POSTPROCESS; 1-4 pertencem às capturas.
#define FAST_WORLD_PRESENT_COMMAND 5u

// Chamadas na thread gráfica. Lista imutável até FastEndWorldPresentationFrame.
// Begin invalida qualquer prova anterior, inclusive quando o pool é reutilizado.
API_EXPORT void FastBeginWorldPresentationFrame(const void* root);
// Publicar somente após emitir a fronteira alcançável na lista root deste quadro.
API_EXPORT void FastSetWorldPresentationBoundary(const void* root, const void* marker);
// Encerrar depois de TODOS os replays interpolados, mesmo se foram descartados.
API_EXPORT void FastEndWorldPresentationFrame(void);
