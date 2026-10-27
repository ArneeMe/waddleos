#pragma once

#include <vector>

#include "core/activities/Activity.h"
#include "services/mesh/MessageStore.h"

// Meldinger: the group and one row per member, newest message and unread count.
// "Synk" asks the server for anything missed.
class MessagesActivity final : public Activity {
 public:
  MessagesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  std::vector<uint8_t> conversations;  // CONV_GROUP and member numbers
  std::vector<Box> rows;
  Box syncBox;
  int focus = 0;
  uint32_t shownChanges = 0;

  void open(uint8_t conversation);
};

// One conversation: messages as bubbles, a field to write in.
class ChatActivity final : public Activity {
 public:
  ChatActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint8_t conversation);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  uint8_t conversation;
  std::vector<mesh::ChatMessage> messages;
  int scroll = 0;  // messages hidden at the bottom (0 = newest shown)
  uint32_t shownChanges = 0;
  int inputY = 0;
  int renders = 0;

  void reload();
  void write();
  const char* title() const;
};
