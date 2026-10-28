#pragma once

class GfxRenderer;

namespace waddle {

// Draws the 30x30 penguin sprite with its top-left corner at (x, y), each
// pixel as a scale x scale block. Transparent pixels are left untouched.
void drawPenguin(const GfxRenderer& renderer, int x, int y, int scale, bool mirror = false);

// Small music note (two shapes, `kind` 0 or 1), top-left at (x, y).
void drawNote(const GfxRenderer& renderer, int x, int y, int scale, int kind);

// 32x32 speaker icon in the style of the menu icons, top-left at (x, y).
void drawSpeakerIcon(const GfxRenderer& renderer, int x, int y);

// Big pixel text for the clock and temperature: digits, ':', '-', 'd' (the
// degree sign), and 'S' and 'O' for SOS. Each glyph is 5x7 pixels (':' is 2 wide), drawn scale x scale.
// Other characters are skipped. y is the top of the glyphs. black = false draws
// white, for black backgrounds.
void drawPixelText(const GfxRenderer& renderer, int x, int y, const char* text, int scale, bool black = true);
int pixelTextWidth(const char* text, int scale);

// Boot and default sleep screen: big penguin centered on the screen, then
// "WaddleOS" and `subtitle` below it.
void drawSplash(const GfxRenderer& renderer, const char* subtitle);

}  // namespace waddle
