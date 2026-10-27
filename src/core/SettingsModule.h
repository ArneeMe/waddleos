#pragma once

#include <ArduinoJson.h>

#include <span>
#include <vector>

struct SettingInfo;

using SettingVisitor = void (*)(void* context, SettingInfo& info);

// Settings that belong to an app rather than to core. Core owns the machinery
// (one settings.json, the Settings screen, the web settings page); an app hands
// it one of these to add its own rows and keys. Each device lists the apps whose
// settings it has in settingsModules(), in products/<device>/.
struct SettingsModule {
  // Its rows, visited the same way as forEachBaseSetting(). Keys must stay
  // unique across core and every module, and must never change once shipped.
  void (*forEachSetting)(SettingVisitor visit, void* context);
  // Keys the generic row loop does not cover (dynamic options, legacy fields).
  void (*toJson)(JsonDocument& doc);
  // Reads those keys back; returns true when the file should be saved again
  // in the current format.
  bool (*fromJson)(JsonVariantConst doc);
  // Adjusts the rows shown on the device and the web page: options found at
  // runtime (fonts, dictionaries) and rows the hardware cannot use.
  void (*adjustUiSettings)(std::vector<SettingInfo>& settings);
};

// The modules this device has, in Settings order. Defined by each product.
std::span<const SettingsModule* const> settingsModules();
