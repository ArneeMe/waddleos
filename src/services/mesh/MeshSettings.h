#pragma once

#include <cstdint>

#include "core/SettingsModule.h"

// The mesh unit's settings, saved in settings.json with the others.
//
// The unit is mostly a backup for when other ways to talk are gone. "Hjemme"
// it checks for messages rarely (once a day by default) and sleeps with the
// radio off in between; "På tur" it checks often, keeps the GPS on and shares
// its position. An SOS always goes out at once, in either mode.
struct MeshSettings {
  enum Check : uint8_t { CHECK_ALWAYS, CHECK_15_MIN, CHECK_HOUR, CHECK_6_HOURS, CHECK_DAY, CHECK_ON_WAKE };

  uint8_t onTrip = 0;  // 0 = Hjemme, 1 = På tur ("mesh.onTrip" in settings.json)
  uint8_t positionMinutes = 10;
  uint8_t homeCheck = CHECK_DAY;
  uint8_t tripCheck = CHECK_HOUR;
  uint8_t homeWifi = 1;  // checks at home also ask the server over Wi-Fi

  // How often the unit checks for messages in the current mode, in ms. 0 =
  // listens all the time; UINT32_MAX = only when woken.
  uint32_t checkIntervalMs() const {
    constexpr uint32_t MIN = 60UL * 1000;
    switch (onTrip ? tripCheck : homeCheck) {
      case CHECK_15_MIN:
        return 15 * MIN;
      case CHECK_HOUR:
        return 60 * MIN;
      case CHECK_6_HOURS:
        return 6 * 60 * MIN;
      case CHECK_DAY:
        return 24 * 60 * MIN;
      case CHECK_ON_WAKE:
        return UINT32_MAX;
      default:
        return 0;
    }
  }

  static MeshSettings& getInstance() {
    static MeshSettings instance;
    return instance;
  }
  static const SettingsModule& settingsModule();
};

#define MESH_SETTINGS MeshSettings::getInstance()
