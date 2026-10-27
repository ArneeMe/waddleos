#include "apps/reader/ReaderSettings.h"
#include "core/SettingsModule.h"
#include "services/mesh/MeshSettings.h"

// The apps on the mesh unit that keep settings, in the order they appear.
std::span<const SettingsModule* const> settingsModules() {
  static const SettingsModule* const modules[] = {&ReaderSettings::settingsModule(), &MeshSettings::settingsModule()};
  return modules;
}
