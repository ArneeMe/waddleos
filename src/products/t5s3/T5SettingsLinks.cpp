#include "T5SettingsActivity.h"
#include "core/activities/network/WifiSelectionActivity.h"
#include "core/activities/settings/ClockSettingsActivity.h"
#include "core/waddle/UpdateActivity.h"

void openT5Setting(const T5SettingsActivity::Row row) {
  auto& renderer = activityManager.getRenderer();
  auto& input = activityManager.getMappedInput();
  switch (row) {
    case T5SettingsActivity::WIFI:
      activityManager.replaceActivity(std::make_unique<WifiSelectionActivity>(renderer, input, false));
      break;
    case T5SettingsActivity::CLOCK:
      activityManager.replaceActivity(std::make_unique<ClockSettingsActivity>(renderer, input));
      break;
    case T5SettingsActivity::UPDATE:
      activityManager.replaceActivity(std::make_unique<UpdateActivity>(renderer, input));
      break;
    case T5SettingsActivity::READING:
      activityManager.goToSettings(SettingsSection::Reader);
      break;
    default:
      break;
  }
}
