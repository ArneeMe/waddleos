#pragma once

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
  int toggleY = 0;
  uint32_t shownChanges = 0;
  unsigned long drawnAt = 0;
};
