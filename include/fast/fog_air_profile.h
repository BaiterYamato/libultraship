#pragma once

#include <cstdint>

namespace Fast {
// Used only in the private FastFogFlow snapshot, outside the provider API.
inline constexpr uint32_t KokiriAirFlowFlag = 0x40000000u;
inline constexpr float KokiriAirHalfHeight = 56.0f;
}
