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

namespace {
// '#' is black, anything else is left untouched.
constexpr const char* NOTE_SINGLE[] = {"   #   ", "   ##  ", "   # # ", "   #  #", "   #   ",
                                       "   #   ", " ###   ", "####   ", "####   ", " ##    "};
constexpr const char* NOTE_DOUBLE[] = {"  #####", "  #   #", "  #   #", "  #   #", "  #   #",
                                       " ##  ##", "### ###", "### ###", " #   # "};

constexpr const char* NEWS_ICON[32] = {
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "    ########################    ",
    "    ########################    ",
    "    ##                    ##    ",
    "    ##                    ##    ",
    "    ##  ################  ##    ",
    "    ##  ################  ##    ",
    "    ##  ################  ##    ",
    "    ##                    ##    ",
    "    ##                    ##    ",
    "    ##  #######  #######  ##    ",
    "    ##  #######           ##    ",
    "    ##  #######           ##    ",
    "    ##  #######  #######  ##    ",
    "    ##  #######           ##    ",
    "    ##  #######           ##    ",
    "    ##           #######  ##    ",
    "    ##                    ##    ",
    "    ##  ################  ##    ",
    "    ##                    ##    ",
    "    ##                    ##    ",
    "    ########################    ",
    "    ########################    ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                "};
}  // namespace

void drawNote(const GfxRenderer& renderer, const int x, const int y, const int scale, const int kind) {
  const char* const* rows = kind == 0 ? NOTE_SINGLE : NOTE_DOUBLE;
  const int rowCount = kind == 0 ? 10 : 9;
  for (int row = 0; row < rowCount; row++) {
    for (int col = 0; rows[row][col] != '\0'; col++) {
      if (rows[row][col] != '#') continue;
      renderer.fillRect(x + col * scale, y + row * scale, scale, scale);
    }
  }
}

namespace {
constexpr const char* SPEAKER_ICON[32] = {
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                #               ",
    "               ##               ",
    "              ###         ##    ",
    "             ####         ###   ",
    "            ## ##          ###  ",
    "          ###  ##           ##  ",
    "    ########   ##      ##   ### ",
    "    ######     ##      ###   ## ",
    "    ##         ##       ##   ## ",
    "    ##         ##       ##   ## ",
    "    ##         ##       ##   ## ",
    "    ##         ##       ##   ## ",
    "    ######     ##      ###   ## ",
    "    ########   ##      ##   ### ",
    "          ###  ##           ##  ",
    "           ### ##          ###  ",
    "            #####         ###   ",
    "             ####         ##    ",
    "              ###               ",
    "               ##               ",
    "                #               ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                "};
}  // namespace

void drawSpeakerIcon(const GfxRenderer& renderer, const int x, const int y) {
  for (int row = 0; row < 32; row++) {
    for (int col = 0; col < 32; col++) {
      if (SPEAKER_ICON[row][col] == '#') renderer.drawPixel(x + col, y + row, true);
    }
  }
}

void drawNewsIcon(const GfxRenderer& renderer, const int x, const int y) {
  for (int row = 0; row < 32; row++) {
    for (int col = 0; col < 32; col++) {
      if (NEWS_ICON[row][col] == '#') renderer.drawPixel(x + col, y + row, true);
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
