#pragma once

#include "core/activities/Activity.h"

// Hjem on the mesh unit (docs/waddle/mockups/t5-screens.png): clock, date and
// mesh status, the conversations with their newest message and unread count,
// six app buttons, and the penguin.
class T5HomeActivity final : public Activity {
 public:
  T5HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool cleanInitialRefresh = false);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

 private:
  struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  // Things that can be selected, in Up/Down order.
  enum Target { T_CHAT_1, T_CHAT_2, T_CHAT_3, T_CHAT_4, T_MESSAGES, T_MAP, T_NOTES, T_BOOKS, T_SETTINGS, T_SOS, T_COUNT };

  bool cleanInitialRefresh;
  int shownMinute = -1;
  uint32_t shownChanges = 0;
  int focus = T_MESSAGES;
  uint8_t chats[4] = {};  // conversations shown in the rows
  int chatCount = 0;
  Box boxes[T_COUNT];

  bool available(int target) const;
  void activate(int target);
};
