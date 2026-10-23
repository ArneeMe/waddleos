#pragma once

class GfxRenderer;

namespace waddle {

// Draws the 30x30 penguin sprite with its top-left corner at (x, y), each
// pixel as a scale x scale block. Transparent pixels are left untouched.
void drawPenguin(const GfxRenderer& renderer, int x, int y, int scale, bool mirror = false);

// Boot and default sleep screen: big penguin centered on the screen, then
// "WaddleOS" and `subtitle` below it.
void drawSplash(const GfxRenderer& renderer, const char* subtitle);

}  // namespace waddle
