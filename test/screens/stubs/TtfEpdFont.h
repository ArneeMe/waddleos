#pragma once
// TTF fonts: none in the screen simulator. Enough for GfxRenderer to compile.
#include <EpdFontFamily.h>

class TtfEpdFont {
 public:
  void clearCache() {}
  void releaseResidentCaches() {}
  bool build(const char*) { return false; }
  bool addCoverage(const char*) { return false; }
};
