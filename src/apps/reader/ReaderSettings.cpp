#include "ReaderSettings.h"

#include <BoardConfig.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <string>

#include "apps/reader/util/DictionaryRegistry.h"
#include "apps/reader/util/ParagraphIndentMigration.h"
#include "core/CrossPointSettings.h"
#include "core/ReaderFontSizes.h"
#include "core/SdCardFontSystem.h"
#include "core/SettingsList.h"
#include "core/fontIds.h"

namespace {

void copyToField(char* dest, const char* src, const size_t maxLen) {
  strncpy(dest, src, maxLen - 1);
  dest[maxLen - 1] = '\0';
}

// Build the font family setting dynamically. When registry is non-null, SD card fonts
// are appended after the built-in fonts. Otherwise only built-in fonts are listed.
SettingInfo buildFontFamilySetting(const SdCardFontRegistry* registry) {
  // Built-in font labels (StrId)
  std::vector<StrId> enumValues = {StrId::STR_NOTO_SANS};
  // Runtime string labels for SD card fonts
  std::vector<std::string> enumStringValues;

  // Reserve: first ReaderSettings::BUILTIN_FONT_COUNT entries use StrId, rest use strings
  if (registry) {
    const auto& families = registry->getFamilies();
    enumStringValues.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(enumStringValues),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  // Capture the SD font count for the lambdas
  const int sdFontCount = static_cast<int>(enumStringValues.size());

  // Total option count = built-in + SD card families
  // For the combined enumStringValues: we need all entries as strings (built-in names + SD names)
  // The render code checks enumStringValues first, then enumValues. So we build enumStringValues
  // with all options when SD fonts are present.
  std::vector<std::string> allStringValues;
  if (sdFontCount > 0) {
    allStringValues.push_back(I18N.get(StrId::STR_NOTO_SANS));
    allStringValues.insert(allStringValues.end(), enumStringValues.begin(), enumStringValues.end());
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_FAMILY;
  s.type = SettingType::ENUM;
  s.enumValues = std::move(enumValues);
  s.enumStringValues = std::move(allStringValues);
  s.key = "fontFamily";
  s.category = StrId::STR_CAT_READER;
  s.inTextSettings = true;  // matches the static font-family entry it replaces

  // Capture registry families by copy for the lambdas
  std::vector<std::string> sdFamilyNames;
  if (registry) {
    const auto& families = registry->getFamilies();
    sdFamilyNames.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilyNames),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  s.valueGetter = [sdFamilyNames]() -> uint8_t {
    // If an SD card font is selected, find its index
    if (READER_SETTINGS.sdFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(sdFamilyNames.size()); i++) {
        if (sdFamilyNames[i] == READER_SETTINGS.sdFontFamilyName) {
          return static_cast<uint8_t>(ReaderSettings::BUILTIN_FONT_COUNT + i);
        }
      }
      // SD font name not found in registry — fall through to built-in
    }
    return READER_SETTINGS.fontFamily < ReaderSettings::BUILTIN_FONT_COUNT ? READER_SETTINGS.fontFamily : 0;
  };

  s.valueSetter = [sdFamilyNames](uint8_t v) {
    if (v < ReaderSettings::BUILTIN_FONT_COUNT) {
      READER_SETTINGS.fontFamily = v;
      READER_SETTINGS.sdFontFamilyName[0] = '\0';
    } else {
      int sdIdx = v - ReaderSettings::BUILTIN_FONT_COUNT;
      if (sdIdx < static_cast<int>(sdFamilyNames.size())) {
        strncpy(READER_SETTINGS.sdFontFamilyName, sdFamilyNames[sdIdx].c_str(), sizeof(READER_SETTINGS.sdFontFamilyName) - 1);
        READER_SETTINGS.sdFontFamilyName[sizeof(READER_SETTINGS.sdFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

// Build the font size setting dynamically: the options are the point sizes the
// active family actually ships, so an SD family built at 10/12/14 offers three
// sizes and a family built at 8..18 offers six. The selected point size persists
// in READER_SETTINGS.fontPointSize (saved/loaded manually in ReaderSettings::
// toJson/fromJson — the generic loop skips dynamic entries), while the ENUM
// contract shared with the web UI stays index-based.
SettingInfo buildFontSizeSetting(const SdCardFontRegistry* registry) {
  // Captured by copy: getSettingsList() returns by value and the lambdas outlive
  // this call, so they must not reference the registry.
  const std::vector<uint8_t> sizes = readerFontPointSizes(registry, READER_SETTINGS.sdFontFamilyName);

  // "pt" is deliberately not translated — see the matching note in
  // TextSettingsActivity::rebuildSizeList().
  std::vector<std::string> labels;
  labels.reserve(sizes.size());
  for (const uint8_t pt : sizes) {
    labels.push_back(std::to_string(pt) + " pt");
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.enumStringValues = std::move(labels);
  s.key = "fontSize";
  s.category = StrId::STR_CAT_READER;
  s.inTextSettings = true;  // matches the static font-size entry it replaces

  s.valueGetter = [sizes]() -> uint8_t {
    const uint8_t pt = snapToNearestPointSize(sizes, READER_SETTINGS.fontPointSize);
    for (int i = 0; i < static_cast<int>(sizes.size()); i++) {
      if (sizes[i] == pt) return static_cast<uint8_t>(i);
    }
    return 0;
  };

  s.valueSetter = [sizes](uint8_t v) {
    if (v < sizes.size()) READER_SETTINGS.fontPointSize = sizes[v];
  };

  return s;
}

// Build the dictionary selection setting dynamically from the folders discovered
// under /dictionaries. "None" plus one option per dictionary; the selected folder
// name persists in READER_SETTINGS.dictionaryName (saved/loaded manually in
// ReaderSettings::toJson/fromJson — the generic loop skips dynamic entries).
SettingInfo buildDictionarySetting(const std::vector<DictionaryEntry>& dictionaries) {
  std::vector<std::string> folderNames;
  folderNames.reserve(dictionaries.size());
  std::transform(dictionaries.begin(), dictionaries.end(), std::back_inserter(folderNames),
                 [](const DictionaryEntry& d) { return d.name; });

  SettingInfo s;
  s.nameId = StrId::STR_DICTIONARY;
  s.key = "dictionaryName";  // web settings API; persisted by name, not by the generic loop
  s.type = SettingType::ENUM;
  s.enumStringValues.reserve(folderNames.size() + 1);
  s.enumStringValues.push_back(I18N.get(StrId::STR_NONE_OPT));
  s.enumStringValues.insert(s.enumStringValues.end(), folderNames.begin(), folderNames.end());
  s.category = StrId::STR_CAT_READER;

  s.valueGetter = [folderNames]() -> uint8_t {
    for (size_t i = 0; i < folderNames.size(); i++) {
      // Compare within the settings field capacity: an over-long folder name is
      // stored truncated, and must still match its list entry.
      if (strncmp(folderNames[i].c_str(), READER_SETTINGS.dictionaryName, sizeof(READER_SETTINGS.dictionaryName) - 1) == 0) {
        return static_cast<uint8_t>(i + 1);
      }
    }
    return 0;  // "None", also when the stored folder no longer exists
  };

  s.valueSetter = [folderNames](uint8_t v) {
    if (v == 0 || v > folderNames.size()) {
      READER_SETTINGS.dictionaryName[0] = '\0';
      return;
    }
    strncpy(READER_SETTINGS.dictionaryName, folderNames[v - 1].c_str(), sizeof(READER_SETTINGS.dictionaryName) - 1);
    READER_SETTINGS.dictionaryName[sizeof(READER_SETTINGS.dictionaryName) - 1] = '\0';
  };

  return s;
}

std::vector<StrId> buildLongPressMenuValues() {
  // No KOReader sync in WaddleOS: the list starts at LP_MENU_DISABLED.
  std::vector<StrId> values = {StrId::STR_DISABLED, StrId::STR_BOOKMARK_OPTION, StrId::STR_DICTIONARY};
  values.reserve(ReaderSettings::LONG_PRESS_MENU_FUNCTION_COUNT);
  if (BoardConfig::hasHomeKey()) values.push_back(StrId::STR_READER_MENU);
  values.push_back(StrId::STR_SAVE_CLIPPING);
  values.push_back(StrId::STR_FOOTNOTES);
  return values;
}

uint8_t longPressMenuDisplayValue() {
  // Display index = raw - 1 (no KOReader entry), minus one more after the
  // reader-menu slot on boards without a home key.
  const uint8_t raw = READER_SETTINGS.longPressMenuFunction;
  if (raw == ReaderSettings::LP_MENU_KOSYNC) return 0;
  if (raw <= ReaderSettings::LP_MENU_DICTIONARY) return raw - 1;
  if (raw == ReaderSettings::LP_MENU_READER_MENU) {
    return BoardConfig::hasHomeKey() ? ReaderSettings::LP_MENU_READER_MENU - 1 : 0;
  }
  if (raw < ReaderSettings::LONG_PRESS_MENU_FUNCTION_COUNT) return BoardConfig::hasHomeKey() ? raw - 1 : raw - 2;
  return 0;
}

void setLongPressMenuFromDisplayValue(const uint8_t displayValue) {
  if (displayValue < ReaderSettings::LP_MENU_DICTIONARY) {
    READER_SETTINGS.longPressMenuFunction = displayValue + 1;
  } else {
    READER_SETTINGS.longPressMenuFunction = BoardConfig::hasHomeKey() ? displayValue + 1 : displayValue + 2;
  }
}

SettingInfo buildLongPressMenuSetting() {
  return SettingInfo::DynamicEnum(StrId::STR_LONG_PRESS_MENU, buildLongPressMenuValues(), longPressMenuDisplayValue,
                                  setLongPressMenuFromDisplayValue, "longPressMenuFunction", StrId::STR_CAT_CONTROLS);
}

// The reader's rows. Load and save walk this list too, so every condition here
// must be fixed before settings load; runtime probes belong in adjustUiSettings().
void forEachSetting(SettingVisitor visit, void* context) {
  ReaderSettings& r = READER_SETTINGS;
  static constexpr auto STATUS_BAR_CLOCK_VALUES = [] {
    std::array<StrId, ReaderSettings::STATUS_BAR_CLOCK_MODE_COUNT> values{};
    values[ReaderSettings::STATUS_BAR_CLOCK_HIDE] = StrId::STR_HIDE;
    values[ReaderSettings::STATUS_BAR_CLOCK_RIGHT] = StrId::STR_DIR_RIGHT;
    values[ReaderSettings::STATUS_BAR_CLOCK_LEFT] = StrId::STR_DIR_LEFT;
    return values;
  }();

  const auto emit = [visit, context](auto&& info) { visit(context, info); };
  // --- Reader ---
  // Built-in font-family entry. Replaced per-call with a registry-aware
  // version when SD fonts are installed.
  emit(SettingInfo::Enum(StrId::STR_FONT_FAMILY, &r.fontFamily, {StrId::STR_NOTO_SANS}, "fontFamily",
                         StrId::STR_CAT_READER)
           .withTextSettings());
  // Placeholder: the selectable sizes depend on the active font family, so
  // adjustUiSettings() replaces this entry with buildFontSizeSetting(). It only
  // fixes the setting's position in the Reader category.
  emit(SettingInfo::Enum(StrId::STR_FONT_SIZE, nullptr, {}, "fontSize", StrId::STR_CAT_READER).withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_LINE_SPACING, &r.lineSpacing,
                         {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE}, "lineSpacing",
                         StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Value(StrId::STR_WORD_SPACING, &r.wordSpacing,
                          {ReaderSettings::WORD_SPACING_MIN, ReaderSettings::WORD_SPACING_MAX,
                           ReaderSettings::WORD_SPACING_STEP},
                          "wordSpacing", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_CHARACTER_SPACING, &r.characterSpacing,
                         {StrId::STR_SPACING_MINUS_2, StrId::STR_SPACING_MINUS_1, StrId::STR_SPACING_ZERO,
                          StrId::STR_SPACING_PLUS_1, StrId::STR_SPACING_PLUS_2},
                         "characterSpacing", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &r.screenMargin,
                          {ReaderSettings::SCREEN_MARGIN_MIN, ReaderSettings::SCREEN_MARGIN_MAX,
                           ReaderSettings::SCREEN_MARGIN_STEP},
                          "screenMargin", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &r.paragraphAlignment,
                         {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                          StrId::STR_BOOK_S_STYLE},
                         "paragraphAlignment", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &r.embeddedStyle, "embeddedStyle",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_FOCUS_READING, &r.focusReadingEnabled, "focusReadingEnabled",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_HYPHENATION, &r.hyphenationEnabled, "hyphenationEnabled",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(
      StrId::STR_ORIENTATION, &r.orientation,
      {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW},
      "orientation", StrId::STR_CAT_READER));
  emit(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &r.extraParagraphSpacing,
                           "extraParagraphSpacing", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Value(StrId::STR_PARAGRAPH_INDENTATION, &r.paragraphIndentSpaces, {0, 5, 1},
                          "paragraphIndentSpaces", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_TEXT_AA, &r.textAntiAliasing, "textAntiAliasing",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_IMAGES, &r.imageRendering,
                         {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                         "imageRendering", StrId::STR_CAT_READER));
  emit(SettingInfo::Enum(StrId::STR_READER_MENU_STYLE, &r.readerMenuStyle,
                         {StrId::STR_MENU_STYLE_LIST, StrId::STR_MENU_STYLE_TOOLBAR}, "readerMenuStyle",
                         StrId::STR_CAT_READER));
  // The reader menu style stays available on button boards (the toolbar
  // chrome is button-navigable); only the touch controls are hidden.
  if (BoardConfig::hasTouch()) {
    emit(SettingInfo::Toggle(StrId::STR_TOUCH_READER_CONTROLS, &r.touchReaderControls,
                             "touchReaderControls", StrId::STR_CAT_CONTROLS));
    emit(SettingInfo::Enum(StrId::STR_NEXT_PAGE_GESTURE, &r.pageTurnGesture,
                           {StrId::STR_TAP_AND_SWIPE, StrId::STR_TAP_ONLY, StrId::STR_SWIPE_ONLY,
                            StrId::STR_INVERTED_TAP, StrId::STR_DISABLED},
                           "pageTurnGesture", StrId::STR_CAT_CONTROLS));
    emit(SettingInfo::Enum(StrId::STR_PREV_PAGE_GESTURE, &r.previousPageGesture,
                           {StrId::STR_TAP_AND_SWIPE, StrId::STR_TAP_ONLY, StrId::STR_SWIPE_ONLY,
                            StrId::STR_INVERTED_TAP, StrId::STR_DISABLED},
                           "previousPageGesture", StrId::STR_CAT_CONTROLS));
  }
  // The reader-menu gesture choice only makes sense where the menu stays
  // reachable without the tap and the bottom edge is free (the capacitive
  // Home key); everywhere else the bottom-edge up-swipe is Home and the
  // center tap is the primary path, so the setting stays at its Tap default.
  // Persisted under the legacy "tapForReaderMenu" key: old saves map
  // 0 = Off, 1 = Tap.
  if (BoardConfig::hasHomeKey())
    emit(SettingInfo::Enum(StrId::STR_SHOW_READER_MENU, &r.showReaderMenu,
                           {StrId::STR_STATE_OFF, StrId::STR_STATE_TAP, StrId::STR_STATE_SWIPE_UP}, "tapForReaderMenu",
                           StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Enum(StrId::STR_LONG_PRESS_BEHAVIOR, &r.longPressButtonBehavior,
                         {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                          StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                         "longPressButtonBehavior", StrId::STR_CAT_CONTROLS));
  emit(buildLongPressMenuSetting());
  // Always persisted; getSettingsList() hides it when the IMU probe fails.
  emit(SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN, &r.tiltPageTurn,
                         {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED}, "tiltPageTurn",
                         StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Toggle(StrId::STR_PWR_BTN_FOOTNOTE_BACK, &r.pwrBtnFootnoteBack,
                           "pwrBtnFootnoteBack", StrId::STR_CAT_CONTROLS));
  if (!BoardConfig::hasTouch())
    emit(SettingInfo::Toggle(StrId::STR_BACK_SHORT_TO_FILE_BROWSER, &r.backShortToFileBrowser,
                             "backShortToFileBrowser", StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &r.showHiddenFiles, "showHiddenFiles",
                           StrId::STR_CAT_SYSTEM));
  emit(SettingInfo::Toggle(StrId::STR_LIBRARY_USE_METADATA, &r.libraryUseMetadata,
                           "libraryUseMetadata", StrId::STR_CAT_SYSTEM));
  emit(SettingInfo::Toggle(StrId::STR_REMOVE_READ_FROM_RECENTS, &r.removeReadBooksFromRecents,
                           "removeReadBooksFromRecents", StrId::STR_CAT_SYSTEM));
  emit(SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &r.moveFinishedToReadFolder,
                           "moveFinishedToReadFolder", StrId::STR_CAT_SYSTEM));
  // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
  emit(SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &r.statusBarChapterPageCount,
                           "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &r.statusBarBookProgressPercentage,
                           "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &r.statusBarProgressBar,
                         {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &r.statusBarProgressBarThickness,
                         {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                         "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_TITLE, &r.statusBarTitle,
                         {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_BATTERY, &r.statusBarBattery, "statusBarBattery",
                           StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &r.xtcStatusBarMode,
                         {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP}, "xtcStatusBarMode",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::StaticEnum(StrId::STR_CLOCK, &r.statusBarClock, STATUS_BAR_CLOCK_VALUES,
                               "statusBarClock", StrId::STR_CUSTOMISE_STATUS_BAR));
}

void toJson(JsonDocument& doc) {
  const ReaderSettings& r = READER_SETTINGS;
  // Font family and size — both use dynamic getter/setters (the option lists
  // depend on the SD font registry), so the generic loop skips them.
  doc["fontFamily"] = ReaderSettings::STORED_NOTOSANS;
  doc["fontSize"] = r.fontPointSize;
  // SD card font family name — not a row, save manually
  if (r.sdFontFamilyName[0] != '\0') {
    doc["sdFontFamilyName"] = r.sdFontFamilyName;
  }
  // Dictionary folder name — uses a dynamic getter/setter, save manually
  if (r.dictionaryName[0] != '\0') {
    doc["dictionaryName"] = r.dictionaryName;
  }
  // The displayed option list varies by board, so this setting uses a dynamic
  // display-index mapping and is skipped by the generic persistence loop.
  doc["longPressMenuFunction"] = r.longPressMenuFunction;
}

bool fromJson(JsonVariantConst doc) {
  ReaderSettings& r = READER_SETTINGS;
  bool needsResave = false;
  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  const auto indentSpaces = doc["paragraphIndentSpaces"];
  const bool hasSavedWidth = indentSpaces.is<int>();
  const int savedWidth = hasSavedWidth ? indentSpaces.as<int>() : 0;
  r.paragraphIndentSpaces = migrateParagraphIndentSpaces(hasSavedWidth, savedWidth, r.extraParagraphSpacing != 0);
  if (!hasSavedWidth || savedWidth < 0 || savedWidth > 5) needsResave = true;

  // Older files stored one combined touch mode under "touchReaderControls":
  // 0=off, 1=tap, 2=swipe, 3=inverted tap. Split it into the master toggle
  // plus the per-direction gesture pair (the generic loop already folded
  // out-of-range toggle values back to the On default).
  if (doc["pageTurnGesture"].isNull() && doc["previousPageGesture"].isNull() &&
      doc["touchReaderControls"].is<uint8_t>()) {
    const uint8_t mode = doc["touchReaderControls"].as<uint8_t>();
    if (mode >= 1 && mode <= 3) {
      r.touchReaderControls = ReaderSettings::TOUCH_READER_ON;
      r.pageTurnGesture = mode == 1   ? ReaderSettings::TAP_ONLY
                          : mode == 2 ? ReaderSettings::SWIPE_ONLY
                                      : ReaderSettings::INVERTED_TAP;
      r.previousPageGesture = r.pageTurnGesture;
      needsResave = true;
    }
  }

  // Reader font size — an actual point size since 1.5. Files written by 1.4 and
  // earlier hold the old SMALL/MEDIUM/LARGE/EXTRA_LARGE slot in 0..3; no font is
  // renderable at those sizes, so the range is unambiguous and folds to the
  // point sizes those slots used to mean. Drop this once 1.4 upgrades are done.
  uint8_t storedFontSize = doc["fontSize"] | ReaderSettings::DEFAULT_FONT_POINT_SIZE;
  if (storedFontSize <= ReaderSettings::LEGACY_FONT_SIZE_MAX) {
    storedFontSize = 12 + storedFontSize * 2;  // 0,1,2,3 -> 12,14,16,18
    needsResave = true;
  }
  r.fontPointSize = storedFontSize;

  // Font family — uses a dynamic getter/setter so the generic loop skips it.
  const uint8_t storedFontFamily = doc["fontFamily"] | ReaderSettings::STORED_NOTOSANS;
  r.fontFamily = ReaderSettings::NOTOSANS;
  r.longPressMenuFunction =
      clamp(doc["longPressMenuFunction"] | (uint8_t)ReaderSettings::LP_MENU_DISABLED,
            (uint8_t)ReaderSettings::LONG_PRESS_MENU_FUNCTION_COUNT, (uint8_t)ReaderSettings::LP_MENU_DISABLED);

  // SD card font family name — not a row, load manually
  copyToField(r.sdFontFamilyName, doc["sdFontFamilyName"] | "", sizeof(r.sdFontFamilyName));
  if (r.sdFontFamilyName[0] == '\0') {
    switch (storedFontFamily) {
      case ReaderSettings::LEGACY_NOTOSERIF:
        copyToField(r.sdFontFamilyName, "NotoSerif", sizeof(r.sdFontFamilyName));
        break;
      case ReaderSettings::LEGACY_OPENDYSLEXIC:
        copyToField(r.sdFontFamilyName, "OpenDyslexic", sizeof(r.sdFontFamilyName));
        break;
    }
  }
  if (storedFontFamily != ReaderSettings::STORED_NOTOSANS) needsResave = true;
  // Dictionary folder name — uses a dynamic getter/setter, load manually
  copyToField(r.dictionaryName, doc["dictionaryName"] | "", sizeof(r.dictionaryName));
  return needsResave;
}

void adjustUiSettings(std::vector<SettingInfo>& v) {
  // Tilt page turn needs an IMU with a gyroscope. UI-only: a failed probe on
  // one boot must not drop the saved value.
  if (!halTiltSensor.isAvailable()) {
    v.erase(
        std::remove_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_TILT_PAGE_TURN; }),
        v.end());
  }
  // Hidden on touch boards: the Home button actions (X4 Pro) or the short
  // power click (other touch boards) cover these shortcuts.
  if (BoardConfig::hasTouch()) {
    v.erase(
        std::remove_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_LONG_PRESS_MENU; }),
        v.end());
  }
  const SdCardFontRegistry* registry = &sdFontSystem.registry();
  if (registry->getFamilyCount() > 0) {
    auto it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_FAMILY; });
    if (it != v.end()) {
      *it = buildFontFamilySetting(registry);
    }
  }
  {
    // Unconditional: even with no SD fonts installed the sizes come from the
    // built-in family rather than a fixed Small/Medium/Large/XL enum.
    auto it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_SIZE; });
    if (it != v.end()) {
      *it = buildFontSizeSetting(registry);
    }
  }
  // Rescanned on every call: cheap (one directory listing) and picks up
  // dictionaries copied to the SD card since the last visit.
  std::vector<DictionaryEntry> dictionaries;
  DictionaryRegistry::discover(dictionaries);
  if (!dictionaries.empty()) {
    // At the end of the Reader category.
    auto last = std::find_if(v.rbegin(), v.rend(),
                             [](const SettingInfo& s) { return s.category == StrId::STR_CAT_READER; });
    v.insert(last.base(), buildDictionarySetting(dictionaries));
  }
}

}  // namespace

const SettingsModule& ReaderSettings::settingsModule() {
  static const SettingsModule module = {forEachSetting, toJson, fromJson, adjustUiSettings};
  return module;
}

ReaderSettings::StatusBarSpec ReaderSettings::statusBarSpec() const {
  StatusBarSpec spec;
  spec.showChapterPageCount = statusBarChapterPageCount != 0;
  spec.showBookProgressPercent = statusBarBookProgressPercentage != 0;
  spec.titleMode = statusBarTitle;
  spec.showBattery = statusBarBattery != 0;
  spec.showBatteryPercent = SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_NEVER;
  spec.clockMode = statusBarClock;
  spec.clock12h = SETTINGS.clockFormat == 1;
  spec.progressBarMode = statusBarProgressBar;
  spec.progressBarHeightPx =
      statusBarProgressBar != HIDE_PROGRESS ? static_cast<uint8_t>((statusBarProgressBarThickness + 1) * 2) : 0;
  spec.xtcMode = xtcStatusBarMode;
  return spec;
}

ReaderRenderSpec ReaderSettings::readerRenderSpec(const uint16_t viewportWidth,
                                                      const uint16_t viewportHeight) const {
  ReaderRenderSpec spec;
  spec.fontId = getReaderFontId();
  spec.lineCompression = getReaderLineCompression();
  spec.characterSpacing = getCharacterSpacing();
  spec.wordSpacingPercent = wordSpacing;
  spec.extraParagraphSpacing = extraParagraphSpacing != 0;
  spec.paragraphIndentSpaces = paragraphIndentSpaces;
  spec.paragraphAlignment = paragraphAlignment;
  spec.viewportWidth = viewportWidth;
  spec.viewportHeight = viewportHeight;
  spec.hyphenationEnabled = hyphenationEnabled != 0;
  spec.embeddedStyle = embeddedStyle != 0;
  spec.imageRendering = imageRendering;
  spec.focusReadingEnabled = focusReadingEnabled != 0;
  return spec;
}

float ReaderSettings::getReaderLineCompression() const {
  // One scale for all SD card fonts, whatever their format: .cpfont and vector
  // fonts both take their line height from FreeType's size metrics. Same values
  // as the former built-in Noto Serif; Normal keeps the font's own line height.
  if (sdFontFamilyName[0] != '\0') {
    switch (lineSpacing) {
      case TIGHT:
        return 0.95f;
      case NORMAL:
      default:
        return 1.0f;
      case WIDE:
        return 1.1f;
      case EXTRA_WIDE:
        return 1.2f;
    }
  }

  switch (lineSpacing) {
    case TIGHT:
      return 0.90f;
    case NORMAL:
    default:
      return 0.95f;
    case WIDE:
      return 1.0f;
    case EXTRA_WIDE:
      return 1.05f;
  }
}

void ReaderSettings::clearSdFontFamily() {
  sdFontFamilyName[0] = '\0';
  fontPointSize =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), fontPointSize);
  SETTINGS.saveToFile();
}

int ReaderSettings::getReaderFontId() const {
  // Check SD card font first
  if (sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, fontPointSize);
    if (id != 0) return id;
    // Fall through to built-in if SD font not found
  }

  // A built-in family only exists at BUILTIN_READER_POINT_SIZES, so a size
  // carried over from an SD family may not be one of them. ensureLoaded()
  // normally persists the snap; snap again here (without allocating — this runs
  // in the page render loop) so rendering is correct even before it has run.
  const uint8_t pt =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), fontPointSize);
  switch (pt) {
    case 12:
      return NOTOSANS_12_FONT_ID;
    case 16:
      return NOTOSANS_16_FONT_ID;
    case 18:
      return NOTOSANS_18_FONT_ID;
    case 14:
    default:
      return NOTOSANS_14_FONT_ID;
  }
}
