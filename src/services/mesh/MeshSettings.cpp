#include "MeshSettings.h"

#include <I18n.h>

#include "core/SettingsList.h"

namespace {

constexpr StrId CHECKS[] = {StrId::STR_CHECK_ALWAYS, StrId::STR_CHECK_15_MIN, StrId::STR_CHECK_HOUR,
                            StrId::STR_CHECK_6_HOURS, StrId::STR_CHECK_DAY,   StrId::STR_CHECK_ON_WAKE};

void forEachSetting(SettingVisitor visit, void* context) {
  MeshSettings& s = MESH_SETTINGS;
  const StrId cat = StrId::STR_CAT_SYSTEM;
  SettingInfo mode = SettingInfo::Enum(StrId::STR_OFF_GRID_MODE, &s.onTrip, {StrId::STR_MODE_HOME, StrId::STR_MODE_TRIP},
                                       "mesh.offGrid", cat);
  visit(context, mode);
  SettingInfo home = SettingInfo::StaticEnum(StrId::STR_CHECK_HOME, &s.homeCheck, CHECKS, "mesh.homeCheck", cat);
  visit(context, home);
  SettingInfo wifi = SettingInfo::Toggle(StrId::STR_CHECK_WIFI, &s.homeWifi, "mesh.homeWifi", cat);
  visit(context, wifi);
  // On a trip "only when I wake it" makes no sense: the list stops before it.
  SettingInfo trip = SettingInfo::StaticEnum(StrId::STR_CHECK_TRIP, &s.tripCheck, std::span<const StrId>(CHECKS, 4),
                                             "mesh.tripCheck", cat);
  visit(context, trip);
  SettingInfo minutes = SettingInfo::Value(StrId::STR_POSITION_MINUTES, &s.positionMinutes, {5, 60, 5},
                                           "mesh.positionMinutes", cat);
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
