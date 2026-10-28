#pragma once
// Buttons and touch: nobody presses anything in the screen simulator.
#include <Arduino.h>
class HalGPIO {
 public:
  bool isPressed(uint8_t) const { return false; }
  bool wasPressed(uint8_t) const { return false; }
  bool wasReleased(uint8_t) const { return false; }
  bool hasTouch() const { return true; }
  bool hasEdgeSideButtons() const { return false; }
  bool deviceIsX3() const { return false; }
  bool isUsbConnected() const { return false; }
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};
inline HalGPIO gpio;
