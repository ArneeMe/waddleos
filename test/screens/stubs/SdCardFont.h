#pragma once
// Fonts from the SD card: none in the screen simulator (only the built-in
// fonts are used by the mesh screens). Enough for GfxRenderer to compile.
#include <EpdFont.h>
#include <EpdFontData.h>

#include <cstdint>

class SdCardFont {
 public:
  template <class... A>
  int prewarm(A&&...) {
    return 0;
  }
  template <class... A>
  int buildAdvanceTable(A&&...) {
    return 0;
  }
  template <class... A>
  int buildAdvanceTablePacked(A&&...) {
    return 0;
  }
  uint16_t getAdvance(uint32_t, uint8_t) const { return 0; }
  bool hasAdvanceTable() const { return false; }
  void clearCache() {}
  void releaseResidentCaches() {}
  uint8_t resolveStyle(uint8_t style) const { return style; }
  bool isOverflowGlyph(const EpdGlyph*) const { return false; }
  const uint8_t* getOverflowBitmap(const EpdGlyph*) const { return nullptr; }
  const EpdFont* getEpdFont(uint8_t) const { return nullptr; }
  void logStats(const char*) {}
  void resetStats() {}
  static SdCardFont* fromMissCtx(void* ctx) { return static_cast<SdCardFont*>(ctx); }
};
