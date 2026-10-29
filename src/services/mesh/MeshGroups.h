#pragma once

// Keys this unit holds besides its own (protocol/PROTOCOL.md):
//  - ChatGroups: groups made on a unit, each with its key numbers ("Groups").
//  - SenderKeys: the other members' sender keys ("Encryption").
// Both kept on the SD card in sealed files (MeshConfig.h, saveSealed).

#include <MeshCrypto.h>
#include <MeshPacket.h>

#include <cstdint>

namespace mesh {

constexpr int MAX_GROUPS = 16;
constexpr int MAX_GROUP_KEYS = 6;  // key numbers kept per group; the oldest goes first
constexpr size_t GROUP_NAME = 24;
constexpr int MAX_NOTICES = 8;  // unconfirmed invitations and notices per group

struct GroupKey {
  uint8_t number = 0;  // 0 = unused
  uint8_t from = 0;    // the member who made it (the lower one wins a tie)
  uint8_t key[KEY_SIZE] = {};
};

// My invitation or notice to a member that their unit has not confirmed yet.
// Sent again on each check, also after a restart, until it is.
struct Notice {
  uint8_t member = 0;  // 0 = unused
  uint8_t kind = 0;    // Control::Invite, Leave or Remove (MeshControl.h)
  uint8_t about = 0;   // for 'R': the member to take out
  uint32_t counter = 0;
};

struct ChatGroup {
  uint16_t id = 0;      // creator × 256 + count (MeshPacket.h groupId); 0 = free slot
  Members members = 0;  // me included
  bool left = false;    // I left: kept to read the history, no longer written to
  char name[GROUP_NAME] = "";
  GroupKey keys[MAX_GROUP_KEYS];
  Notice notices[MAX_NOTICES];

  // Only members' messages are shown. One who left or was taken out still has
  // the old keys, so their messages are dropped from then on, also ones they
  // wrote before that arrive late (the counter they carry proves nothing).
  bool has(uint8_t member) const { return members & memberBit(member); }
  void setNotice(uint8_t member, char kind, uint8_t about, uint32_t counter);
  // True when counter was my notice to member (now confirmed, so forgotten).
  bool confirm(uint8_t member, uint32_t counter);

  // The newest key, or nullptr.
  const GroupKey* newest() const;
  const uint8_t* key(uint8_t number) const;
  // Adds or replaces a key number. False when nothing changed.
  bool addKey(uint8_t number, uint8_t from, const uint8_t key[KEY_SIZE]);
};

class ChatGroups {
 public:
  void load();
  // False when the SD card could not be written (logged).
  bool save();
  // The slot (0..MAX_GROUPS-1) of a group id, or -1.
  int find(uint16_t id) const;
  // A free slot for a new id (the oldest left group is reused when full), or -1.
  int take(uint16_t id);
  ChatGroup& at(int slot) { return groups_[slot]; }
  const ChatGroup& at(int slot) const { return groups_[slot]; }
  // The next id for a group made on this unit, or 0 when none is free.
  uint16_t nextId(uint8_t me);

 private:
  ChatGroup groups_[MAX_GROUPS];
};

// The last few sender keys of each other member.
class SenderKeys {
 public:
  static constexpr int KEPT = 3;
  void load();
  bool save();
  const uint8_t* key(uint8_t member, uint8_t number) const;
  // False when nothing changed.
  bool add(uint8_t member, uint8_t number, const uint8_t key[KEY_SIZE]);
  void forget(uint8_t member);

 private:
  struct Kept {
    uint8_t number = 0;
    uint8_t key[KEY_SIZE] = {};
  };
  Kept keys_[MAX_MEMBERS][KEPT];
};

}  // namespace mesh
