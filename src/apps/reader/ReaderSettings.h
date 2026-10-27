#pragma once

#include <ArduinoJson.h>
#include <Epub/ReaderRenderSpec.h>

#include <cstdint>
#include <vector>

#include "core/SettingsModule.h"

// The reader's settings: fonts and text, status bar, page turning, library.
// Saved in the same settings.json as the core settings and under the same keys
// as before; CrossPointSettings loads and saves them through settingsModule().
class ReaderSettings {
 public:
  static ReaderSettings& getInstance() {
    static ReaderSettings instance;
    return instance;
  }

  // Rows, saving and loading for settings.json (see core/SettingsModule.h).
  static const SettingsModule& settingsModule();

  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum XTC_STATUS_BAR_MODE {
    XTC_STATUS_BAR_HIDE = 0,
    XTC_STATUS_BAR_BOTTOM = 1,
    XTC_STATUS_BAR_TOP = 2,
    XTC_STATUS_BAR_MODE_COUNT
  };

  enum STATUS_BAR_CLOCK_MODE {
    STATUS_BAR_CLOCK_HIDE = 0,
    STATUS_BAR_CLOCK_RIGHT = 1,
    STATUS_BAR_CLOCK_LEFT = 2,
    STATUS_BAR_CLOCK_MODE_COUNT
  };
  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };
  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName)
  enum FONT_FAMILY { NOTOSANS = 0, FONT_FAMILY_COUNT };
  static constexpr uint8_t LEGACY_NOTOSERIF = 0;
  // Persisted IDs stay stable even though the built-in menu now starts at zero.
  static constexpr uint8_t STORED_NOTOSANS = 1;
  static constexpr uint8_t LEGACY_OPENDYSLEXIC = 2;
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Reader font size is a point size, not an enum slot — see fontPointSize.
  // Legacy 1.4-and-earlier files stored a 0..3 SMALL/MEDIUM/LARGE/EXTRA_LARGE
  // slot; fromJson() folds that range up (see LEGACY_FONT_SIZE_MAX).
  static constexpr uint8_t LEGACY_FONT_SIZE_MAX = 3;
  static constexpr uint8_t DEFAULT_FONT_POINT_SIZE = 14;
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, EXTRA_WIDE = 3, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };
  // Long-press Confirm action while reading an EPUB. Persisted values are stable;
  // SettingsList maps them to the device-specific option list shown to the user.
  enum LONG_PRESS_MENU_FUNCTION {
    LP_MENU_KOSYNC = 0,
    LP_MENU_DISABLED = 1,
    LP_MENU_BOOKMARK = 2,
    LP_MENU_DICTIONARY = 3,
    LP_MENU_READER_MENU = 4,
    LP_MENU_CREATE_CLIPPING = 5,
    LP_MENU_FOOTNOTES = 6,
    LONG_PRESS_MENU_FUNCTION_COUNT
  };
  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // UI Theme

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };
  // How Select opens the reader menu: the classic full-screen list, or a toolbar
  // overlay (top/bottom bars with Contents / Text / More bottom-sheet panels)
  // painted over the page.
  enum READER_MENU_STYLE { READER_MENU_LIST = 0, READER_MENU_TOOLBAR = 1, READER_MENU_STYLE_COUNT };

  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_NORMAL = 1, TILT_NVERTED = 2, TILT_PAGE_TURN_COUNT };
  enum TOUCH_READER_CONTROLS { TOUCH_READER_OFF = 0, TOUCH_READER_ON = 1, TOUCH_READER_CONTROLS_COUNT };

  // Per-direction reader page-turn gestures. INVERTED_TAP is tap-only; either
  // direction set to it swaps both directions' shared tap zones (see
  // ReaderUtils::detectTouchPageTurn).
  enum PAGE_TURN_GESTURE {
    TAP_AND_SWIPE = 0,
    TAP_ONLY = 1,
    SWIPE_ONLY = 2,
    INVERTED_TAP = 3,
    PAGE_TURN_GESTURE_DISABLED = 4,
    PAGE_TURN_GESTURE_COUNT
  };

  // How the reader menu opens on touch boards. Persisted under the legacy
  // "tapForReaderMenu" key: 0/1 keep their old Off/Tap meaning.
  enum SHOW_READER_MENU { READER_MENU_OFF = 0, READER_MENU_TAP = 1, READER_MENU_SWIPE_UP = 2, SHOW_READER_MENU_COUNT };
  // Status bar settings
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  // Clock display in status bar (any board whose RTC probe succeeds)
  uint8_t statusBarClock = STATUS_BAR_CLOCK_HIDE;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t paragraphIndentSpaces = 2;
  static constexpr uint8_t WORD_SPACING_MIN = 50;
  static constexpr uint8_t WORD_SPACING_MAX = 200;
  static constexpr uint8_t WORD_SPACING_STEP = 25;
  uint8_t wordSpacing = 100;                              // percent of the font's space advance
  static constexpr uint8_t CHARACTER_SPACING_OFFSET = 2;  // stored 0..4 maps to -2..+2 px
  uint8_t characterSpacing = CHARACTER_SPACING_OFFSET;
  int8_t getCharacterSpacing() const { return static_cast<int8_t>(characterSpacing - CHARACTER_SPACING_OFFSET); }
  uint8_t textAntiAliasing = 1;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Reader font settings
  uint8_t fontFamily = NOTOSANS;
  // Point size of the reader font. Only sizes the active family actually ships
  // are selectable; SdCardFontSystem::ensureLoaded() snaps this to the nearest
  // available size (and persists the snap) whenever the family changes.
  uint8_t fontPointSize = DEFAULT_FONT_POINT_SIZE;
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = JUSTIFIED;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  static constexpr uint8_t SCREEN_MARGIN_MIN = 5;
  static constexpr uint8_t SCREEN_MARGIN_MAX = 40;
  static constexpr uint8_t SCREEN_MARGIN_STEP = 5;
  uint8_t screenMargin = SCREEN_MARGIN_MIN;
  // Long-press page turn button behavior
  uint8_t longPressButtonBehavior = OFF;
  // Long-press Confirm function in EPUB reader (cycles through LONG_PRESS_MENU_FUNCTION values).
  // Defaults to Disabled so shortcut-based bookmark toggling remains opt-in.
  uint8_t longPressMenuFunction = LP_MENU_DISABLED;
  // Power button return from footnotes (1 = enabled, 0 = disabled)
  uint8_t pwrBtnFootnoteBack = 1;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Focus Reading - emphasizes the first part of words with bold
  uint8_t focusReadingEnabled = 0;
  uint8_t readerMenuStyle = READER_MENU_LIST;
  // SD card font family name (empty = use built-in fontFamily)
  char sdFontFamilyName[32] = "";
  // Dictionary folder name under /dictionaries (empty = no dictionary)
  char dictionaryName[32] = "";
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Show the title and author read from inside each book rather than its
  // filename. Users can disable this to make index rebuilds skip EPUB parsing.
  uint8_t libraryUseMetadata = 1;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;
  // Short press Back goes to file browser instead of home (0 = disabled, 1 = enabled)
  uint8_t backShortToFileBrowser = 0;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = TILT_OFF;
  // Master reader-touch toggle on boards with a touch controller.
  uint8_t touchReaderControls = TOUCH_READER_ON;
  // Which gestures turn the page in each direction (PAGE_TURN_GESTURE).
  uint8_t pageTurnGesture = SWIPE_ONLY;
  uint8_t previousPageGesture = SWIPE_ONLY;
  // Reader menu open gesture (SHOW_READER_MENU: off / center tap / bottom-edge
  // up-swipe). Only surfaced on home-key boards, where Home is the capacitive
  // key and the bottom edge is free; elsewhere it stays at the Tap default.
  uint8_t showReaderMenu = READER_MENU_TAP;
  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t fontSize);
  SdFontIdResolver sdFontIdResolver = nullptr;
  void* sdFontResolverCtx = nullptr;

  int getReaderFontId() const;

  // Drop the SD font selection and fall back to the built-in family. The reader
  // point size comes back into BUILTIN_READER_POINT_SIZES with it, since that is
  // the only set a built-in family ships — otherwise the settings UI would keep
  // offering a size nothing renders at. Both fields are persisted in one write.
  void clearSdFontFamily();

  // Resolved status-bar composition. Consumers read the spec; only settings
  // editors read the raw fields.
  //
  // Deliberately NOT built under storeMutex: every field it reads is a single
  // byte, so a concurrent settings write can never produce a corrupt value —
  // only a snapshot mixing pre- and post-change fields. That costs at most one
  // e-ink frame drawn with a mixed status bar, which self-corrects on the next
  // refresh. Locking here would instead put a mutex on the render path and
  // stall it behind the SD write inside saveToFile(). Don't add one back.
  struct StatusBarSpec {
    bool showChapterPageCount = false;
    bool showBookProgressPercent = false;
    uint8_t titleMode = HIDE_TITLE;  // STATUS_BAR_TITLE
    bool showBattery = false;
    bool showBatteryPercent = false;
    uint8_t clockMode = STATUS_BAR_CLOCK_HIDE;  // STATUS_BAR_CLOCK_MODE
    bool clock12h = false;
    uint8_t progressBarMode = HIDE_PROGRESS;  // STATUS_BAR_PROGRESS_BAR
    uint8_t progressBarHeightPx = 0;          // (thickness+1)*2; 0 when the bar is hidden
    uint8_t xtcMode = XTC_STATUS_BAR_HIDE;    // XTC_STATUS_BAR_MODE

    bool showsProgressBar() const { return progressBarMode != HIDE_PROGRESS; }
    bool showsTitle() const { return titleMode != HIDE_TITLE; }
    bool showsClock() const { return clockMode != STATUS_BAR_CLOCK_HIDE; }
    // Visibility of the text lane. Clock hardware presence is the caller's
    // concern: pass halClock.isAvailable(), or true for layout reservation.
    bool textLaneVisible(bool clockAvailable) const {
      return showChapterPageCount || showBookProgressPercent || showsTitle() || showBattery ||
             (showsClock() && clockAvailable);
    }
  };
  StatusBarSpec statusBarSpec() const;

  // Resolved text-rendering configuration for the Epub layout engine. The
  // viewport is renderer/orientation-derived, so the caller supplies it —
  // passing it in keeps a spec from ever existing in a half-filled state.
  // Unlocked for the same reason as statusBarSpec(); see the note above.
  ReaderRenderSpec readerRenderSpec(uint16_t viewportWidth, uint16_t viewportHeight) const;

  float getReaderLineCompression() const;

 private:
  ReaderSettings() = default;
};

#define READER_SETTINGS ReaderSettings::getInstance()
