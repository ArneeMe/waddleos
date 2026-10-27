#pragma once
// The simulated world shared by stubs.cpp and screens.cpp.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "services/mesh/MeshGps.h"

class Activity;

namespace sim {
extern uint32_t nowMs;    // millis()
extern uint32_t nowUnix;  // time()
extern std::vector<std::vector<uint8_t>> rxQueue;  // packets the radio hears next
extern std::vector<std::vector<uint8_t>> sent;     // packets the radio sent
extern mesh::gps::Fix fix;                         // the GPS
extern std::unique_ptr<Activity> opened;           // the last screen the firmware asked to open
// The next wasScreenTapped() answers this point, once.
void tap(int x, int y);
}  // namespace sim
