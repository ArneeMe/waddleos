#include "T5AppsActivity.h"
#include "T5SettingsActivity.h"
#include "apps/news/NewsActivity.h"
#include "apps/news/NewsPaper.h"
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

void openT5App(const T5AppsActivity::Row row) {
  switch (row) {
    case T5AppsActivity::BOOKS:
      activityManager.goToLibrary();
      break;
    case T5AppsActivity::NEWS:
      activityManager.replaceActivity(std::make_unique<NewsActivity>(
          activityManager.getRenderer(), activityManager.getMappedInput(), waddle::news::due()));
      break;
    default:
      break;
  }
}
