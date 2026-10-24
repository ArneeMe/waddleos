#include "NewsActivity.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <WiFi.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "WaddleDraw.h"
#include "WaddleWifi.h"
#include "activities/ActivityManager.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

constexpr const char* DIR = "/Nyheter";
constexpr const char* LIST_PATH = "/Nyheter/nrk.json";
constexpr const char* NEW_LIST_PATH = "/Nyheter/nrk.new.json";
constexpr const char* READ_PATH = "/Nyheter/lest.txt";
constexpr const char* LIST_URL = "https://arnee.me/api/nrk.json";
constexpr const char* EPUB_URL = "https://arnee.me/api/nrk.epub";
constexpr unsigned long WIFI_TIMEOUT_MS = 15000;

// Reads the "bygd" (build time) field of a downloaded list.
std::string builtOf(const char* path) {
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path, doc)) return "";
  return doc["bygd"] | "";
}

}  // namespace

NewsActivity::NewsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool fetch)
    : Activity("News", renderer, mappedInput), fetchOnEnter(fetch) {}

void NewsActivity::onEnter() {
  Activity::onEnter();
  loadList();
  loadRead();
  if (fetchOnEnter) {
    startFetch();
  } else {
    state = State::LIST;
    // Select the first unread story.
    for (size_t i = 0; i < items.size(); i++) {
      if (!isRead(items[i].id)) {
        selected = static_cast<int>(i);
        break;
      }
    }
  }
  requestUpdate();
}

void NewsActivity::onExit() {
  waddle::wifi::off();
  Activity::onExit();
}

void NewsActivity::startFetch() {
  state = State::CONNECTING;
  stale = false;
  connectStartedAt = millis();
  requestUpdate();
  if (WiFi.status() != WL_CONNECTED && !waddle::wifi::joinSaved(*this)) finishFetch(false);
}

// Downloads the list, and the book only when the paper is new. The old files
// stay until the new ones are complete.
bool NewsActivity::download() {
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("NEWS", "low heap for download (%u free)", ESP.getFreeHeap());
    return false;
  }
  Storage.mkdir(DIR);
  if (HttpDownloader::downloadToFile(LIST_URL, NEW_LIST_PATH) != HttpDownloader::OK) return false;
  const std::string newBuilt = builtOf(NEW_LIST_PATH);
  if (newBuilt.empty()) {
    Storage.remove(NEW_LIST_PATH);
    return false;
  }
  if (newBuilt == built && Storage.exists(waddle::news::EPUB_PATH)) {
    Storage.remove(NEW_LIST_PATH);
    return true;
  }
  if (HttpDownloader::downloadToFile(EPUB_URL, waddle::news::EPUB_PATH) != HttpDownloader::OK) {
    Storage.remove(NEW_LIST_PATH);
    return false;
  }
  // Same file name as the old paper, so drop the old paper's layout cache.
  Epub(waddle::news::EPUB_PATH, "/.crosspoint").clearCache();
  return Storage.replaceFile(NEW_LIST_PATH, LIST_PATH);
}

void NewsActivity::finishFetch(const bool ok) {
  waddle::wifi::off();
  if (ok) loadList();
  {
    RenderLock lock(*this);
    if (ok) selected = 0;
    stale = !ok;
    state = State::LIST;
  }
  requestUpdate();
}

bool NewsActivity::loadList() {
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(LIST_PATH, doc)) return false;
  RenderLock lock(*this);
  edition = doc["tittel"] | "NRK";
  built = doc["bygd"] | "";
  items.clear();
  JsonArray list = doc["liste"].as<JsonArray>();
  items.reserve(list.size());
  for (JsonObject obj : list) {
    items.push_back({obj["id"] | "", obj["t"] | "", obj["kl"] | ""});
  }
  return true;
}

// lest.txt: one article id per line.
void NewsActivity::loadRead() {
  readIds.clear();
  if (!Storage.exists(READ_PATH)) return;
  const String text = Storage.readFile(READ_PATH);
  int start = 0;
  while (start < static_cast<int>(text.length())) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = text.length();
    if (end > start) readIds.emplace_back(text.substring(start, end).c_str());
    start = end + 1;
  }
}

bool NewsActivity::isRead(const std::string& id) const {
  return std::find(readIds.begin(), readIds.end(), id) != readIds.end();
}

void NewsActivity::open(const int index) {
  if (index < 0 || index >= static_cast<int>(items.size()) || !Storage.exists(waddle::news::EPUB_PATH)) return;

  // Remember it as read. Only ids in today's paper are kept, so the file stays small.
  if (!isRead(items[index].id)) readIds.push_back(items[index].id);
  String text;
  for (const auto& item : items) {
    if (isRead(item.id)) {
      text += item.id.c_str();
      text += '\n';
    }
  }
  Storage.writeFile(READ_PATH, text);

  // Chapter 0 is the front page, so story i is chapter i + 1. Saving it as the
  // reading position makes the reader open there.
  Epub epub(waddle::news::EPUB_PATH, "/.crosspoint");
  epub.setupCacheDir();
  EpubReaderUtils::saveProgress(epub, index + 1, 0, 0);
  activityManager.goToReader(waddle::news::EPUB_PATH);
}

void NewsActivity::moveTo(const int index) {
  const int count = static_cast<int>(items.size());
  if (count == 0) return;
  selected = std::clamp(index, 0, count - 1);
  requestUpdate();
}

void NewsActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) {
    activityManager.goHome();
    return;
  }

  if (state == State::CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      finishFetch(download());
    } else if (millis() - connectStartedAt > WIFI_TIMEOUT_MS) {
      finishFetch(false);
    }
    return;
  }

  // Hold OK to fetch again. wasLongPressed() swallows the release that follows.
  if (mappedInput.wasLongPressed(Button::Confirm, ReaderUtils::SKIP_HOLD_MS)) {
    startFetch();
    return;
  }

  const int perPage = std::max(rowsPerPage, 1);
  if (mappedInput.wasReleased(Button::Confirm)) {
    open(selected);
  } else if (mappedInput.wasReleased(Button::Down)) {
    moveTo(selected + 1);
  } else if (mappedInput.wasReleased(Button::Up)) {
    moveTo(selected - 1);
  } else if (mappedInput.wasReleased(Button::PageForward) || mappedInput.wasReleased(Button::Right)) {
    moveTo((selected / perPage + 1) * perPage);
  } else if (mappedInput.wasReleased(Button::PageBack) || mappedInput.wasReleased(Button::Left)) {
    moveTo((selected / perPage - 1) * perPage);
  } else {
    int x = 0, y = 0;
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Left) {
      moveTo((selected / perPage + 1) * perPage);
    } else if (swipe == MappedInputManager::SwipeDir::Down || swipe == MappedInputManager::SwipeDir::Right) {
      moveTo((selected / perPage - 1) * perPage);
    } else if (mappedInput.wasScreenTapped(x, y) && y >= listTop) {
      const int row = (y - listTop) / rowHeight;
      if (row < perPage) open((selected / perPage) * perPage + row);
    }
  }
}

namespace {

// Small check mark, about 14x10, top-left at (x, y).
void drawCheck(const GfxRenderer& renderer, const int x, const int y, const bool black) {
  renderer.drawLine(x, y + 5, x + 4, y + 9, 2, black);
  renderer.drawLine(x + 4, y + 9, x + 13, y, 2, black);
}

}  // namespace

void NewsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int side = 20;

  renderer.clearScreen();

  if (state == State::CONNECTING) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Nyheter");
    waddle::drawPenguin(renderer, (pageWidth - 90) / 2, pageHeight / 2 - 140, 3);
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, "Henter nyheter...", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (items.empty()) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Nyheter");
    waddle::drawPenguin(renderer, (pageWidth - 90) / 2, pageHeight / 2 - 140, 3);
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, "Fikk ikke hentet nyheter", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 36, "Hold OK for å prøve igjen");
    renderer.displayBuffer();
    return;
  }

  char subtitle[96];
  snprintf(subtitle, sizeof(subtitle), "%s  ·  %d saker%s", edition.c_str(), static_cast<int>(items.size()),
           stale ? "  ·  ikke oppdatert" : "");
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Nyheter", subtitle);

  // Each row has room for a two-line headline, so every page has the same rows.
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int footerH = 36;
  listTop = metrics.topPadding + metrics.headerHeight + 8;
  rowHeight = 2 * lineH + 16;
  rowsPerPage = std::max(1, (pageHeight - listTop - footerH) / rowHeight);

  const int count = static_cast<int>(items.size());
  const int page = selected / rowsPerPage;
  const int pages = (count + rowsPerPage - 1) / rowsPerPage;
  const int timeX = side + 2;
  const int titleX = side + 60;
  const int titleW = pageWidth - titleX - side;

  for (int row = 0; row < rowsPerPage; row++) {
    const int i = page * rowsPerPage + row;
    if (i >= count) break;
    const auto& item = items[i];
    const int y = listTop + row * rowHeight;
    const bool sel = i == selected;
    const bool read = isRead(item.id);
    const bool ink = !sel;  // white text on the black selection

    if (sel) renderer.fillRoundedRect(side - 8, y, pageWidth - 2 * (side - 8), rowHeight - 4, 8, Color::Black);
    renderer.drawText(SMALL_FONT_ID, timeX, y + 10, item.time.c_str(), ink);
    if (read) drawCheck(renderer, timeX, y + 38, ink);

    const auto style = read ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
    const auto lines = renderer.wrappedText(UI_12_FONT_ID, item.title.c_str(), titleW, 2, style);
    for (size_t k = 0; k < lines.size(); k++) {
      renderer.drawText(UI_12_FONT_ID, titleX, y + 6 + static_cast<int>(k) * lineH, lines[k].c_str(), ink, style);
    }
    const bool nextSelected = i + 1 == selected;
    if (!sel && !nextSelected) renderer.drawLine(titleX, y + rowHeight - 2, pageWidth - side, y + rowHeight - 2);
  }

  char footer[32];
  snprintf(footer, sizeof(footer), "Side %d av %d", page + 1, pages);
  const int footerY = pageHeight - footerH + 6;
  renderer.drawText(SMALL_FONT_ID, side, footerY, footer);
  const char* hint = "Hold OK: hent nå";
  renderer.drawText(SMALL_FONT_ID, pageWidth - side - renderer.getTextWidth(SMALL_FONT_ID, hint), footerY, hint);

  renderer.displayBuffer();
}
