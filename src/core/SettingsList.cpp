#include "SettingsList.h"

#include <array>
#include <utility>

namespace {

// Core's own rows: the settings every WaddleOS device has.
void forEachCoreSetting(SettingVisitor visit, void* context) {
  CrossPointSettings& s = SETTINGS;
  // Load and save walk this list, so every condition here must be fixed before
  // settings load. Runtime probes belong in getSettingsList().

  // Enum settings are persisted as numeric values. Assign these labels by enum
  // value so a reordered menu or enum cannot silently swap their behavior.
  static constexpr auto SLEEP_SCREEN_VALUES = [] {
    std::array<StrId, CrossPointSettings::SLEEP_SCREEN_MODE_COUNT> values{};
    values[CrossPointSettings::DARK] = StrId::STR_DARK;
    values[CrossPointSettings::LIGHT] = StrId::STR_LIGHT;
    values[CrossPointSettings::CUSTOM] = StrId::STR_CUSTOM;
    values[CrossPointSettings::COVER] = StrId::STR_COVER;
    values[CrossPointSettings::COVER_CUSTOM] = StrId::STR_COVER_CUSTOM;
    values[CrossPointSettings::BLANK] = StrId::STR_NONE_OPT;
    values[CrossPointSettings::QUICK_RESUME] = StrId::STR_QUICK_RESUME;
    values[CrossPointSettings::TRANSPARENT_CUSTOM] = StrId::STR_TRANSPARENT;
    values[CrossPointSettings::COVER_TRANSPARENT] = StrId::STR_COVER_TRANSPARENT;
    return values;
  }();
  const auto emit = [visit, context](auto&& info) { visit(context, info); };
  // --- Display ---
  emit(SettingInfo::StaticEnum(StrId::STR_SLEEP_SCREEN, &s.sleepScreen, SLEEP_SCREEN_VALUES,
                               "sleepScreen", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &s.sleepScreenCoverMode,
                         {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &s.sleepScreenCoverFilter,
                         {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                         "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &s.hideBatteryPercentage,
                         {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                         StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_REFRESH_FREQ, &s.refreshFrequency,
                         {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15,
                          StrId::STR_PAGES_30, StrId::STR_NEVER},
                         "refreshFrequency", StrId::STR_CAT_DISPLAY));
  if (!BoardConfig::hasTouch())
    emit(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &s.fadingFix, "fadingFix",
                             StrId::STR_CAT_DISPLAY));
#if FREEINK_CAP_FRONTLIGHT
  emit(SettingInfo::Toggle(StrId::STR_RESTORE_LIGHT_ON_WAKE, &s.frontlightRestoreOnWake,
                           "frontlightRestoreOnWake", StrId::STR_CAT_DISPLAY));
#endif
  // Night mode = inverted output polarity everywhere (ActivityManager
  // applies it to every activity), so it lives in the Display category.
  emit(SettingInfo::Toggle(StrId::STR_NIGHT_MODE, &s.screenInverted, "screenInverted",
                           StrId::STR_CAT_DISPLAY));

  // --- Controls ---
#if FREEINK_CAP_HAPTIC
  emit(SettingInfo::Enum(StrId::STR_VIBRATION, &s.vibration,
                         {StrId::STR_VIBRATION_TOUCH, StrId::STR_VIBRATION_TOUCH_PAGE, StrId::STR_STATE_OFF},
                         "vibration", StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Enum(StrId::STR_HAPTIC_INTENSITY, &s.hapticIntensity,
                         {StrId::STR_HAPTIC_LOW, StrId::STR_HAPTIC_MEDIUM, StrId::STR_HAPTIC_HIGH}, "hapticIntensity",
                         StrId::STR_CAT_CONTROLS));
#endif
  emit(SettingInfo::Enum(
      StrId::STR_SIDE_BTN_LAYOUT, &s.sideButtonLayout,
      {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_DISABLED, StrId::STR_NEXT_NEXT, StrId::STR_PREV_PREV},
      "sideButtonLayout", StrId::STR_CAT_CONTROLS));
  if (!BoardConfig::hasTouch())
    emit(SettingInfo::Toggle(StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION, &s.frontButtonFollowOrientation,
                             "frontButtonFollowOrientation", StrId::STR_CAT_CONTROLS));
  // Double-click power frontlight shortcut only exists on the X4 Pro.
  if (BoardConfig::isX4Pro())
    emit(SettingInfo::Toggle(StrId::STR_DBL_CLICK_PWR_LIGHT, &s.doubleClickPwrLight,
                             "doubleClickPwrLight", StrId::STR_CAT_CONTROLS));
  emit(buildShortPowerButtonSetting());

  // --- System ---
  emit(SettingInfo::Value(
      StrId::STR_TIME_TO_SLEEP, &s.sleepTimeoutMinutes,
      {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
      "sleepTimeoutMinutes", StrId::STR_CAT_SYSTEM));

  // Frontlight quick-panel state: persisted and web-exposed, but hidden
  // from the on-device Settings screen because the swipe panel owns it.
  emit(SettingInfo::Value(StrId::STR_BRIGHTNESS, &s.frontlightBrightness, {0, 100, 5},
                          "frontlightBrightness"));
#if FREEINK_CAP_WARMLIGHT
  emit(SettingInfo::Value(StrId::STR_WARMTH, &s.frontlightWarmth, {0, 100, 5}, "frontlightWarmth"));
#endif
  emit(SettingInfo::Toggle(StrId::STR_FRONTLIGHT, &s.frontlightOn, "frontlightOn"));

  // Clock entries (persistence + web settings; the device UI is
  // ClockSettingsActivity under System settings).
  // LEGACY: retired quarter-hour UTC offset (biased by 48). Persisted so
  // timezones::activeIndex() can migrate it into clockTimezone.
  emit(SettingInfo::Value(StrId::STR_CLOCK, &s.clockUtcOffsetQ, {0, 104, 1}, "clockUtcOffsetQ",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &s.clockFormat,
                         {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  // Index into the append-only table in src/core/util/Timezones.cpp; 255 = unset.
  emit(SettingInfo::Value(StrId::STR_TIMEZONE, &s.clockTimezone, {0, 255, 1}, "clockTimezone",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_CLOCK_DST, &s.clockDst,
                         {StrId::STR_CLOCK_DST_AUTO, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, "clockDst",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_CLOCK_IN_HEADER, &s.clockShowInHeader, "clockShowHeader",
                           StrId::STR_CUSTOMISE_STATUS_BAR));
  if (BoardConfig::hasHomeKey()) {
    for (unsigned i = 0; i < 3; ++i) {
      emit(SettingInfo::StaticEnum(home_button::GESTURE_LABELS[i], &(s.*home_button::FIELDS[i]), home_button::ACTION_LABELS,
                                   home_button::KEYS[i], StrId::STR_CAT_CONTROLS));
    }
  }
}

}  // namespace

void forEachBaseSetting(SettingVisitor visit, void* context) {
  forEachCoreSetting(visit, context);
  for (const SettingsModule* module : settingsModules()) module->forEachSetting(visit, context);
}

std::vector<SettingInfo> getBaseSettingsList(const size_t extraCapacity) {
  // Count first so the one contiguous block is sized exactly.
  size_t count = 0;
  forEachBaseSetting([](void* context, SettingInfo&) { ++*static_cast<size_t*>(context); }, &count);
  std::vector<SettingInfo> v;
  v.reserve(count + extraCapacity);
  forEachBaseSetting(
      [](void* context, SettingInfo& info) {
        static_cast<std::vector<SettingInfo>*>(context)->push_back(std::move(info));
      },
      &v);
  return v;
}
