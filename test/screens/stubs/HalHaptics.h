#pragma once
#include <cstdint>
class HalHaptics {
 public:
  static void longPress(bool, uint8_t) {}
  static void feedback(bool, bool, uint8_t) {}
};
