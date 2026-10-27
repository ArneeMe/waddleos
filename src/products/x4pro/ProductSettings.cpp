#include "apps/reader/ReaderSettings.h"
#include "core/SettingsModule.h"

// The apps on the X4 Pro that keep settings, in the order they appear.
std::span<const SettingsModule* const> settingsModules() {
  static const SettingsModule* const modules[] = {&ReaderSettings::settingsModule()};
  return modules;
}
