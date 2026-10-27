#pragma once
#include <cstdint>

#include "SimArduino.h"
struct SimPower {
  uint16_t getBatteryPercentage() const { return 76; }
};
inline SimPower powerManager;
