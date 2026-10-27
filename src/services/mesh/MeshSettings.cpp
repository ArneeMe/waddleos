#include "MeshSettings.h"

#include <I18n.h>

#include "core/SettingsList.h"

namespace {

void forEachSetting(SettingVisitor visit, void* context) {
  MeshSettings& s = MESH_SETTINGS;
  SettingInfo offGrid =
      SettingInfo::Toggle(StrId::STR_OFF_GRID_MODE, &s.offGrid, "mesh.offGrid", StrId::STR_CAT_SYSTEM);
  visit(context, offGrid);
  SettingInfo minutes = SettingInfo::Value(StrId::STR_POSITION_MINUTES, &s.positionMinutes, {5, 60, 5},
                                           "mesh.positionMinutes", StrId::STR_CAT_SYSTEM);
  visit(context, minutes);
}

void toJson(JsonDocument&) {}
bool fromJson(JsonVariantConst) { return false; }
void adjustUiSettings(std::vector<SettingInfo>&) {}

}  // namespace

const SettingsModule& MeshSettings::settingsModule() {
  static const SettingsModule module = {forEachSetting, toJson, fromJson, adjustUiSettings};
  return module;
}
