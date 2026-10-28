#include "MeshConfig.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_random.h>
#include <monocypher.h>

#include <cstring>

namespace mesh {

namespace {

constexpr const char* NVS_NAMESPACE = "waddlemesh";
constexpr const char* NETWORK_PATH = "/.mesh/network.bin";
constexpr const char* ROSTER_PATH = "/.mesh/roster.bin";
constexpr const char* JOIN_PATH = "/.mesh/join.bin";
// From before the network (one fixed group of four, protocol version 2): unused now.
constexpr const char* OLD_GROUP_PATH = "/.mesh/group.json";
constexpr const char* LINK_PREFIX = "waddle://join/";
constexpr size_t LINK_FIXED = 1 + 8 + PASSWORD_SIZE + KEY_SIZE + 32;  // then the server address

bool isZero(const uint8_t* data, const size_t len) {
  uint8_t any = 0;
  for (size_t i = 0; i < len; i++) any |= data[i];
  return any == 0;
}

Identity id;
bool loaded = false;

}  // namespace

Members Group::everyone() const {
  Members bits = 0;
  for (uint8_t n = 1; n <= MAX_MEMBERS; n++) {
    if (members[n - 1].present) bits |= memberBit(n);
  }
  return bits;
}

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

std::vector<uint8_t> fromBase64Url(const std::string& text) {
  std::vector<uint8_t> out;
  uint32_t bits = 0;
  int count = 0;
  for (const char c : text) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '-' || c == '+') v = 62;
    else if (c == '_' || c == '/') v = 63;
    else if (c == '=') break;
    else return {};
    bits = (bits << 6) | uint32_t(v);
    count += 6;
    if (count >= 8) {
      count -= 8;
      out.push_back(uint8_t(bits >> count));
    }
  }
  return out;
}

// --- Keys in flash --------------------------------------------------------------

const Identity& identity() {
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
    prefs.putUInt("senderNum", 1);
    LOG_INF("MESH", "Made new keys for this unit");
  }
  id.senderNumber = uint8_t(prefs.getUInt("senderNum", 1));
  prefs.end();
  publicKey(id.secret, id.publicKey);
  loaded = true;
  return id;
}

void newSenderKey() {
  identity();
  esp_fill_random(id.senderKey, KEY_SIZE);
  id.senderNumber = uint8_t(id.senderNumber + 1 ? id.senderNumber + 1 : 1);
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBytes("sender", id.senderKey, KEY_SIZE);
  prefs.putUInt("senderNum", id.senderNumber);
  prefs.end();
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

// --- Sealed files on the SD card ------------------------------------------------

namespace {
void storageKey(uint8_t key[KEY_SIZE]) {
  static const char LABEL[] = "waddle-storage";
  uint8_t input[KEY_SIZE + sizeof(LABEL)];
  memcpy(input, identity().secret, KEY_SIZE);
  memcpy(input + KEY_SIZE, LABEL, sizeof(LABEL));
  crypto_blake2b(key, KEY_SIZE, input, sizeof(input));
  crypto_wipe(input, sizeof(input));
}
}  // namespace

bool saveSealed(const char* path, const std::vector<uint8_t>& plain) {
  // [nonce 12][ciphertext][tag 16]; a fresh nonce every time the file is written.
  std::vector<uint8_t> out(12 + plain.size() + TAG_SIZE);
  esp_fill_random(out.data(), 12);
  uint8_t key[KEY_SIZE];
  storageKey(key);
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key, out.data());
  crypto_aead_write(&ctx, out.data() + 12, out.data() + 12 + plain.size(), nullptr, 0, plain.data(), plain.size());
  crypto_wipe(&ctx, sizeof(ctx));
  crypto_wipe(key, sizeof(key));
  Storage.mkdir("/.mesh");
  HalFile f = Storage.open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return false;
  const bool ok = f.write(out.data(), out.size()) == out.size();
  f.close();
  if (!ok) LOG_ERR("MESH", "Could not write %s", path);
  return ok;
}

bool loadSealed(const char* path, std::vector<uint8_t>& plain) {
  HalFile f = Storage.open(path, O_RDONLY);
  if (!f) return false;
  std::vector<uint8_t> data(f.fileSize());
  const bool read = data.size() >= 12 + TAG_SIZE && f.read(data.data(), data.size()) == int(data.size());
  f.close();
  if (!read) return false;
  plain.assign(data.size() - 12 - TAG_SIZE, 0);
  uint8_t key[KEY_SIZE];
  storageKey(key);
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key, data.data());
  const int bad = crypto_aead_read(&ctx, plain.data(), data.data() + data.size() - TAG_SIZE, nullptr, 0,
                                   data.data() + 12, plain.size());
  crypto_wipe(&ctx, sizeof(ctx));
  crypto_wipe(key, sizeof(key));
  if (bad) LOG_ERR("MESH", "%s did not open (another unit's card?)", path);
  return !bad;
}

// --- The network ----------------------------------------------------------------

namespace {

// Fills group.members from a checked roster and derives the DM keys.
void takeMembers(Group& group, const Roster& roster) {
  const Identity& mine = identity();
  for (auto& m : group.members) m = Member{};
  for (const auto& e : roster.members) {
    Member* m = group.member(e.number);
    if (!m) continue;
    m->present = true;
    m->admin = e.admin;
    strlcpy(m->name, e.name.c_str(), sizeof(m->name));
    memcpy(m->publicKey, e.publicKey, KEY_SIZE);
    if (e.number != group.me) pairKey(mine.secret, mine.publicKey, m->publicKey, m->pairKey);
  }
  group.rosterVersion = roster.version;
}

// The network part of a welcome (everything but the roster).
const char* parseNetwork(const JsonDocument& doc, Group& group) {
  group = Group{};
  const int network = doc["network"] | -1;
  const int me = doc["me"] | -1;
  if (network < 0 || network > 255) return "network mangler";
  if (!isMember(uint8_t(me))) return "me må være 1-32";
  group.id = uint8_t(network);
  group.me = uint8_t(me);
  if (!fromHex(doc["networkKey"] | "", group.networkKey, KEY_SIZE)) return "networkKey er feil";
  if (!fromHex(doc["adminSign"] | "", group.adminSign, SIGN_KEY_SIZE)) return "adminSign er feil";
  if (!fromHex(doc["adminBox"] | "", group.adminBox, KEY_SIZE)) return "adminBox er feil";
  group.serverUrl = doc["server"]["url"] | "";
  group.serverToken = doc["server"]["token"] | "";
  group.serverCert = doc["server"]["cert"] | "";
  group.counterFloor = doc["counterFloor"] | 0u;
  return nullptr;
}

const char* takeRoster(Group& group, const uint8_t* data, const size_t len) {
  Roster roster;
  if (!readRoster(data, len, group.adminSign, roster)) return "rosteren er ikke signert av admin";
  if (roster.network != group.id) return "rosteren er for et annet nettverk";
  const RosterEntry* self = roster.member(group.me);
  if (!self || memcmp(self->publicKey, identity().publicKey, KEY_SIZE) != 0) return "jeg er ikke i rosteren";
  takeMembers(group, roster);
  group.roster.assign(data, data + len);
  group.configured = true;
  return nullptr;
}

}  // namespace

bool loadGroup(Group& group) {
  group = Group{};
  if (Storage.exists(OLD_GROUP_PATH)) {
    Storage.remove(OLD_GROUP_PATH);
    LOG_INF("MESH", "Removed the old group setup: join the network with `waddle mesh join`");
  }
  std::vector<uint8_t> network, roster;
  if (!loadSealed(NETWORK_PATH, network) || !loadSealed(ROSTER_PATH, roster)) return false;
  JsonDocument doc;
  const char* err = deserializeJson(doc, network.data(), network.size()) ? "ikke gyldig JSON" : parseNetwork(doc, group);
  if (!err) err = takeRoster(group, roster.data(), roster.size());
  if (err) {
    LOG_ERR("MESH", "network: %s", err);
    group = Group{};
    return false;
  }
  return true;
}

bool applyRoster(Group& group, const uint8_t* data, const size_t len) {
  Roster roster;
  if (!group.configured || !readRoster(data, len, group.adminSign, roster) || roster.network != group.id ||
      roster.version <= group.rosterVersion) {
    return false;
  }
  if (!roster.member(group.me)) {
    // I was taken out: keep the old roster, so the history still has names.
    LOG_INF("MESH", "This unit is no longer in the network");
    group.configured = false;
    return true;
  }
  if (!saveSealed(ROSTER_PATH, std::vector<uint8_t>(data, data + len))) return false;
  takeMembers(group, roster);
  group.roster.assign(data, data + len);
  LOG_INF("MESH", "Roster version %u: %u members", roster.version, unsigned(roster.members.size()));
  return true;
}

// --- Joining --------------------------------------------------------------------

namespace {
struct Invitation {
  uint8_t network = 0;
  uint8_t id[8] = {};
  uint8_t password[PASSWORD_SIZE] = {};
  uint8_t adminBox[KEY_SIZE] = {};
};

bool readLink(const std::string& link, Invitation& inv) {
  if (link.compare(0, strlen(LINK_PREFIX), LINK_PREFIX) != 0) return false;
  const auto bytes = fromBase64Url(link.substr(strlen(LINK_PREFIX)));
  if (bytes.size() <= LINK_FIXED) return false;
  inv.network = bytes[0];
  memcpy(inv.id, &bytes[1], 8);
  memcpy(inv.password, &bytes[9], PASSWORD_SIZE);
  memcpy(inv.adminBox, &bytes[9 + PASSWORD_SIZE], KEY_SIZE);
  return true;
}
}  // namespace

const char* startJoin(const std::string& link, const std::string& name, std::string& boxHex, char code[8]) {
  Invitation inv;
  if (!readLink(link, inv)) return "lenken er ikke en invitasjon";
  if (name.empty() || name.size() > ROSTER_NAME) return "navnet må være 1-20 tegn";
  const Identity& mine = identity();
  uint8_t k[KEY_SIZE];
  joinKey(mine.secret, mine.publicKey, inv.adminBox, inv.password, k);
  JsonDocument doc;
  doc["name"] = name;
  std::string json;
  serializeJson(doc, json);
  std::vector<uint8_t> box(json.size() + TAG_SIZE);
  setupSeal(k, 3, reinterpret_cast<const uint8_t*>(json.data()), json.size(), box.data());
  crypto_wipe(k, sizeof(k));
  boxHex = toHex(box.data(), box.size());
  joinCode(inv.adminBox, mine.publicKey, code);
  // Kept until the welcome comes.
  const auto* raw = reinterpret_cast<const uint8_t*>(&inv);
  return saveSealed(JOIN_PATH, std::vector<uint8_t>(raw, raw + sizeof(inv))) ? nullptr : "kunne ikke lagre";
}

const char* finishJoin(const std::string& boxHex) {
  std::vector<uint8_t> kept;
  if (!loadSealed(JOIN_PATH, kept) || kept.size() != sizeof(Invitation)) return "ingen invitasjon på denne enheten";
  Invitation inv;
  memcpy(&inv, kept.data(), sizeof(inv));
  const size_t len = boxHex.size() / 2;
  if (len <= TAG_SIZE || len > 8192) return "feil lengde";
  std::vector<uint8_t> box(len), plain(len - TAG_SIZE);
  if (!fromHex(boxHex.c_str(), box.data(), len)) return "ikke hex";
  const Identity& mine = identity();
  uint8_t k[KEY_SIZE];
  joinKey(mine.secret, mine.publicKey, inv.adminBox, inv.password, k);
  const bool opened = setupOpen(k, 4, box.data(), len, plain.data());
  crypto_wipe(k, sizeof(k));
  if (!opened) return "velkomsten kunne ikke åpnes (feil invitasjon?)";
  JsonDocument doc;
  if (deserializeJson(doc, plain.data(), plain.size())) return "ikke gyldig JSON";
  Group check;
  if (const char* err = parseNetwork(doc, check)) return err;
  if (check.id != inv.network || memcmp(check.adminBox, inv.adminBox, KEY_SIZE) != 0) return "feil nettverk";
  const char* rosterHex = doc["roster"] | "";
  std::vector<uint8_t> roster(strlen(rosterHex) / 2);
  if (roster.empty() || !fromHex(rosterHex, roster.data(), roster.size())) return "rosteren mangler";
  if (const char* err = takeRoster(check, roster.data(), roster.size())) return err;
  // Saved without the roster, which has a file of its own.
  doc.remove("roster");
  std::string network;
  serializeJson(doc, network);
  if (!saveSealed(NETWORK_PATH, std::vector<uint8_t>(network.begin(), network.end())) ||
      !saveSealed(ROSTER_PATH, roster)) {
    return "kunne ikke lagre";
  }
  Storage.remove(JOIN_PATH);
  LOG_INF("MESH", "Joined network %u as member %u", check.id, check.me);
  return nullptr;
}

}  // namespace mesh
