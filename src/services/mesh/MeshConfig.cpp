#include "MeshConfig.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_random.h>

#include <cstring>

namespace mesh {

namespace {

constexpr const char* NVS_NAMESPACE = "waddlemesh";

bool isZero(const uint8_t* data, const size_t len) {
  uint8_t any = 0;
  for (size_t i = 0; i < len; i++) any |= data[i];
  return any == 0;
}

}  // namespace

const char* Group::name(const uint8_t number) const {
  if (number == SERVER) return "Server";
  const Member* m = member(number);
  return m && m->present && m->name[0] ? m->name : "?";
}

std::string toHex(const uint8_t* data, const size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    s += digits[data[i] >> 4];
    s += digits[data[i] & 15];
  }
  return s;
}

bool fromHex(const char* hex, uint8_t* out, const size_t len) {
  if (!hex || strlen(hex) != len * 2) return false;
  auto nibble = [](const char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < len; i++) {
    const int hi = nibble(hex[2 * i]);
    const int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = uint8_t(hi << 4 | lo);
  }
  return true;
}

const Identity& identity() {
  static Identity id;
  static bool loaded = false;
  if (loaded) return id;
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  const bool have = prefs.getBytes("secret", id.secret, KEY_SIZE) == KEY_SIZE &&
                    prefs.getBytes("sender", id.senderKey, KEY_SIZE) == KEY_SIZE && !isZero(id.secret, KEY_SIZE);
  if (!have) {
    esp_fill_random(id.secret, KEY_SIZE);
    esp_fill_random(id.senderKey, KEY_SIZE);
    prefs.putBytes("secret", id.secret, KEY_SIZE);
    prefs.putBytes("sender", id.senderKey, KEY_SIZE);
    LOG_INF("MESH", "Made new keys for this unit");
  }
  prefs.end();
  publicKey(id.secret, id.publicKey);
  loaded = true;
  return id;
}

uint32_t savedCounterLimit() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  const uint32_t limit = prefs.getUInt("counter", 0);
  prefs.end();
  return limit;
}

void saveCounterLimit(const uint32_t limit) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUInt("counter", limit);
  prefs.end();
}

namespace {

// Fills group from parsed JSON. Returns an error text, or nullptr.
const char* parseGroup(const JsonDocument& doc, Group& group) {
  group = Group{};
  const int id = doc["group"] | -1;
  const int me = doc["me"] | -1;
  if (id < 0 || id > 255) return "group mangler";
  if (!isMember(uint8_t(me))) return "me må være 1-4";
  group.id = uint8_t(id);
  group.me = uint8_t(me);
  const Identity& mine = identity();
  int count = 0;
  for (JsonObjectConst m : doc["members"].as<JsonArrayConst>()) {
    const int number = m["number"] | -1;
    Member* member = group.member(uint8_t(number));
    if (!member) return "medlem må ha number 1-4";
    member->present = true;
    strlcpy(member->name, m["name"] | "", sizeof(member->name));
    if (!fromHex(m["publicKey"] | "", member->publicKey, KEY_SIZE)) return "publicKey er feil";
    if (!fromHex(m["senderKey"] | "", member->senderKey, KEY_SIZE)) return "senderKey er feil";
    count++;
  }
  Member* self = group.member(group.me);
  if (!self->present) return "jeg er ikke med i gruppen";
  if (memcmp(self->publicKey, mine.publicKey, KEY_SIZE) != 0 || memcmp(self->senderKey, mine.senderKey, KEY_SIZE) != 0)
    return "nøklene mine stemmer ikke (denne gruppen er laget for en annen enhet)";
  if (count < 2) return "gruppen trenger minst 2 medlemmer";
  for (auto& m : group.members) {
    if (m.present && &m != self) pairKey(mine.secret, mine.publicKey, m.publicKey, m.pairKey);
  }
  for (JsonObjectConst g : doc["groups"].as<JsonArrayConst>()) {
    uint8_t bits = 0;
    for (JsonVariantConst n : g["members"].as<JsonArrayConst>()) bits |= memberBit(n.as<uint8_t>());
    if (!isSmallGroup(SMALL_GROUP | bits) || !(bits & memberBit(group.me))) return "en gruppe er feil";
    if (!fromHex(g["key"] | "", group.smallGroupKeys[bits], KEY_SIZE)) return "gruppenøkkel er feil";
    group.smallGroups |= uint16_t(1u << bits);
  }
  group.serverUrl = doc["server"]["url"] | "";
  group.serverToken = doc["server"]["token"] | "";
  group.serverCert = doc["server"]["cert"] | "";
  group.configured = true;
  return nullptr;
}

}  // namespace

bool loadGroup(Group& group) {
  group = Group{};
  if (!Storage.exists(GROUP_PATH)) return false;
  const String text = Storage.readFile(GROUP_PATH);
  JsonDocument doc;
  if (deserializeJson(doc, text.c_str(), text.length())) {
    LOG_ERR("MESH", "group.json is not valid JSON");
    return false;
  }
  if (const char* err = parseGroup(doc, group)) {
    LOG_ERR("MESH", "group.json: %s", err);
    group = Group{};
    return false;
  }
  return true;
}

const char* saveGroupJson(const char* json, const size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return "ikke gyldig JSON";
  Group check;
  if (const char* err = parseGroup(doc, check)) return err;
  Storage.mkdir("/.mesh");
  if (!Storage.writeFile(GROUP_PATH, String(json, len))) return "kunne ikke skrive til SD-kortet";
  return nullptr;
}

}  // namespace mesh
