#pragma once

#include <cstdint>

namespace Fast {
enum FlyCameraKey : uint32_t {
    FlyForward = 1u << 0,
    FlyBackward = 1u << 1,
    FlyLeft = 1u << 2,
    FlyRight = 1u << 3,
    FlyUp = 1u << 4,
    FlyDownLeft = 1u << 5,
    FlyDownRight = 1u << 6,
    FlyBoostLeft = 1u << 7,
    FlyBoostRight = 1u << 8,
};

bool TakeFlyCameraToggle();
uint32_t TakeFlyCameraKeys();
bool FlyCameraKeysHeld();
} // namespace Fast
