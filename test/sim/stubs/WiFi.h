#pragma once
#include "SimArduino.h"
enum { WL_DISCONNECTED = 0, WL_CONNECTED = 3 };
enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 };
namespace sim {
extern bool wifiAvailable;  // a known network is in range
extern bool wifiOn;
}  // namespace sim
struct SimWiFi {
  int status() const { return sim::wifiOn && sim::wifiAvailable ? WL_CONNECTED : WL_DISCONNECTED; }
  int getMode() const { return sim::wifiOn ? WIFI_STA : WIFI_OFF; }
};
inline SimWiFi WiFi;
