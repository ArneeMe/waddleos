#pragma once

#include <cstdint>

#include "core/SettingsModule.h"

// The mesh unit's settings, saved in settings.json with the others.
struct MeshSettings {
  // Off-grid mode: GPS on, position shared with the group every positionMinutes.
  uint8_t offGrid = 0;
  uint8_t positionMinutes = 10;

  static MeshSettings& getInstance() {
    static MeshSettings instance;
    return instance;
  }
  static const SettingsModule& settingsModule();
};

#define MESH_SETTINGS MeshSettings::getInstance()
