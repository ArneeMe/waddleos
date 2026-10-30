#pragma once

#include "core/activities/Activity.h"

// Apper on the T5: what is not about the mesh. Bøker (the library) and Nyheter
// (NRK, fetched only when opened). More can be added as rows.
class T5AppsActivity final : public Activity {
 public:
  T5AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  enum Row { BOOKS, NEWS, ROWS };

 private:
  struct Box {
    int y = 0, h = 0;
  };
  Box boxes[ROWS];
  int focus = BOOKS;
};

// Opens an app (T5SettingsLinks.cpp, so the screen simulator can draw this
// page without the apps).
void openT5App(T5AppsActivity::Row row);
