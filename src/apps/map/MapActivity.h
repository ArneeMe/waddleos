#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "core/activities/Activity.h"

// Kart: where the others are, as a compass (north up) with distance and
// direction, from the last positions they shared. Works without map tiles.
// The button at the bottom turns off-grid mode (position sharing) on and off.
class MapActivity final : public Activity {
 public:
  MapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  int toggleY = 0;
  std::vector<std::pair<uint8_t, Box>> taps;  // tap a dot or a row: that person
  uint32_t shownChanges = 0;
  unsigned long drawnAt = 0;
};

// One person from the map: how far, which way (an arrow), how old the position
// is, height, battery, coordinates, when last heard, and "Skriv til …".
class PersonActivity final : public Activity {
 public:
  PersonActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint8_t member);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  uint8_t member;
  int writeY = 0;
  uint32_t shownChanges = 0;
  unsigned long drawnAt = 0;
};
