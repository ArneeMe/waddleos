#pragma once
#include "WiFi.h"
namespace waddle::wifi {
inline bool joinSaved() {
  sim::wifiOn = true;
  return sim::wifiAvailable;
}
inline void off() { sim::wifiOn = false; }
}  // namespace waddle::wifi
