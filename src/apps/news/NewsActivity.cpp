#include "NewsActivity.h"

#include <GfxRenderer.h>
#include <WiFi.h>

#include <algorithm>

#include "core/MappedInputManager.h"
#include "core/waddle/WaddleDraw.h"
#include "core/waddle/WaddleWifi.h"
#include "core/activities/ActivityManager.h"
#include "core/components/UITheme.h"
#include "core/fontIds.h"

namespace {

constexpr unsigned long WIFI_TIMEOUT_MS = 15000;

}  // namespace

NewsActivity::NewsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool fetch)
    : Activity("News", renderer, mappedInput), fetchOnEnter(fetch) {}

void NewsActivity::onEnter() {
  Activity::onEnter();
  loadList();
  moveTo(0);  // the first unread story
  if (fetchOnEnter) {
    startFetch();
  } else {
    state = State::LIST;
  }
  requestUpdate();
}

void NewsActivity::onExit() {
  if (!wifiWasOn) waddle::wifi::off();
  Activity::onExit();
}

void NewsActivity::startFetch() {
  state = State::CONNECTING;
  stale = false;
  connectStartedAt = millis();
  wifiWasOn = WiFi.status() == WL_CONNECTED;
  requestUpdate();
  if (WiFi.status() != WL_CONNECTED && !waddle::wifi::joinSaved(*this)) finishFetch(false);
}

void NewsActivity::finishFetch(const bool ok) {
  if (!wifiWasOn) waddle::wifi::off();
  if (ok) loadList();
  {
    RenderLock lock(*this);
    stale = !ok;
    state = State::LIST;
  }
  if (ok) moveTo(0);
  requestUpdate();
}

void NewsActivity::loadList() {
  waddle::news::Paper loaded;
  waddle::news::load(loaded);
  auto ids = waddle::news::loadReadIds();
  RenderLock lock(*this);
  paper = std::move(loaded);
  readIds = std::move(ids);
  buildRows();
}

// Unread stories first, in NRK's order, then a "Lest" heading and the read ones.
void NewsActivity::buildRows() {
  rows.clear();
  rows.reserve(paper.stories.size() + 1);
  int readCount = 0;
  for (size_t i = 0; i < paper.stories.size(); i++) {
    if (isRead(paper.stories[i].id)) {
      readCount++;
    } else {
      rows.push_back(static_cast<int>(i));
    }
  }
  unread = static_cast<int>(paper.stories.size()) - readCount;
  if (readCount == 0) return;
  rows.push_back(READ_HEADING);
  for (size_t i = 0; i < paper.stories.size(); i++) {
    if (isRead(paper.stories[i].id)) rows.push_back(static_cast<int>(i));
  }
}

void NewsActivity::open(const int row) {
  if (row < 0 || row >= static_cast<int>(rows.size()) || rows[row] == READ_HEADING) return;
  waddle::news::openStory(paper, readIds, rows[row]);
}

void NewsActivity::toggleRead(const int row) {
  if (row < 0 || row >= static_cast<int>(rows.size()) || rows[row] == READ_HEADING) return;
  const int index = rows[row];
  waddle::news::setRead(paper, readIds, index, !isRead(paper.stories[index].id));
  {
    RenderLock lock(*this);
    buildRows();
  }
  moveTo(selected);
}

// Selects a row, stepping over the "Lest" heading in the direction of travel.
void NewsActivity::moveTo(const int row, const int direction) {
  const int count = static_cast<int>(rows.size());
  if (count == 0) return;
  int next = std::clamp(row, 0, count - 1);
  if (rows[next] == READ_HEADING) {
    next += direction;
    if (next < 0 || next >= count) next -= 2 * direction;
    next = std::clamp(next, 0, count - 1);
  }
  selected = next;
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
      finishFetch(waddle::news::sync());
    } else if (millis() - connectStartedAt > WIFI_TIMEOUT_MS) {
      finishFetch(false);
    }
    return;
  }

  // No paper: OK or a tap tries again.
  if (paper.stories.empty()) {
    int x = 0, y = 0;
    if (mappedInput.wasReleased(Button::Confirm) || mappedInput.wasScreenTapped(x, y)) startFetch();
    return;
  }

  const int perPage = std::max(rowsPerPage, 1);
  if (mappedInput.wasReleased(Button::Confirm)) {
    open(selected);
  } else if (mappedInput.wasReleased(Button::Down)) {
    moveTo(selected + 1);
  } else if (mappedInput.wasReleased(Button::Up)) {
    moveTo(selected - 1, -1);
  } else if (mappedInput.wasReleased(Button::PageForward) || mappedInput.wasReleased(Button::Right)) {
    moveTo((selected / perPage + 1) * perPage);
  } else if (mappedInput.wasReleased(Button::PageBack) || mappedInput.wasReleased(Button::Left)) {
    moveTo((selected / perPage - 1) * perPage);
  } else {
    int x = 0, y = 0;
    // Hold a headline to mark it read or unread.
    if (mappedInput.wasScreenLongPress(x, y)) {
      if (y >= listTop && y < footerTop) toggleRead((selected / perPage) * perPage + (y - listTop) / rowHeight);
      return;
    }
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Left) {
      moveTo((selected / perPage + 1) * perPage);
    } else if (swipe == MappedInputManager::SwipeDir::Down || swipe == MappedInputManager::SwipeDir::Right) {
      moveTo((selected / perPage - 1) * perPage);
    } else if (!mappedInput.wasScreenTapped(x, y)) {
      return;
    } else if (y >= footerTop) {
      // The footer thirds are the Forrige, Hent nå and Neste buttons.
      const int third = renderer.getScreenWidth() / 3;
      if (x < third) {
        moveTo((selected / perPage - 1) * perPage);
      } else if (x < 2 * third) {
        startFetch();
      } else {
        moveTo((selected / perPage + 1) * perPage);
      }
    } else if (y >= listTop) {
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

void drawButton(const GfxRenderer& renderer, const int x, const int y, const int w, const int h, const char* label) {
  renderer.drawRoundedRect(x, y, w, h, 2, 10, true);
  const int textW = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int textY = y + (h - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, x + (w - textW) / 2, textY, label, true, EpdFontFamily::BOLD);
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

  if (paper.stories.empty()) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Nyheter");
    waddle::drawPenguin(renderer, (pageWidth - 90) / 2, pageHeight / 2 - 140, 3);
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, "Fikk ikke hentet nyheter", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 36, "Trykk for å prøve igjen");
    renderer.displayBuffer();
    return;
  }

  char subtitle[96];
  snprintf(subtitle, sizeof(subtitle), "%s  ·  %d uleste%s", paper.edition.c_str(), unread,
           stale ? "  ·  ikke oppdatert" : "");
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Nyheter", subtitle);

  // Each row has room for a two-line headline, so every page has the same rows.
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int footerH = 76;
  footerTop = pageHeight - footerH;
  listTop = metrics.topPadding + metrics.headerHeight + 8;
  rowHeight = 2 * lineH + 16;
  rowsPerPage = std::max(1, (pageHeight - listTop - footerH) / rowHeight);

  const int count = static_cast<int>(rows.size());
  const int page = selected / rowsPerPage;
  const int pages = (count + rowsPerPage - 1) / rowsPerPage;
  const int timeX = side + 2;
  const int titleX = side + 60;
  const int titleW = pageWidth - titleX - side;

  for (int row = 0; row < rowsPerPage; row++) {
    const int i = page * rowsPerPage + row;
    if (i >= count) break;
    const int y = listTop + row * rowHeight;
    if (rows[i] == READ_HEADING) {
      char heading[24];
      snprintf(heading, sizeof(heading), "LEST (%d)", static_cast<int>(paper.stories.size()) - unread);
      const int textY = y + rowHeight - renderer.getLineHeight(SMALL_FONT_ID) - 12;
      renderer.drawText(SMALL_FONT_ID, side, textY, heading, true, EpdFontFamily::BOLD);
      const int lineX = side + renderer.getTextWidth(SMALL_FONT_ID, heading, EpdFontFamily::BOLD) + 12;
      const int lineY = textY + renderer.getLineHeight(SMALL_FONT_ID) / 2;
      renderer.drawLine(lineX, lineY, pageWidth - side, lineY);
      continue;
    }
    const auto& item = paper.stories[rows[i]];
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
    const bool nextHeading = i + 1 < count && rows[i + 1] == READ_HEADING;
    if (!sel && !nextSelected && !nextHeading) renderer.drawLine(titleX, y + rowHeight - 2, pageWidth - side, y + rowHeight - 2);
  }

  // Footer: the page number, then Forrige, Hent nå and Neste.
  char footer[32];
  snprintf(footer, sizeof(footer), "Side %d av %d", page + 1, pages);
  renderer.drawCenteredText(SMALL_FONT_ID, footerTop + 2, footer);
  const int buttonW = 130;
  const int buttonH = 44;
  const int buttonY = pageHeight - buttonH - 8;
  if (page > 0) drawButton(renderer, 12, buttonY, buttonW, buttonH, "< Forrige");
  drawButton(renderer, (pageWidth - buttonW) / 2, buttonY, buttonW, buttonH, "Hent nå");
  if (page + 1 < pages) drawButton(renderer, pageWidth - 12 - buttonW, buttonY, buttonW, buttonH, "Neste >");

  renderer.displayBuffer();
}
