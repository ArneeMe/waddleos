#pragma once

#include <ArduinoJson.h>
#include <I18nKeys.h>
#include <BlePageTurner.h>
#include <PersistableStore.h>

#include <cstdint>

#include "core/util/HomeButtonInput.h"

class CrossPointSettings : public PersistableStore<CrossPointSettings> {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  friend class PersistableStore<CrossPointSettings>;

 public:
  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    COVER_CUSTOM = 4,
    BLANK = 5,
    QUICK_RESUME = 6,
    TRANSPARENT_CUSTOM = 7,
    COVER_TRANSPARENT = 8,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };

  // Auto follows the timezone's baked DST rule; On/Off override it — the
  // escape hatch for a zone whose law changed before the firmware caught up.
  enum CLOCK_DST_MODE { CLOCK_DST_AUTO = 0, CLOCK_DST_ON = 1, CLOCK_DST_OFF = 2, CLOCK_DST_MODE_COUNT };


  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Up = Previous, Down = Next. NEXT_NEXT / PREV_PREV assign both
  // buttons to the same direction for one-handed reading.
  enum SIDE_BUTTON_LAYOUT {
    PREV_NEXT = 0,
    NEXT_PREV = 1,
    SIDE_BUTTONS_DISABLED = 2,
    NEXT_NEXT = 3,
    PREV_PREV = 4,
    SIDE_BUTTON_LAYOUT_COUNT
  };


  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes).
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_NEVER = 5,
    REFRESH_FREQUENCY_COUNT
  };

  // Short power button press actions. Touch boards get PWR_CONFIRM in place of
  // CREATE_CLIPPING; touch boards without a home key also get the reader
  // shortcuts (see SettingsList.h). Persisted by index: append only.
  enum SHORT_PWRBTN {
    IGNORE = 0,
    SLEEP = 1,
    PAGE_TURN = 2,
    FORCE_REFRESH = 3,
    FOOTNOTES = 4,
    PWR_CONFIRM = 5,
    CREATE_CLIPPING = 6,
    PWR_BOOKMARK = 7,
    PWR_SYNC = 8,
    PWR_READER_MENU = 9,
    SHORT_PWRBTN_COUNT
  };


  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };


  enum VIBRATION { VIBRATION_TOUCH = 0, VIBRATION_TOUCH_PAGE = 1, VIBRATION_OFF = 2 };
  uint8_t vibration = VIBRATION_OFF;
  enum HAPTIC_INTENSITY { HAPTIC_LOW = 0, HAPTIC_MEDIUM = 1, HAPTIC_HIGH = 2 };
  uint8_t hapticIntensity = HAPTIC_HIGH;


  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Night mode: inverted output polarity, applied to every activity per
  // render by ActivityManager. The sleep screen opts out itself.
  uint8_t screenInverted = 0;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // LEGACY, kept for migration only: quarter-hour UTC offset biased by 48
  // (48 = UTC+0). Superseded by clockTimezone; read once by
  // timezones::activeIndex() when clockTimezone is unset.
  uint8_t clockUtcOffsetQ = 48;
  // Clock display format: 0 = 24-hour, 1 = 12-hour
  uint8_t clockFormat = 0;
  // Index into the timezone table (src/core/util/Timezones.cpp, append-only).
  // 255 = never chosen; falls back to the legacy UTC offset, then UTC.
  // WaddleOS default: Oslo (index 19, CET/CEST).
  uint8_t clockTimezone = 19;
  // CLOCK_DST_MODE: follow the zone's DST rule, or force it on/off.
  uint8_t clockDst = CLOCK_DST_AUTO;
  // Show the clock opposite the battery in every header band that draws one.
  uint8_t clockShowInHeader = 0;
  // Short power button click behaviour
  uint8_t shortPwrBtn = IGNORE;
  // X4 Pro: double-click power toggles the frontlight. Disabling frees the
  // power button for shortPwrBtn actions without the double-click wait.
  uint8_t doubleClickPwrLight = 1;
  uint8_t homeButtonTapAction = static_cast<uint8_t>(HomeButtonAction::Home);
  uint8_t homeButtonDoubleTapAction = static_cast<uint8_t>(HomeButtonAction::ToggleFrontlight);
  uint8_t homeButtonLongPressAction = static_cast<uint8_t>(HomeButtonAction::ReaderMenu);
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonFollowOrientation = 0;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // UI Theme
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Frontlight quick-panel state. Category-less SettingsList entries persist
  // these without adding them to the regular Settings screen.
  uint8_t frontlightBrightness = 60;
  uint8_t frontlightWarmth = 50;  // 0 = cool .. 100 = warm
  uint8_t frontlightOn = 0;
  // Restore the saved on/off state after a normal boot or wake. Brightness and
  // warmth are always remembered even when this is disabled.
  uint8_t frontlightRestoreOnWake = 1;
  // What the slide switch controls (boards with one): frontlight on/off or
  // night mode. The switch's position is applied at boot and on each change.
  enum SLIDE_SWITCH_ACTION : uint8_t {
    SLIDE_SWITCH_FRONTLIGHT = 0,
    SLIDE_SWITCH_NIGHT_MODE = 1,
    SLIDE_SWITCH_TOUCH_CONTROLS = 2,
  };
  uint8_t slideSwitchAction = SLIDE_SWITCH_FRONTLIGHT;
  // Language setting (Language enum index, default 0 = EN)
  // WaddleOS default: Norsk bokmål.
  uint8_t language = static_cast<uint8_t>(Language::NB);
  // Bluetooth page turner, saved under the keys lib/BlePageTurner reads and writes (BlePageTurnerJson.h).
  bleturner::Config ble;
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.

  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t SLEEP_TIMEOUT_NEVER_MINUTES = 31;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = SLEEP_TIMEOUT_NEVER_MINUTES;

  uint16_t getPowerButtonDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? 10 : 400;
  }
  static const char* getFilePath() { return "/.crosspoint/settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);

  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
