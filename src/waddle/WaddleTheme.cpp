#include "WaddleTheme.h"

#include <GfxRenderer.h>
#include <esp_random.h>

#include "WaddleDraw.h"
#include "WaddlePenguin.h"
#include "fontIds.h"

#ifndef WADDLE_OWNER_NAME
#define WADDLE_OWNER_NAME "Arne"
#endif

namespace {

constexpr const char* GREETING = "Hei " WADDLE_OWNER_NAME "!";
constexpr const char* PHRASES[] = {
    "Klar for litt lesing?", "Hvor var vi?", "Tid for et kapittel?", "Kos deg med boka!", "En side til?",
};
constexpr const char* FOOTER = "God lesing!";

// Picked once per boot. Waking from sleep is a reboot, so each wake gets a new line.
const char* phrase() {
  static const char* chosen = PHRASES[esp_random() % (sizeof(PHRASES) / sizeof(PHRASES[0]))];
  return chosen;
}

}  // namespace

void WaddleTheme::drawHeader(const GfxRenderer& renderer, const Rect rect, const char* title, const char* subtitle,
                             const bool backButton) const {
  LyraTheme::drawHeader(renderer, rect, title, subtitle, backButton);
  // Only the home screen draws an untitled root header.
  if (title != nullptr || backButton) return;

  const auto& metrics = LyraMetrics::values;
  const int x = rect.x + metrics.contentSidePadding;
  const int y = rect.y + 8;
  waddle::drawPenguin(renderer, x - 4, y - 4, 1);
  const int textX = x + WADDLE_PENGUIN_SIZE + 6;
  renderer.drawText(UI_12_FONT_ID, textX, y, GREETING, true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, textX, y + renderer.getLineHeight(UI_12_FONT_ID) + 4, phrase());
}

void WaddleTheme::drawRecentBookCover(GfxRenderer& renderer, const Rect rect, const std::vector<RecentBook>& recentBooks,
                                      const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                      bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  LyraTheme::drawRecentBookCover(renderer, rect, recentBooks, selectorIndex, coverRendered, coverBufferStored,
                                 bufferRestored, storeCoverBuffer);
  // Penguin peeking over the top-right corner of the tile. The sprite has
  // 4 empty rows on top and its feet end at row 27, so at 2x it sits on the edge.
  constexpr int scale = 2;
  const int size = WADDLE_PENGUIN_SIZE * scale;
  waddle::drawPenguin(renderer, rect.x + rect.width - size - 16, rect.y - 27 * scale + 6, scale);
}

void WaddleTheme::drawButtonMenu(GfxRenderer& renderer, const Rect rect, const int buttonCount, const int selectedIndex,
                                 const std::function<std::string(int index)>& buttonLabel,
                                 const std::function<UIIcon(int index)>& rowIcon) const {
  LyraTheme::drawButtonMenu(renderer, rect, buttonCount, selectedIndex, buttonLabel, rowIcon);

  // Penguin family on an ice floe at the bottom of the screen, below the menu rows.
  const int menuBottom = rect.y + buttonCount * (getMenuRowHeight(renderer) + LyraMetrics::values.menuSpacing);
  const int screenH = renderer.getScreenHeight();
  const int ground = screenH - 14;
  constexpr int bigScale = 2;
  const int bigTop = ground - 28 * bigScale;
  if (bigTop < menuBottom) return;  // not enough room with many menu rows

  const int screenW = renderer.getScreenWidth();
  for (int x = 0; x < screenW; x++) {
    for (int y = ground; y < ground + 6; y++) {
      renderer.drawPixel(x, y, ((x + y) & 1) == 0);
    }
  }
  waddle::drawPenguin(renderer, screenW - 190, bigTop, bigScale, true);
  waddle::drawPenguin(renderer, screenW - 110, ground - 28, 1, true);
  waddle::drawPenguin(renderer, screenW - 72, ground - 28, 1, true);
  renderer.drawText(SMALL_FONT_ID, LyraMetrics::values.contentSidePadding, ground - 30, FOOTER);
}
