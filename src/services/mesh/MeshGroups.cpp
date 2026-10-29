#include "MeshGroups.h"

#include <Logging.h>
#include <Preferences.h>

#include <cstring>
#include <vector>

#include "MeshConfig.h"

namespace mesh {

namespace {

constexpr const char* GROUPS_PATH = "/.mesh/groups.bin";
constexpr const char* SENDERS_PATH = "/.mesh/senders.bin";
constexpr uint8_t RECORD_VERSION = 4;

template <typename T>
bool saveRecord(const char* path, const T& data) {
  std::vector<uint8_t> bytes(1 + sizeof(T));
  bytes[0] = RECORD_VERSION;
  memcpy(bytes.data() + 1, &data, sizeof(T));
  return saveSealed(path, bytes);
}

template <typename T>
bool loadRecord(const char* path, T& data) {
  std::vector<uint8_t> bytes;
  if (!loadSealed(path, bytes) || bytes.size() != 1 + sizeof(T) || bytes[0] != RECORD_VERSION) return false;
  memcpy(&data, bytes.data() + 1, sizeof(T));
  return true;
}

}  // namespace

// --- One group ----------------------------------------------------------------

void ChatGroup::setNotice(const uint8_t member, const char kind, const uint8_t about, const uint32_t counter) {
  Notice* slot = nullptr;
  for (auto& n : notices) {
    if ((n.member == member && n.kind == uint8_t(kind)) || (!slot && !n.member)) slot = &n;
  }
  if (!slot) slot = &notices[0];
  *slot = Notice{member, uint8_t(kind), about, counter};
}

bool ChatGroup::confirm(const uint8_t member, const uint32_t counter) {
  for (auto& n : notices) {
    if (n.member == member && n.counter == counter) {
      n = Notice{};
      return true;
    }
  }
  return false;
}

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

// --- All groups -----------------------------------------------------------------

void ChatGroups::load() {
  for (auto& g : groups_) g = ChatGroup{};
  loadRecord(GROUPS_PATH, groups_);
}

bool ChatGroups::save() {
  const bool ok = saveRecord(GROUPS_PATH, groups_);
  if (!ok) LOG_ERR("MESH", "Could not save the groups");
  return ok;
}

int ChatGroups::find(const uint16_t id) const {
  for (int i = 0; i < MAX_GROUPS; i++) {
    if (id && groups_[i].id == id) return i;
  }
  return -1;
}

int ChatGroups::take(const uint16_t id) {
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

uint16_t ChatGroups::nextId(const uint8_t me) {
  Preferences prefs;
  prefs.begin("meshgroups", false);
  uint32_t count = prefs.getUInt("next", 0);
  // 256 per member; past that, the id of a group I have left is used again
  // (its invitations say "new group", so the others start it afresh).
  uint16_t id = 0;
  for (int tries = 0; tries < 256 && !id; tries++, count++) {
    const uint16_t candidate = groupId(me, uint8_t(count & 0xFF));
    const int slot = find(candidate);
    if (slot < 0 || groups_[slot].left) id = candidate;
  }
  prefs.putUInt("next", count);
  prefs.end();
  return id;
}

// --- Sender keys ----------------------------------------------------------------

void SenderKeys::load() {
  for (auto& member : keys_) {
    for (auto& k : member) k = Kept{};
  }
  loadRecord(SENDERS_PATH, keys_);
}

bool SenderKeys::save() { return saveRecord(SENDERS_PATH, keys_); }

const uint8_t* SenderKeys::key(const uint8_t member, const uint8_t number) const {
  if (!isMember(member) || !number) return nullptr;
  for (const auto& k : keys_[member - 1]) {
    if (k.number == number) return k.key;
  }
  return nullptr;
}

bool SenderKeys::add(const uint8_t member, const uint8_t number, const uint8_t key[KEY_SIZE]) {
  if (!isMember(member) || !number) return false;
  Kept* slot = nullptr;
  for (auto& k : keys_[member - 1]) {
    if (k.number == number) {
      if (memcmp(k.key, key, KEY_SIZE) == 0) return false;
      slot = &k;  // a member who joined again under the same number
    }
  }
  if (!slot) {
    for (auto& k : keys_[member - 1]) {
      if (!slot || k.number < slot->number) slot = &k;
    }
  }
  slot->number = number;
  memcpy(slot->key, key, KEY_SIZE);
  return true;
}

void SenderKeys::forget(const uint8_t member) {
  if (!isMember(member)) return;
  for (auto& k : keys_[member - 1]) k = Kept{};
}

}  // namespace mesh
