#pragma once

#include <BoardConfig.h>
#include <HalClock.h>
#include <HalFrontlight.h>
#include <HalTiltSensor.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "HomeButtonSettings.h"
#include "core/activities/settings/SettingsActivity.h"
#include "core/components/UITheme.h"
#include "SettingsModule.h"

struct ShortPowerButtonOption {
  StrId label;
  uint8_t value;
};

// Menu order differs from the persisted enum order, so map by value.
inline std::vector<ShortPowerButtonOption> shortPowerButtonOptions() {
  std::vector<ShortPowerButtonOption> options = {{StrId::STR_IGNORE, CrossPointSettings::IGNORE},
                                                 {StrId::STR_SLEEP, CrossPointSettings::SLEEP},
                                                 {StrId::STR_PAGE_TURN, CrossPointSettings::PAGE_TURN},
                                                 {StrId::STR_FORCE_REFRESH, CrossPointSettings::FORCE_REFRESH},
                                                 {StrId::STR_FOOTNOTES, CrossPointSettings::FOOTNOTES}};
  options.reserve(CrossPointSettings::SHORT_PWRBTN_COUNT);
  if (BoardConfig::hasTouch()) {
    options.push_back({StrId::STR_CONFIRM, CrossPointSettings::PWR_CONFIRM});
  } else {
    options.push_back({StrId::STR_SAVE_CLIPPING, CrossPointSettings::CREATE_CLIPPING});
  }
  // Touch boards without a home key have no button for these shortcuts.
  if (BoardConfig::hasTouch() && !BoardConfig::hasHomeKey()) {
    options.push_back({StrId::STR_BOOKMARK_OPTION, CrossPointSettings::PWR_BOOKMARK});
    options.push_back({StrId::STR_READER_MENU, CrossPointSettings::PWR_READER_MENU});
  }
  return options;
}

inline std::vector<StrId> buildShortPowerButtonValues() {
  std::vector<StrId> values;
  for (const auto& option : shortPowerButtonOptions()) values.push_back(option.label);
  return values;
}

inline uint8_t shortPowerButtonDisplayValue() {
  const auto options = shortPowerButtonOptions();
  for (size_t i = 0; i < options.size(); i++) {
    if (options[i].value == SETTINGS.shortPwrBtn) return static_cast<uint8_t>(i);
  }
  return CrossPointSettings::IGNORE;
}

inline void setShortPowerButtonFromDisplayValue(const uint8_t displayValue) {
  const auto options = shortPowerButtonOptions();
  SETTINGS.shortPwrBtn = displayValue < options.size() ? options[displayValue].value : CrossPointSettings::IGNORE;
}

inline SettingInfo buildShortPowerButtonSetting() {
  return SettingInfo::DynamicEnum(StrId::STR_SHORT_PWR_BTN, buildShortPowerButtonValues(), shortPowerButtonDisplayValue,
                                  setShortPowerButtonFromDisplayValue, "shortPwrBtn", StrId::STR_CAT_CONTROLS);
}

// Visits synchronously in list order. Borrowed references are valid only during
// the callback; move from the descriptor to retain ownership. Its board conditions
// must be fixed before settings load, or load and save would see different keys.
void forEachBaseSetting(SettingVisitor visit, void* context);

template <typename Fn>
void forEachBaseSetting(Fn& fn) {
  forEachBaseSetting([](void* context, SettingInfo& info) { (*static_cast<Fn*>(context))(info); }, &fn);
}

// Collects the same descriptors into an owning list for UI and web callers.
std::vector<SettingInfo> getBaseSettingsList(size_t extraCapacity = 0);

// UI callers own the list for runtime filtering and runtime options: the base
// rows, then each settings module adjusts its own (fonts, dictionaries, ...).
inline std::vector<SettingInfo> getSettingsList() {
  auto v = getBaseSettingsList(1);
  // Frontlit and lightless variants of a board share one binary (EEGO A4);
  // presence is the I2C probe result from Frontlight.begin(). UI-only: the probe
  // has not run at load time, so the base list must keep the persisted key.
  if (!Frontlight.present()) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SettingInfo& s) { return s.nameId == StrId::STR_RESTORE_LIGHT_ON_WAKE; }),
            v.end());
  }
  for (const SettingsModule* module : settingsModules()) module->adjustUiSettings(v);
  return v;
}
