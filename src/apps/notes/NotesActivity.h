#pragma once

#include <string>
#include <vector>

#include "core/activities/Activity.h"

// Notater: short personal notes in /Notater on the SD card, one text file each.
// Tap a note to change it; empty it to delete it.
class NotesActivity final : public Activity {
 public:
  NotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  std::vector<std::string> files;  // newest first
  std::vector<std::string> previews;
  std::vector<Box> rows;
  Box newBox;
  int focus = 0;  // 0 = "Nytt notat", 1.. = notes

  void reload();
  void edit(int index);  // -1 = new
};
