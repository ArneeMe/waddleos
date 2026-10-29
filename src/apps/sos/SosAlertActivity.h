#pragma once

#include "core/activities/Activity.h"

// My own SOS (opened by holding the user button for 3 seconds, or the SOS
// button on Hjem): black screen, sent again every minute until "Avbryt SOS".
class SosActivity final : public Activity {
 public:
  // countdown: started by holding the button, so it waits 10 s (any press stops
  // it) before sending, in case the button was pressed by accident.
  SosActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool countdown = false);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  int cancelY = 0;
  unsigned long countdownFrom = 0;  // millis() when the countdown began; 0 = sending
  int shownSeconds = -1;
  unsigned long drawnAt = 0;
  uint32_t shownChanges = 0;
};

// An SOS from another member: takes over the screen until it is seen.
class SosAlertActivity final : public Activity {
 public:
  SosAlertActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint8_t from);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  uint8_t from;
  uint32_t shownChanges = 0;
};
