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

// Ny gruppe: tick the people to write to. One person opens the DM; otherwise
// a group is made (with an optional name) and its key sent to the others.
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
  std::vector<uint8_t> members;  // the others, by name
  std::vector<Box> rows;         // per member; empty when not on screen
  Box doneBox, upBox, downBox;
  mesh::Members chosen = 0;
  int focus = 0;  // a member row, or members.size() for the button
  int first = 0;  // first member shown, when they do not all fit
  const char* problem = nullptr;

  void toggle(int index);
  void finish();
  void openChat(uint8_t conversation);
};

// A group: its members, and "Forlat gruppe" (nobody takes others out: each
// leaves themselves; a lost unit leaves every group when Arne takes it out of
// the network).
class GroupInfoActivity final : public Activity {
 public:
  GroupInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint8_t conversation);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  uint8_t conversation;
  Box leaveBox, moreBox;
  int page = 0;  // of the member list, in a large group
  bool armed = false;  // "Forlat gruppe" tapped once, waiting for "Sikker?"
  uint32_t shownChanges = 0;
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
  int titleBottom = 0;  // tapping above it opens a group's members

  void reload();
  void write();
};
