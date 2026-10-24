#include "WaddleHomeActivity.h"

#include <ArduinoJson.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdlib>

#include "MappedInputManager.h"
#include "NewsActivity.h"
#include "NodeActivity.h"
#include "WaddleDraw.h"
#include "WaddleWeatherIcons.h"
#include "WaddleWifi.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookProgress.h"

#ifndef WADDLE_OWNER_NAME
#define WADDLE_OWNER_NAME "Arne"
#endif

namespace {

constexpr const char* WEATHER_URL = "https://arnee.me/api/hjem.json";
constexpr const char* WEATHER_PATH = "/.waddle/hjem.json";
constexpr const char* FETCHED_PATH = "/.waddle/hjem-hentet.txt";
constexpr long STALE_MINUTES = 30;
// Fetch only after the screen has been up this long, so passing through Hjem
// on the way somewhere else does not turn on Wi-Fi.
constexpr unsigned long FETCH_DELAY_MS = 2000;
// After a failed fetch, wait this long before trying again.
constexpr unsigned long RETRY_MS = 10 * 60 * 1000;
constexpr unsigned long WIFI_TIMEOUT_MS = 15000;
// A half refresh this often clears e-ink ghosting from the clock updates.
constexpr int CLEAN_EVERY_RENDERS = 30;
constexpr int COVER_W = 100;
constexpr int COVER_H = 150;

constexpr const char* WEEKDAYS[] = {"Søndag", "Mandag", "Tirsdag", "Onsdag", "Torsdag", "Fredag", "Lørdag"};
constexpr const char* MONTHS[] = {"januar", "februar", "mars",     "april",   "mai",      "juni",
                                  "juli",   "august",  "september", "oktober", "november", "desember"};

// Survives leaving and re-entering Hjem, so a failed fetch is not retried on every visit.
unsigned long lastAttemptAt = 0;
bool attempted = false;

// Days since 1970-01-01 for a calendar date (Howard Hinnant's days_from_civil).
long daysFromCivil(int y, const unsigned m, const unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long>(doe) - 719468;
}

// Local wall-clock minutes since 1970, or -1 when the clock is not set.
long nowMinutes() {
  struct tm t {};
  if (!halClock.localTime(t) || t.tm_year < 120) return -1;
  return daysFromCivil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday) * 1440 + t.tm_hour * 60 + t.tm_min;
}

bool syncWeather() {
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("HJEM", "low heap for download (%u free)", ESP.getFreeHeap());
    return false;
  }
  Storage.mkdir("/.waddle");
  return HttpDownloader::downloadToFile(WEATHER_URL, WEATHER_PATH) == HttpDownloader::OK;
}

void loadWeather(WaddleHomeActivity::Weather& weather) {
  weather = {};
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(WEATHER_PATH, doc)) return;
  weather.place = doc["sted"] | "";
  weather.icon = doc["ikon"] | "sky";
  weather.text = doc["tekst"] | "";
  weather.temp = doc["temp"] | 0;
  weather.min = doc["min"] | 0;
  weather.max = doc["max"] | 0;
  weather.valid = !doc["temp"].isNull();
}

const char* const* weatherIcon(const std::string& name) {
  if (name == "sol") return waddle::WEATHER_SOL;
  if (name == "lettsky") return waddle::WEATHER_LETTSKY;
  if (name == "regn") return waddle::WEATHER_REGN;
  if (name == "sno") return waddle::WEATHER_SNO;
  if (name == "torden") return waddle::WEATHER_TORDEN;
  if (name == "taake") return waddle::WEATHER_TAAKE;
  return waddle::WEATHER_SKY;
}

void drawWeatherIcon(const GfxRenderer& renderer, const std::string& name, const int x, const int y, const int scale) {
  const char* const* rows = weatherIcon(name);
  for (int row = 0; row < waddle::WEATHER_ICON_SIZE; row++) {
    for (int col = 0; col < waddle::WEATHER_ICON_SIZE; col++) {
      if (rows[row][col] == '#') renderer.fillRect(x + col * scale, y + row * scale, scale, scale);
    }
  }
}

void drawFocus(const GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  renderer.drawRoundedRect(x, y, w, h, 3, 10, true);
}

}  // namespace

WaddleHomeActivity::WaddleHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const bool cleanInitialRefresh)
    : Activity("Home", renderer, mappedInput), cleanInitialRefresh(cleanInitialRefresh) {}

void WaddleHomeActivity::onEnter() {
  Activity::onEnter();
  loadData();
  enteredAt = millis();
  requestUpdate();
}

void WaddleHomeActivity::onExit() {
  if (fetching) waddle::wifi::off();
  Activity::onExit();
}

void WaddleHomeActivity::loadData() {
  // Book being read: the most recent one that is still on the card.
  RecentBook latest;
  bool found = false;
  for (const auto& recent : RECENT_BOOKS.getBooks()) {
    if (Storage.exists(recent.path.c_str())) {
      latest = recent;
      found = true;
      break;
    }
  }
  const int progress = found ? loadBookProgress(latest.path) : -1;

  Weather loadedWeather;
  loadWeather(loadedWeather);
  long loadedFetchedAt = -1;
  if (Storage.exists(FETCHED_PATH)) {
    const String text = Storage.readFile(FETCHED_PATH);
    if (text.length() > 0) loadedFetchedAt = atol(text.c_str());
  }

  waddle::news::Paper loadedPaper;
  waddle::news::load(loadedPaper);
  auto ids = waddle::news::loadReadIds();
  std::vector<int> top;
  int unreadCount = 0;
  for (size_t i = 0; i < loadedPaper.stories.size(); i++) {
    if (waddle::news::isRead(ids, loadedPaper.stories[i].id)) continue;
    unreadCount++;
    if (static_cast<int>(top.size()) < TOP_STORIES) top.push_back(static_cast<int>(i));
  }

  RenderLock lock(*this);
  hasBook = found;
  book = std::move(latest);
  bookProgress = progress;
  weather = std::move(loadedWeather);
  fetchedAt = loadedFetchedAt;
  paper = std::move(loadedPaper);
  readIds = std::move(ids);
  topStories = std::move(top);
  unread = unreadCount;
  if (!available(focus)) focus = T_BOOK;
}

bool WaddleHomeActivity::isStale() const {
  const long now = nowMinutes();
  return fetchedAt < 0 || now < 0 || now < fetchedAt || now - fetchedAt >= STALE_MINUTES;
}

void WaddleHomeActivity::startFetch() {
  attempted = true;
  lastAttemptAt = millis();
  fetching = true;
  connectStartedAt = millis();
  if (WiFi.status() != WL_CONNECTED && !waddle::wifi::joinSaved(*this)) finishFetch();
}

void WaddleHomeActivity::finishFetch() {
  bool ok = false;
  if (WiFi.status() == WL_CONNECTED) {
    const bool weatherOk = syncWeather();
    const bool newsOk = waddle::news::sync();
    ok = weatherOk || newsOk;
  }
  waddle::wifi::off();
  fetching = false;
  if (ok) {
    const long now = nowMinutes();
    if (now >= 0) Storage.writeFile(FETCHED_PATH, String(now));
    loadData();
    renders = 0;  // the next render is a clean one
  }
  requestUpdate();
}

// Generates the cover thumbnail of the book being read when it is missing.
// Runs after the first render, so the screen shows up first.
void WaddleHomeActivity::ensureCover() {
  coverTried = true;
  if (!hasBook || book.coverBmpPath.empty() || !FsHelpers::hasReflowableBookExtension(book.path)) return;
  const std::string thumb = UITheme::getCoverThumbPath(book.coverBmpPath, COVER_H);
  if (Storage.exists(thumb.c_str())) return;
  Epub epub(book.path, "/.crosspoint");
  if (epub.load(false, true) && epub.generateThumbBmp(COVER_H)) requestUpdate();
}

bool WaddleHomeActivity::available(const int target) const {
  if (target >= T_STORY_1 && target <= T_STORY_3) return target - T_STORY_1 < static_cast<int>(topStories.size());
  return target >= 0 && target < T_COUNT;
}

void WaddleHomeActivity::moveFocus(const int step) {
  int next = focus;
  do {
    next = (next + step + T_COUNT) % T_COUNT;
  } while (!available(next));
  focus = next;
  requestUpdate();
}

void WaddleHomeActivity::activate(const int target) {
  switch (target) {
    case T_BOOK:
      if (hasBook) {
        activityManager.goToReader(book.path);
      } else {
        activityManager.goToLibrary();
      }
      return;
    case T_STORY_1:
    case T_STORY_2:
    case T_STORY_3:
      if (target - T_STORY_1 < static_cast<int>(topStories.size())) {
        waddle::news::openStory(paper, readIds, topStories[target - T_STORY_1]);
      }
      return;
    case T_NEWS_LIST:
      activityManager.replaceActivity(std::make_unique<NewsActivity>(renderer, mappedInput, isStale()));
      return;
    case T_LIBRARY:
      activityManager.goToLibrary();
      return;
    case T_FILES:
      activityManager.goToFileBrowser();
      return;
    case T_TRANSFER:
      activityManager.goToFileTransfer();
      return;
    case T_BLUESOUND:
      activityManager.replaceActivity(std::make_unique<NodeActivity>(renderer, mappedInput));
      return;
    case T_SETTINGS:
      activityManager.goToSettings();
      return;
    default:
      return;
  }
}

void WaddleHomeActivity::loop() {
  using Button = MappedInputManager::Button;

  if (fetching) {
    if (WiFi.status() == WL_CONNECTED || millis() - connectStartedAt > WIFI_TIMEOUT_MS) finishFetch();
  } else if (millis() - enteredAt > FETCH_DELAY_MS && isStale() &&
             (!attempted || millis() - lastAttemptAt > RETRY_MS)) {
    startFetch();
  }

  if (!firstRender && !coverTried) ensureCover();

  if (mappedInput.wasReleased(Button::Confirm)) {
    activate(focus);
  } else if (mappedInput.wasReleased(Button::Down) || mappedInput.wasReleased(Button::Right) ||
             mappedInput.wasReleased(Button::PageForward)) {
    moveFocus(1);
  } else if (mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::Left) ||
             mappedInput.wasReleased(Button::PageBack)) {
    moveFocus(-1);
  } else {
    int x = 0, y = 0;
    if (mappedInput.wasScreenTapped(x, y)) {
      for (int target = 0; target < T_COUNT; target++) {
        if (available(target) && boxes[target].contains(x, y)) {
          activate(target);
          return;
        }
      }
    }
  }

  // Redraw when the minute changes, for the clock.
  uint8_t hour = 0, minute = 0;
  if (halClock.getTime(hour, minute) && minute != shownMinute) requestUpdate();
}

void WaddleHomeActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int side = 20;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);

  renderer.clearScreen();
  for (auto& box : boxes) box = {};

  // Battery, top right.
  {
    const uint16_t percent = std::min<uint16_t>(powerManager.getBatteryPercentage(), 100);
    char text[8];
    snprintf(text, sizeof(text), "%u%%", percent);
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, text);
    renderer.drawText(SMALL_FONT_ID, pageWidth - side - textW, 4, text);
    GUI.fillBatteryIcon(renderer, Rect{pageWidth - side - textW - 26, 6, 18, 11}, percent);
  }

  // Clock and date.
  struct tm now {};
  const bool hasTime = halClock.localTime(now) && now.tm_year >= 120;
  char clock[8] = "--:--";
  if (hasTime) {
    snprintf(clock, sizeof(clock), "%02d:%02d", now.tm_hour, now.tm_min);
    shownMinute = now.tm_min;
  }
  constexpr int clockScale = 9;
  const int clockY = 30;
  waddle::drawPixelText(renderer, side, clockY, clock, clockScale);
  const int dateY = clockY + 7 * clockScale + 10;
  if (hasTime) {
    char date[40];
    snprintf(date, sizeof(date), "%s %d. %s", WEEKDAYS[now.tm_wday % 7], now.tm_mday, MONTHS[now.tm_mon % 12]);
    renderer.drawText(UI_12_FONT_ID, side, dateY, date);
  }

  // Weather, top right under the battery.
  if (weather.valid) {
    char temp[8];
    snprintf(temp, sizeof(temp), "%dd", weather.temp);
    constexpr int tempScale = 5;
    constexpr int iconScale = 2;
    const int tempW = waddle::pixelTextWidth(temp, tempScale);
    const int iconX = pageWidth - side - tempW - 10 - waddle::WEATHER_ICON_SIZE * iconScale;
    drawWeatherIcon(renderer, weather.icon, iconX, 26, iconScale);
    waddle::drawPixelText(renderer, pageWidth - side - tempW, 40, temp, tempScale);
    char range[48];
    snprintf(range, sizeof(range), "%s · %d° / %d°", weather.place.c_str(), weather.min, weather.max);
    renderer.drawText(SMALL_FONT_ID, iconX, 96, range);
    renderer.drawText(SMALL_FONT_ID, iconX, 96 + smallH + 2, weather.text.c_str());
  }

  const int dividerY = dateY + lineH + 14;
  renderer.fillRect(side, dividerY, pageWidth - 2 * side, 2);

  // Book being read.
  const int bookY = dividerY + 16;
  boxes[T_BOOK] = {side - 6, bookY - 6, pageWidth - 2 * (side - 6), COVER_H + 12};
  if (hasBook) {
    bool coverDrawn = false;
    if (!book.coverBmpPath.empty()) {
      HalFile file;
      if (Storage.openFileForRead("HJEM", UITheme::getCoverThumbPath(book.coverBmpPath, COVER_H), file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          coverDrawn = BaseTheme::drawCoverThumbFill(renderer, bitmap, Rect{side, bookY, COVER_W, COVER_H});
        }
        file.close();
      }
    }
    if (!coverDrawn) BaseTheme::drawCoverPlaceholder(renderer, Rect{side, bookY, COVER_W, COVER_H});
    renderer.drawRect(side, bookY, COVER_W, COVER_H, true);

    const int textX = side + COVER_W + 16;
    const int textW = pageWidth - textX - side;
    int y = bookY;
    renderer.drawText(SMALL_FONT_ID, textX, y, "Fortsett å lese");
    y += smallH + 6;
    const auto title = renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), textW, 2, EpdFontFamily::BOLD);
    for (const auto& line : title) {
      renderer.drawText(UI_12_FONT_ID, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += lineH;
    }
    if (!book.author.empty()) {
      renderer.drawText(SMALL_FONT_ID, textX, y + 2,
                        renderer.truncatedText(SMALL_FONT_ID, book.author.c_str(), textW).c_str());
    }
    if (bookProgress >= 0) {
      const int barY = bookY + COVER_H - smallH - 26;
      renderer.drawRoundedRect(textX, barY, textW, 14, 2, 7, true);
      const int filled = (textW - 6) * std::min(bookProgress, 100) / 100;
      if (filled > 0) renderer.fillRect(textX + 3, barY + 3, filled, 8);
      char percent[16];
      snprintf(percent, sizeof(percent), "%d %%", bookProgress);
      renderer.drawText(SMALL_FONT_ID, textX, barY + 22, percent);
    }
  } else {
    BaseTheme::drawCoverPlaceholder(renderer, Rect{side, bookY, COVER_W, COVER_H});
    const int textX = side + COVER_W + 16;
    renderer.drawText(UI_12_FONT_ID, textX, bookY + 30, "Ingen bok ennå", true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, textX, bookY + 30 + lineH + 4, "Trykk for å åpne biblioteket");
  }

  // Top unread headlines.
  int y = bookY + COVER_H + 26;
  renderer.drawText(SMALL_FONT_ID, side, y, "NYHETER", true, EpdFontFamily::BOLD);
  if (!paper.stories.empty()) {
    char count[24];
    snprintf(count, sizeof(count), "%d uleste  >", unread);
    const int countW = renderer.getTextWidth(SMALL_FONT_ID, count);
    renderer.drawText(SMALL_FONT_ID, pageWidth - side - countW, y, count);
  }
  boxes[T_NEWS_LIST] = {side - 6, y - 6, pageWidth - 2 * (side - 6), smallH + 12};
  y += smallH + 12;
  if (topStories.empty()) {
    renderer.drawText(SMALL_FONT_ID, side, y, paper.stories.empty() ? "Ingen nyheter ennå" : "Alt er lest!");
    y += smallH + 12;
  }
  for (size_t k = 0; k < topStories.size(); k++) {
    const auto& story = paper.stories[topStories[k]];
    const auto lines =
        renderer.wrappedText(UI_12_FONT_ID, story.title.c_str(), pageWidth - side - 42, 2, EpdFontFamily::BOLD);
    renderer.fillRect(side + 2, y + lineH / 2 - 3, 6, 6);
    for (size_t n = 0; n < lines.size(); n++) {
      renderer.drawText(UI_12_FONT_ID, side + 20, y + static_cast<int>(n) * lineH, lines[n].c_str(), true,
                        EpdFontFamily::BOLD);
    }
    const int storyH = static_cast<int>(lines.size()) * lineH + 12;
    boxes[T_STORY_1 + k] = {side - 6, y - 6, pageWidth - 2 * (side - 6), storyH};
    y += storyH;
  }
  renderer.fillRect(side, y, pageWidth - 2 * side, 2);
  const int newsBottom = y + 2;

  // Buttons, icons only: Bibliotek, Filer, Overføring, Bluesound, Innstillinger.
  constexpr int buttonH = 64;
  constexpr int gap = 8;
  const int buttonsY = pageHeight - buttonH - 36;
  const int buttonW = (pageWidth - 2 * side - 4 * gap) / 5;
  const uint8_t* icons[] = {LibraryIcon, FolderIcon, TransferIcon, nullptr, Settings2Icon};
  for (int i = 0; i < 5; i++) {
    const int x = side + i * (buttonW + gap);
    renderer.drawRoundedRect(x, buttonsY, buttonW, buttonH, 2, 12, true);
    const int iconX = x + (buttonW - 32) / 2;
    const int iconY = buttonsY + (buttonH - 32) / 2;
    if (icons[i]) {
      renderer.drawIcon(icons[i], iconX, iconY, 32);
    } else {
      waddle::drawSpeakerIcon(renderer, iconX, iconY);
    }
    boxes[T_LIBRARY + i] = {x, buttonsY, buttonW, buttonH};
  }

  // Penguin family and greeting in the space between the news and the buttons.
  const int space = buttonsY - newsBottom;
  if (space >= 90) {
    const int ground = newsBottom + space / 2 + 30;
    const int groupX = (pageWidth - 200) / 2;
    for (int x = groupX - 20; x < groupX + 220; x++) {
      for (int gy = ground; gy < ground + 6; gy++) renderer.drawPixel(x, gy, ((x + gy) & 1) == 0);
    }
    waddle::drawPenguin(renderer, groupX + 110, ground - 28 * 2, 2, true);
    waddle::drawPenguin(renderer, groupX + 172, ground - 28, 1, true);
    renderer.drawText(UI_12_FONT_ID, groupX, ground - 28 * 2 + 4, "Hei " WADDLE_OWNER_NAME "!", true,
                      EpdFontFamily::BOLD);
  }

  // When the weather and news were fetched.
  if (fetchedAt >= 0) {
    char updated[32];
    snprintf(updated, sizeof(updated), "oppdatert %02ld:%02ld", (fetchedAt % 1440) / 60, fetchedAt % 60);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 26, updated);
  }

  const Box& selected = boxes[focus];
  if (selected.w > 0) drawFocus(renderer, selected.x, selected.y, selected.w, selected.h);

  if (firstRender) {
    firstRender = false;
    renders = 1;
    if (cleanInitialRefresh) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    } else {
      renderer.displayBuffer();
    }
    return;
  }
  const bool clean = renders % CLEAN_EVERY_RENDERS == 0;
  renders++;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
