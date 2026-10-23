#include "WaddleDraw.h"

#include <GfxRenderer.h>

#include "WaddlePenguin.h"
#include "fontIds.h"

namespace waddle {

void drawPenguin(const GfxRenderer& renderer, const int x, const int y, const int scale, const bool mirror) {
  for (int row = 0; row < WADDLE_PENGUIN_SIZE; row++) {
    for (int col = 0; col < WADDLE_PENGUIN_SIZE; col++) {
      const char c = WADDLE_PENGUIN[row][mirror ? WADDLE_PENGUIN_SIZE - 1 - col : col];
      if (c == ' ') continue;
      for (int dy = 0; dy < scale; dy++) {
        for (int dx = 0; dx < scale; dx++) {
          const int px = x + col * scale + dx;
          const int py = y + row * scale + dy;
          const bool black = c == '#' || (c == ':' && ((px + py) & 1) == 0);
          renderer.drawPixel(px, py, black);
        }
      }
    }
  }
}

void drawSplash(const GfxRenderer& renderer, const char* subtitle) {
  constexpr int scale = 5;
  const int size = WADDLE_PENGUIN_SIZE * scale;
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int top = (pageHeight - size) / 2 - 40;
  drawPenguin(renderer, (pageWidth - size) / 2, top, scale);
  // The sprite's feet end at row 27, so text starts just below them.
  const int textY = top + 28 * scale + 24;
  renderer.drawCenteredText(UI_12_FONT_ID, textY, "WaddleOS", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, textY + renderer.getLineHeight(UI_12_FONT_ID) + 8, subtitle);
}

}  // namespace waddle
