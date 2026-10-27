#pragma once
#include "SimArduino.h"
enum { WL_DISCONNECTED = 0, WL_CONNECTED = 3 };
namespace sim {
extern bool wifiAvailable;  // a known network is in range
extern bool wifiOn;
}  // namespace sim
struct SimWiFi {
  int status() const { return sim::wifiOn && sim::wifiAvailable ? WL_CONNECTED : WL_DISCONNECTED; }
};
inline SimWiFi WiFi;
