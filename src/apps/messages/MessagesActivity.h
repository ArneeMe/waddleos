#pragma once

#include <vector>

#include "core/activities/Activity.h"
#include "services/mesh/MessageStore.h"

// Meldinger: everyone ("Alle"), one row per member, the small groups in use,
// and "Ny gruppe". Each row shows the newest message and the unread count.
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
  static constexpr uint8_t NEW_GROUP = 0xFF;  // the "Ny gruppe" row
  std::vector<uint8_t> conversations;         // MessageStore.h numbering, and NEW_GROUP
  std::vector<Box> rows;
  Box syncBox;
  int focus = 0;
  int first = 0;  // first row shown, when they do not all fit
  uint32_t shownChanges = 0;

  void open(uint8_t conversation);
};

// Ny gruppe: tick the people to write to. Two people is a DM, everyone is
// "Alle", and anything between is a small group (keys from `waddle mesh setup`).
class NewGroupActivity final : public Activity {
 public:
  NewGroupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  std::vector<uint8_t> members;  // the others
  std::vector<Box> rows;
  Box doneBox;
  uint8_t chosen = 0;  // member bits
  int focus = 0;       // a member row, or members.size() for the button
  const char* problem = nullptr;

  void toggle(int index);
  void finish();
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

  std::string name;

  void reload();
  void write();
};
