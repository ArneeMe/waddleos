#pragma once

#include <vector>

#include "core/activities/Activity.h"

// Innstillinger on the T5, in three parts: what applies to everything (sleep,
// Wi-Fi, clock, update), books (the reader's settings, one row away), and what
// only the T5 has (messages and position).
class T5SettingsActivity final : public Activity {
 public:
  T5SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  // The rows, in order (the last ones open another screen: T5SettingsLinks.cpp).
  enum Row { SLEEP, WIFI, CLOCK, UPDATE, READING, MODE, HOME_CHECK, HOME_WIFI, TRIP_CHECK, POSITION, ROWS };

 private:
  struct Box {
    int y = 0, h = 0;
  };
  Box boxes[ROWS];
  int focus = SLEEP;
  void activate(int row);
};

// Opens the screen behind a row that leads elsewhere (Wi-Fi, clock, update,
// the reader's settings). Kept apart so the screen simulator can draw
// this page without those screens.
void openT5Setting(T5SettingsActivity::Row row);
