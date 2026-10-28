#pragma once

// Groups made on a unit (protocol/PROTOCOL.md, "Groups"): some of the members,
// with their own key. Kept in flash (NVS), one entry per group. A group keeps
// every key number it has had, so older messages still open after someone
// leaves and the group gets a new key.

#include <MeshCrypto.h>

#include <cstdint>

namespace mesh {

constexpr int MAX_GROUPS = 16;
constexpr int MAX_GROUP_KEYS = 6;  // key numbers kept per group; the oldest goes first
constexpr size_t GROUP_NAME = 24;

struct GroupKey {
  uint8_t number = 0;  // 0 = unused
  uint8_t from = 0;    // the member who made it (the lower one wins a tie)
  uint8_t key[KEY_SIZE] = {};
};

struct ChatGroup {
  uint8_t id = 0;       // creator and count (MeshPacket.h groupId); 0 = free slot
  uint8_t members = 0;  // member bits, me included
  bool left = false;    // I left: kept to read the history, no longer written to
  char name[GROUP_NAME] = "";
  GroupKey keys[MAX_GROUP_KEYS];

  // The newest key, or nullptr.
  const GroupKey* newest() const;
  const uint8_t* key(uint8_t number) const;
  // Adds or replaces a key number. False when nothing changed.
  bool addKey(uint8_t number, uint8_t from, const uint8_t key[KEY_SIZE]);
};

class ChatGroups {
 public:
  void load();
  // The slot (0..MAX_GROUPS-1) of a group id, or -1.
  int find(uint8_t id) const;
  // A free slot for a new id (the oldest left group is reused when full), or -1.
  int take(uint8_t id);
  ChatGroup& at(int slot) { return groups_[slot]; }
  const ChatGroup& at(int slot) const { return groups_[slot]; }
  void save(int slot);
  // The next id for a group made on this unit.
  uint8_t nextId(uint8_t me);

 private:
  ChatGroup groups_[MAX_GROUPS];
};

}  // namespace mesh
