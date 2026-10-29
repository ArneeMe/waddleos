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

Members Network::everyone() const {
  Members bits = 0;
  for (uint8_t n = 1; n <= MAX_MEMBERS; n++) {
    if (members[n - 1].present) bits |= memberBit(n);
  }
  return bits;
}

const char* Network::name(const uint8_t number) const {
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
  id.senderKeyNumber = uint8_t(prefs.getUInt("senderNum", 1));
  prefs.end();
  publicKey(id.secret, id.publicKey);
  loaded = true;
  return id;
}

void newIdentity() {
  identity();
  esp_fill_random(id.secret, KEY_SIZE);
  publicKey(id.secret, id.publicKey);
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBytes("secret", id.secret, KEY_SIZE);
  prefs.end();
  newSenderKey();
  LOG_INF("MESH", "New identity: %s", toHex(id.publicKey, 8).c_str());
}

void adoptSenderKey(const uint8_t number, const uint8_t key[KEY_SIZE]) {
  identity();
  memcpy(id.senderKey, key, KEY_SIZE);
  id.senderKeyNumber = number;
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBytes("sender", id.senderKey, KEY_SIZE);
  prefs.putUInt("senderNum", id.senderKeyNumber);
  prefs.end();
}

bool hasTwin() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  const bool twin = prefs.getUInt("twin", 0) != 0;
  prefs.end();
  return twin;
}

void setHasTwin(const bool twin) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUInt("twin", twin ? 1 : 0);
  prefs.end();
}

void newSenderKey() {
  identity();
  esp_fill_random(id.senderKey, KEY_SIZE);
  id.senderKeyNumber = uint8_t(id.senderKeyNumber + 1 ? id.senderKeyNumber + 1 : 1);
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBytes("sender", id.senderKey, KEY_SIZE);
  prefs.putUInt("senderNum", id.senderKeyNumber);
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
// Its own random secret in flash, not the identity's: a new identity
// (newIdentity) keeps the chats and files readable.
void storageKey(uint8_t key[KEY_SIZE]) {
  static uint8_t secret[KEY_SIZE];
  static bool loaded = false;
  if (!loaded) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    if (prefs.getBytes("storage", secret, KEY_SIZE) != KEY_SIZE || isZero(secret, KEY_SIZE)) {
      esp_fill_random(secret, KEY_SIZE);
      prefs.putBytes("storage", secret, KEY_SIZE);
    }
    prefs.end();
    loaded = true;
  }
  static const char LABEL[] = "waddle-storage";
  uint8_t input[KEY_SIZE + sizeof(LABEL)];
  memcpy(input, secret, KEY_SIZE);
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
  const std::string tmp = std::string(path) + ".tmp";
  HalFile f = Storage.open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) {
    LOG_ERR("MESH", "Could not write %s", path);
    return false;
  }
  const bool written = f.write(out.data(), out.size()) == out.size();
  if (!f.close() || !written || !Storage.replaceFile(tmp.c_str(), path)) {
    LOG_ERR("MESH", "Could not write %s; the old one stays", path);
    Storage.remove(tmp.c_str());
    return false;
  }
  return true;
}

namespace {
bool openSealedFile(const char* path, std::vector<uint8_t>& plain) {
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
}  // namespace

bool loadSealed(const char* path, std::vector<uint8_t>& plain) {
  if (openSealedFile(path, plain)) return true;
  // A power cut between removing the old file and renaming the new one leaves
  // only path.tmp; it opened, so it is whole.
  const std::string tmp = std::string(path) + ".tmp";
  if (!Storage.exists(tmp.c_str()) || !openSealedFile(tmp.c_str(), plain)) return false;
  Storage.remove(path);
  Storage.replaceFile(tmp.c_str(), path);
  return true;
}

bool appendSealed(const char* path, const uint8_t* plain, const size_t len) {
  const size_t sealed = 12 + len + TAG_SIZE;
  if (sealed > MAX_SEALED_RECORD) return false;
  std::vector<uint8_t> out(2 + sealed);
  out[0] = uint8_t(sealed);
  out[1] = uint8_t(sealed >> 8);
  uint8_t* nonce = out.data() + 2;
  esp_fill_random(nonce, 12);
  uint8_t key[KEY_SIZE];
  storageKey(key);
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key, nonce);
  crypto_aead_write(&ctx, nonce + 12, nonce + 12 + len, nullptr, 0, plain, len);
  crypto_wipe(&ctx, sizeof(ctx));
  crypto_wipe(key, sizeof(key));
  HalFile f = Storage.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) {
    LOG_ERR("MESH", "Could not write %s", path);
    return false;
  }
  const bool ok = f.write(out.data(), out.size()) == out.size();
  f.close();
  return ok;
}

SealedLog::SealedLog(const char* path) : file_(Storage.open(path, O_RDONLY)) { storageKey(key_); }

SealedLog::~SealedLog() {
  crypto_wipe(key_, sizeof(key_));
  file_.close();
}

bool SealedLog::next(std::vector<uint8_t>& plain) {
  if (!file_) return false;
  std::vector<uint8_t> data;
  for (;;) {
    uint8_t head[2];
    if (file_.read(head, 2) != 2) return false;
    const size_t sealed = size_t(head[0]) | size_t(head[1]) << 8;
    // Longer than any record: not a record start (cut short before it).
    if (sealed < 12 + TAG_SIZE || sealed > MAX_SEALED_RECORD) return false;
    data.resize(sealed);
    if (file_.read(data.data(), sealed) != int(sealed)) return false;
    goodEnd_ += 2 + sealed;
    plain.assign(sealed - 12 - TAG_SIZE, 0);
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key_, data.data());
    const int bad = crypto_aead_read(&ctx, plain.data(), data.data() + sealed - TAG_SIZE, nullptr, 0,
                                     data.data() + 12, plain.size());
    crypto_wipe(&ctx, sizeof(ctx));
    if (!bad) return true;
    // A damaged record (the card changed a bit): skip it, keep the rest.
  }
}

void repairSealedLog(const char* path) {
  size_t size = 0, good = 0;
  {
    SealedLog log(path);
    if (!log) return;
    std::vector<uint8_t> plain;
    while (log.next(plain)) {
    }
    size = log.fileSize();
    good = log.goodEnd();
  }
  if (good >= size) return;
  LOG_ERR("MESH", "%s: cut off %u bytes left half-written", path, unsigned(size - good));
  HalFile f = Storage.open(path, O_RDWR);
  if (f) {
    f.truncate(good);
    f.close();
  }
}

// --- The network ----------------------------------------------------------------

namespace {

// The newest roster this unit took, (network << 16) | version, in flash: a
// copied-back older roster.bin (with someone removed still in it) is refused.
uint32_t newestRoster() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  const uint32_t v = prefs.getUInt("rosterVer", 0);
  prefs.end();
  return v;
}

void saveNewestRoster(const uint8_t network, const uint16_t version) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUInt("rosterVer", uint32_t(network) << 16 | version);
  prefs.end();
}

// Everything of an earlier network: groups, keys, chats, positions.
void forgetNetworkFiles() {
  for (const char* dir : {"/.mesh/chat", "/.mesh/packets"}) {
    for (const auto& name : Storage.listFiles(dir, 500)) {
      Storage.remove((std::string(dir) + "/" + name.c_str()).c_str());
    }
  }
  for (const char* path : {"/.mesh/groups.bin", "/.mesh/senders.bin", "/.mesh/positions.sealed", "/.mesh/read.bin",
                           "/.mesh/last.bin"}) {
    Storage.remove(path);
  }
}

// Fills group.members from a checked roster and derives the DM keys.
void takeMembers(Network& group, const Roster& roster) {
  const Identity& mine = identity();
  for (auto& m : group.members) m = Member{};
  for (const auto& e : roster.members) {
    Member* m = group.member(e.number);
    if (!m) continue;
    m->present = true;
    m->admin = e.admin;
    strlcpy(m->name, e.name.c_str(), sizeof(m->name));
    memcpy(m->publicKey, e.publicKey, KEY_SIZE);
    dmKey(mine.secret, mine.publicKey, m->publicKey, m->dmKey);
  }
  group.rosterVersion = roster.version;
}

// The network part of a welcome (everything but the roster).
const char* parseNetwork(const JsonDocument& doc, Network& group) {
  group = Network{};
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
  group.phone = doc["phone"] | false;
  return nullptr;
}

const char* takeRoster(Network& group, const uint8_t* data, const size_t len) {
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

bool loadNetwork(Network& group) {
  group = Network{};
  std::vector<uint8_t> network, roster;
  if (!loadSealed(NETWORK_PATH, network) || !loadSealed(ROSTER_PATH, roster)) return false;
  JsonDocument doc;
  const char* err = deserializeJson(doc, network.data(), network.size()) ? "ikke gyldig JSON" : parseNetwork(doc, group);
  if (!err) err = takeRoster(group, roster.data(), roster.size());
  const uint32_t newest = newestRoster();
  if (!err && newest >> 16 == group.id && group.rosterVersion < (newest & 0xFFFF)) err = "en eldre roster enn sist";
  if (err) {
    LOG_ERR("MESH", "network: %s", err);
    group = Network{};
    return false;
  }
  return true;
}

bool applyRoster(Network& group, const uint8_t* data, const size_t len) {
  Roster roster;
  if (!group.configured || !readRoster(data, len, group.adminSign, roster) || roster.network != group.id ||
      roster.version <= group.rosterVersion) {
    return false;
  }
  const RosterEntry* self = roster.member(group.me);
  if (!self || memcmp(self->publicKey, identity().publicKey, KEY_SIZE) != 0) {
    // I was taken out (my number may be someone else's now). The roster
    // without me is saved, so the unit is out after a restart too (loadNetwork);
    // the chats stay, for when it is let back in as the same number.
    LOG_INF("MESH", "This unit is no longer in the network");
    saveSealed(ROSTER_PATH, std::vector<uint8_t>(data, data + len));
    saveNewestRoster(group.id, roster.version);
    // Taken out because it, or a phone with its keys, may be in other hands:
    // new keys, so a copy of the old ones opens nothing sent to it later.
    newIdentity();
    group.configured = false;
    return true;
  }
  if (!saveSealed(ROSTER_PATH, std::vector<uint8_t>(data, data + len))) return false;
  saveNewestRoster(group.id, roster.version);
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
  // Only a unit outside any network joins: someone on the Wi-Fi cannot move a
  // unit that is in one to another network.
  Network current;
  if (loadNetwork(current)) return "enheten er allerede med i et nettverk";
  if (name.empty() || name.size() > ROSTER_NAME) return "navnet må være 1-20 tegn";
  const Identity& mine = identity();
  uint8_t k[KEY_SIZE];
  joinKey(mine.secret, mine.publicKey, inv.adminBox, inv.password, k);
  JsonDocument doc;
  doc["name"] = name;
  std::string json;
  serializeJson(doc, json);
  std::vector<uint8_t> box(json.size() + TAG_SIZE);
  sealBox(k, 3, reinterpret_cast<const uint8_t*>(json.data()), json.size(), box.data());
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
  const bool opened = openBox(k, 4, box.data(), len, plain.data());
  crypto_wipe(k, sizeof(k));
  if (!opened) return "velkomsten kunne ikke åpnes (feil invitasjon?)";
  JsonDocument doc;
  if (deserializeJson(doc, plain.data(), plain.size())) return "ikke gyldig JSON";
  Network check;
  if (const char* err = parseNetwork(doc, check)) return err;
  if (check.id != inv.network || memcmp(check.adminBox, inv.adminBox, KEY_SIZE) != 0) return "feil nettverk";
  const char* rosterHex = doc["roster"] | "";
  std::vector<uint8_t> roster(strlen(rosterHex) / 2);
  if (roster.empty() || !fromHex(rosterHex, roster.data(), roster.size())) return "rosteren mangler";
  if (const char* err = takeRoster(check, roster.data(), roster.size())) return err;
  // Another network, or another number in it: the old chats and groups would
  // show under the wrong people. Back in the same network as the same number
  // (after being taken out): the history stays.
  std::vector<uint8_t> before;
  JsonDocument old;
  Network previous;
  const bool same = loadSealed(NETWORK_PATH, before) && !deserializeJson(old, before.data(), before.size()) &&
                    !parseNetwork(old, previous) && previous.id == check.id && previous.me == check.me &&
                    memcmp(previous.adminSign, check.adminSign, SIGN_KEY_SIZE) == 0;
  if (!same) forgetNetworkFiles();
  // The others forgot my old sender key when I left (or never had it).
  newSenderKey();
  saveNewestRoster(check.id, check.rosterVersion);
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
