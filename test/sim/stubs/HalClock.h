#pragma once
#include <ctime>
#include "SimArduino.h"
struct SimClock {
  bool setUtc(time_t) { return true; }
};
inline SimClock halClock;
