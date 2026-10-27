#pragma once
#include <cstddef>
#include <cstdint>
#include <random>
#include "SimArduino.h"
// Seeded per unit (by its directory), so a run can be repeated exactly.
namespace sim {
extern std::mt19937 rng;
}
inline uint32_t esp_random() { return uint32_t(sim::rng()); }
inline void esp_fill_random(void* buf, size_t len) {
  for (size_t i = 0; i < len; i++) static_cast<uint8_t*>(buf)[i] = uint8_t(sim::rng());
}
