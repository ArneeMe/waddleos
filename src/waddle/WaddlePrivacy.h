#pragma once

#include <string>

// WaddleOS privacy build (WADDLE_PRIVATE): radio and network identity.
namespace waddle::privacy {

// Neutral DHCP hostname and user agent, no model name or MAC in them.
constexpr const char* HOSTNAME = "reader";
constexpr const char* USER_AGENT = "reader";
constexpr const char* HOTSPOT_SSID = "Waddle";

// Call after WiFi.mode(WIFI_STA) and before WiFi.begin(). Sets the neutral
// hostname and a MAC address derived from this device and `ssid`: the same
// network always sees the same address (so DHCP reservations keep working),
// but no two networks see the same one, and none sees the factory MAC.
void prepareStation(const char* ssid);

// Call after WiFi.mode(WIFI_STA) and before a scan. Uses a new random MAC
// address for every scan, so probe requests cannot be linked to each other.
void prepareScan();

// WPA2 password for the hotspot: 10 characters derived from this device, so it
// stays the same but is not in the source code.
std::string hotspotPassword();

}  // namespace waddle::privacy
