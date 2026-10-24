#include "WaddleDraw.h"

#include <GfxRenderer.h>

#include <cstring>

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

// 5x7 pixel glyphs for the big clock and temperature. 'd' is the degree sign.
struct PixelGlyph {
  char c;
  const char* rows[7];
};
constexpr PixelGlyph PIXEL_GLYPHS[] = {
    {'0', {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}},
    {'1', {"  #  ", " ##  ", "# #  ", "  #  ", "  #  ", "  #  ", "#####"}},
    {'2', {" ### ", "#   #", "    #", "  ## ", " #   ", "#    ", "#####"}},
    {'3', {" ### ", "#   #", "    #", "  ## ", "    #", "#   #", " ### "}},
    {'4', {"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}},
    {'5', {"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}},
    {'6', {"  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### "}},
    {'7', {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "}},
    {'8', {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}},
    {'9', {" ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  "}},
    {':', {"  ", "##", "##", "  ", "##", "##", "  "}},
    {'-', {"    ", "    ", "    ", "####", "    ", "    ", "    "}},
    {'d', {" ## ", "#  #", "#  #", " ## ", "    ", "    ", "    "}},
};

const PixelGlyph* pixelGlyph(const char c) {
  for (const auto& glyph : PIXEL_GLYPHS) {
    if (glyph.c == c) return &glyph;
  }
  return nullptr;
}
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

int pixelTextWidth(const char* text, const int scale) {
  int width = 0;
  for (const char* p = text; *p; p++) {
    if (const PixelGlyph* glyph = pixelGlyph(*p)) width += (static_cast<int>(strlen(glyph->rows[0])) + 1) * scale;
  }
  return width > 0 ? width - scale : 0;
}

void drawPixelText(const GfxRenderer& renderer, int x, const int y, const char* text, const int scale) {
  for (const char* p = text; *p; p++) {
    const PixelGlyph* glyph = pixelGlyph(*p);
    if (!glyph) continue;
    const int glyphW = static_cast<int>(strlen(glyph->rows[0]));
    for (int row = 0; row < 7; row++) {
      for (int col = 0; col < glyphW; col++) {
        if (glyph->rows[row][col] == '#') renderer.fillRect(x + col * scale, y + row * scale, scale, scale);
      }
    }
    x += (glyphW + 1) * scale;
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
