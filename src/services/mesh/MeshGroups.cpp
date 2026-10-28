#include "MeshGroups.h"

#include <Logging.h>
#include <Preferences.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace mesh {

namespace {

constexpr const char* NVS_NAMESPACE = "meshgroups";
constexpr uint8_t RECORD_VERSION = 2;

std::string slotKey(const int slot) {
  char k[8];
  snprintf(k, sizeof(k), "g%d", slot);
  return k;
}

}  // namespace

const GroupKey* ChatGroup::newest() const {
  const GroupKey* best = nullptr;
  for (const auto& k : keys) {
    if (k.number && (!best || k.number > best->number)) best = &k;
  }
  return best;
}

const uint8_t* ChatGroup::key(const uint8_t number) const {
  for (const auto& k : keys) {
    if (number && k.number == number) return k.key;
  }
  return nullptr;
}

bool ChatGroup::addKey(const uint8_t number, const uint8_t from, const uint8_t key[KEY_SIZE]) {
  if (!number) return false;
  GroupKey* slot = nullptr;
  for (auto& k : keys) {
    if (k.number == number) {
      // Two members made the same key number at once: the lower member's wins.
      if (memcmp(k.key, key, KEY_SIZE) == 0 || from >= k.from) return false;
      slot = &k;
    }
  }
  if (!slot) {
    for (auto& k : keys) {
      if (!slot || k.number < slot->number) slot = &k;  // a free one (0), or the oldest
    }
  }
  slot->number = number;
  slot->from = from;
  memcpy(slot->key, key, KEY_SIZE);
  return true;
}

void ChatGroups::load() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  for (int i = 0; i < MAX_GROUPS; i++) {
    groups_[i] = ChatGroup{};
    uint8_t record[1 + sizeof(ChatGroup)];
    if (prefs.getBytes(slotKey(i).c_str(), record, sizeof(record)) == sizeof(record) && record[0] == RECORD_VERSION) {
      memcpy(&groups_[i], record + 1, sizeof(ChatGroup));
    }
  }
  prefs.end();
}

bool ChatGroups::save(const int slot) {
  uint8_t record[1 + sizeof(ChatGroup)];
  record[0] = RECORD_VERSION;
  memcpy(record + 1, &groups_[slot], sizeof(ChatGroup));
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  const bool ok = prefs.putBytes(slotKey(slot).c_str(), record, sizeof(record)) == sizeof(record);
  prefs.end();
  if (!ok) LOG_ERR("MESH", "Could not save group %02x: flash full?", groups_[slot].id);
  return ok;
}

int ChatGroups::find(const uint8_t id) const {
  for (int i = 0; i < MAX_GROUPS; i++) {
    if (id && groups_[i].id == id) return i;
  }
  return -1;
}

int ChatGroups::take(const uint8_t id) {
  if (const int existing = find(id); existing >= 0) return existing;
  int slot = -1;
  for (int i = 0; i < MAX_GROUPS && slot < 0; i++) {
    if (!groups_[i].id) slot = i;
  }
  for (int i = 0; i < MAX_GROUPS && slot < 0; i++) {
    if (groups_[i].left) slot = i;
  }
  if (slot < 0) return -1;
  groups_[slot] = ChatGroup{};
  groups_[slot].id = id;
  return slot;
}

uint8_t ChatGroups::nextId(const uint8_t me) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  uint32_t count = prefs.getUInt("next", 0);
  // 16 per member: past that, the id of a group I have left is used again (its
  // invitations say "new group", so the others start it afresh).
  uint8_t id = 0;
  for (int tries = 0; tries < 16 && !id; tries++, count++) {
    const uint8_t candidate = groupId(me, uint8_t(count & 15));
    const int slot = find(candidate);
    if (slot < 0 || groups_[slot].left) id = candidate;
  }
  prefs.putUInt("next", count);
  prefs.end();
  return id;
}

}  // namespace mesh
