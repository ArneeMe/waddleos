#pragma once

// The GPS on the T5 S3 Pro (u-blox MIA-M10Q or Quectel L76K; which one ships is
// not known yet, so both are handled). It is powered together with the radio,
// so when it is not needed it is told to sleep instead of being switched off.

#include <cstdint>

namespace mesh::gps {

struct Fix {
  bool valid = false;
  uint32_t time = 0;  // unix seconds of the fix
  int32_t latE7 = 0;
  int32_t lonE7 = 0;
  int16_t altitude = 0;
  uint8_t satellites = 0;
};

// Starts reading the GPS (finds its speed and model). Call after the radio is up.
void wake();
// Reads what the GPS has sent. Call often while awake.
void poll();
// Asks the GPS to sleep (u-blox backup mode; an L76K keeps running).
void sleep();
bool awake();

// The newest fix, and when it was taken (millis), or valid = false.
const Fix& lastFix();
uint32_t lastFixAtMs();
// "u-blox M10", "L76K" or "ukjent" once the GPS has talked.
const char* model();

}  // namespace mesh::gps
