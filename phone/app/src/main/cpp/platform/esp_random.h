#pragma once
// The phone's random numbers: the system's cryptographic generator (keys are
// made from these), not the simulator's repeatable one.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
inline uint32_t esp_random() { return arc4random(); }
inline void esp_fill_random(void* buf, size_t len) { arc4random_buf(buf, len); }
