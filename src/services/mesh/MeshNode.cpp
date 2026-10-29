#include "MeshNode.h"

#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Preferences.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <esp_random.h>
#ifdef BOARD_HAS_PSRAM
#include <esp_heap_caps.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>

#include "MeshGps.h"
#include "MeshRadio.h"
#include "MeshSettings.h"
#include "MeshTime.h"
#include "core/network/HttpDownloader.h"
#include "core/waddle/WaddleVersion.h"
#include "core/waddle/WaddleWifi.h"

namespace mesh {

namespace {

constexpr const char* POSITIONS_PATH = "/.mesh/positions.sealed";
constexpr const char* ROSTER_PACKETS_PATH = "/.mesh/rosterpk.bin";
constexpr size_t MAX_OUTBOX = 40;
constexpr size_t MAX_PENDING = 20;
constexpr size_t MAX_SYNC_ANSWER = 8;
constexpr uint32_t RETRY_MS[] = {30000, 120000, 480000};
constexpr uint32_t WIFI_JOIN_MS = 20000;
constexpr uint32_t WIFI_FALLBACK_GAP_MS = 15UL * 60 * 1000;
constexpr uint32_t SERVER_FRESH_MS = 30UL * 60 * 1000;
constexpr uint32_t SOS_REPEAT_MS = 60000;
constexpr uint8_t LOW_BATTERY = 20;
constexpr uint32_t CHECK_LISTEN_MS = 20000;  // how long a check listens for answers
constexpr uint32_t CHECK_WIFI_GAP_MS = 10UL * 60 * 1000;

uint32_t randomBetween(const uint32_t lo, const uint32_t hi) { return lo + esp_random() % (hi - lo + 1); }

uint32_t unixNow() {
  const time_t t = time(nullptr);
  return t > 1600000000 ? uint32_t(t) : 0;
}

uint8_t battery() { return uint8_t(std::min<uint16_t>(powerManager.getBatteryPercentage(), 100)); }

// The first byte of the short git hash, for the server's status page.
uint8_t buildByte() {
  uint8_t b = 0;
  const char* hash = waddle::build();
  return hash && strlen(hash) >= 2 && fromHex(std::string(hash, 2).c_str(), &b, 1) ? b : 0;
}

// "Tue, 27 Oct 2026 18:02:11 GMT" -> unix seconds, or 0.
uint32_t httpDate(const std::string& text) {
  static const char* MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char month[4] = {};
  int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
  if (sscanf(text.c_str(), "%*3s, %d %3s %d %d:%d:%d", &day, month, &year, &hh, &mm, &ss) != 6) return 0;
  const char* found = strstr(MONTHS, month);
  if (!found || year < 2020) return 0;
  const int mon = int(found - MONTHS) / 3 + 1;
  return unixFromCivil(year, mon, day, hh, mm, ss);
}

// Two packets are the same when type, sender, counter and piece match (hops may differ).
bool samePacket(const uint8_t* a, const uint8_t* b) {
  return a[1] == b[1] && a[4] == b[4] && memcmp(a + 6, b + 6, 5) == 0;
}

}  // namespace

Node& Node::get() {
  // About 17 KB with 32 members: in PSRAM, keeping the internal heap free.
#ifdef BOARD_HAS_PSRAM
  static Node* node = [] {
    void* p = heap_caps_malloc(sizeof(Node), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? new (p) Node() : new Node();
  }();
  return *node;
#else
  static Node node;
  return node;
#endif
}

void Node::begin() {
  identity();
  loadGroup(group_);
  groups_.load();
  senders_.load();
  store_.begin(group_.me);
  // Above the floor from joining, so a unit that gets a former member's number
  // never repeats their counters.
  if (group_.configured && group_.counterFloor > savedCounterLimit()) saveCounterLimit(group_.counterFloor);
  counters_ = std::make_unique<CounterReserve>(savedCounterLimit());
  loadPositions();
  pending_.clear();
  if (group_.configured) {
    // My messages nobody confirmed before the restart: try them again.
    for (uint8_t c = 0; c < CONVERSATIONS; c++) {
      if (!canWrite(c)) continue;
      for (const auto& m : store_.load(c, 20)) {
        if (m.sender != group_.me || m.stored || m.deliveredTo) continue;
        if (pending_.size() >= MAX_PENDING) pending_.erase(pending_.begin());
        pending_.push_back(Pending{m.counter, c, 0, millis() + 10000});
      }
    }
  }
  if (!group_.configured) {
    LOG_INF("MESH", "Not in a network yet: `waddle mesh join`. Public key %s", toHex(identity().publicKey, 8).c_str());
    return;
  }
  LOG_INF("MESH", "Member %u of network %u (roster %u)", group_.me, group_.id, group_.rosterVersion);
  dropMissingMembers();
  // Everyone who has not confirmed my sender key gets it (after joining: all).
  loadKeyConfirmations();
  if (radio::begin(params_)) {
    if (MESH_SETTINGS.onTrip) {
      gps::wake();
    } else {
      gps::sleep();
    }
  }
  // On power-on, check for messages (over Wi-Fi too when a network is saved).
  startCheck();
}

void Node::startCheck() {
  if (!group_.configured) return;
  const uint32_t now = millis();
  radio::resume();
  requestSync();
  // At home the server is one Wi-Fi hop away; on a trip Wi-Fi is rarely there.
  // Retries of unanswered messages still try Wi-Fi in both modes.
  if (!MESH_SETTINGS.onTrip && MESH_SETTINGS.homeWifi && !group_.serverUrl.empty() && battery() >= LOW_BATTERY &&
      (!lastWifiSyncMs_ || now - lastWifiSyncMs_ > CHECK_WIFI_GAP_MS)) {
    startWifiSync();
  }
  resendUnconfirmed();
  checkUntilMs_ = now + CHECK_LISTEN_MS;
  const uint32_t interval = MESH_SETTINGS.checkIntervalMs();
  nextCheckMs_ = interval && interval != UINT32_MAX ? now + interval : 0;
  changes_++;
}

bool Node::listensAlways() const {
  if (MESH_SETTINGS.checkIntervalMs() == 0 || sosActive_) return true;
  for (const auto& m : members_) {
    if (m.sos) return true;  // keep hearing a member's SOS until it ends
  }
  return false;
}

uint32_t Node::msUntilCheck() const {
  const uint32_t interval = MESH_SETTINGS.checkIntervalMs();
  if (interval == UINT32_MAX) return UINT32_MAX;
  if (!nextCheckMs_) return interval;
  const int32_t left = int32_t(nextCheckMs_ - millis());
  return left > 0 ? uint32_t(left) : 0;
}

uint32_t Node::nextCounter() {
  uint32_t save = 0;
  const uint32_t c = counters_->take(&save);
  if (save) saveCounterLimit(save);
  return c;
}

Header Node::header(const Type type, const uint8_t recipient) {
  Header h;
  h.type = type;
  h.group = group_.id;
  h.sender = group_.me;
  h.recipient = recipient;
  h.counter = nextCounter();
  return h;
}

void Node::queue(const uint8_t* raw, const size_t len, const uint32_t delayMs, const bool cancelIfHeard) {
  if (outbox_.size() >= MAX_OUTBOX) {
    // Drop a relay first, then an ACK (they come again); never the user's own packets.
    auto drop = std::find_if(outbox_.begin(), outbox_.end(), [](const Outgoing& o) { return o.cancelIfHeard; });
    if (drop == outbox_.end()) {
      drop = std::find_if(outbox_.begin(), outbox_.end(),
                          [](const Outgoing& o) { return (o.raw[1] & 0x0F) == uint8_t(Type::Ack); });
    }
    if (drop == outbox_.end()) return;
    outbox_.erase(drop);
  }
  outbox_.push_back(Outgoing{std::vector<uint8_t>(raw, raw + len), millis() + delayMs, cancelIfHeard});
}

void Node::sendSealed(const Header& h, const uint8_t* key, const uint8_t* plain, const size_t len, const bool store,
                      const uint32_t delayMs) {
  auto sealed = std::make_unique<Sealed>();
  if (!seal(h, key, plain, len, *sealed)) return;
  for (int i = 0; i < sealed->count; i++) {
    Header piece;
    readHeader(sealed->packets[i], sealed->lengths[i], piece);
    seen_.remember(piece);  // never relay our own packet back
    if (store) store_.savePacket(group_.me, sealed->packets[i], sealed->lengths[i]);
    queue(sealed->packets[i], sealed->lengths[i], delayMs, false);
  }
  if (store) store_.confirmCounter(group_.me, h.counter);
}

void Node::sendAck(const uint8_t toMember, const uint8_t ackedSender, const uint32_t counter, const AckState state) {
  uint8_t raw[HEADER_SIZE + ACK_SIZE];
  const Header h = header(Type::Ack, toMember);
  writeHeader(h, raw);
  writeAck(Ack{ackedSender, counter, state}, raw + HEADER_SIZE);
  seen_.remember(h);
  queue(raw, sizeof(raw), randomBetween(200, 1200), false);
}

const uint8_t* Node::sendingKey(const uint8_t conversation, uint8_t* keyNumber) const {
  if (keyNumber) *keyNumber = 0;
  if (!group_.configured) return nullptr;
  if (conversation == CONV_GROUP) {
    if (keyNumber) *keyNumber = identity().senderNumber;
    return identity().senderKey;
  }
  if (isGroupConversation(conversation)) {
    const ChatGroup& g = groups_.at(conversation - CONV_GROUPS);
    const GroupKey* newest = g.newest();
    if (!g.id || g.left || !(g.members & memberBit(group_.me)) || !newest) return nullptr;
    if (keyNumber) *keyNumber = newest->number;
    return newest->key;
  }
  const Member* m = group_.member(conversation);
  return m && m->present && conversation != group_.me ? m->pairKey : nullptr;
}

bool Node::canWrite(const uint8_t conversation) const { return sendingKey(conversation) != nullptr; }

uint8_t Node::recipientOf(const uint8_t conversation) const {
  if (conversation == CONV_GROUP) return GROUP;
  if (isGroupConversation(conversation)) return GROUP_TO;
  return conversation;
}

int Node::conversationOf(const Header& h) const {
  if (h.recipient == GROUP) return CONV_GROUP;
  if (h.recipient == GROUP_TO) {
    const int slot = groups_.find(h.groupId);
    return slot < 0 ? -1 : CONV_GROUPS + slot;
  }
  return h.sender == group_.me ? h.recipient : h.sender;
}

bool Node::sendText(const uint8_t conversation, const std::string& text) {
  uint8_t keyNumber = 0;
  const uint8_t* key = sendingKey(conversation, &keyNumber);
  if (!key || text.empty() || text.size() > MAX_TEXT) return false;
  const uint8_t recipient = recipientOf(conversation);
  ChatMessage msg;
  msg.sender = group_.me;
  msg.time = unixNow();
  store_.lastHash(conversation, group_.me, msg.prevHash);
  msg.text = text;

  std::string plain(MSG_PREFIX, '\0');
  put32(reinterpret_cast<uint8_t*>(&plain[0]), msg.time);
  memcpy(&plain[4], msg.prevHash, 4);
  plain += text;
  Header h = header(Type::Msg, recipient);
  h.key = keyNumber;
  if (recipient == GROUP_TO) h.groupId = groups_.at(conversation - CONV_GROUPS).id;
  msg.counter = h.counter;
  sendSealed(h, key, reinterpret_cast<const uint8_t*>(plain.data()), plain.size(), true);
  store_.addMessage(conversation, msg);
  if (pending_.size() >= MAX_PENDING) pending_.erase(pending_.begin());
  pending_.push_back(Pending{h.counter, conversation, 0, millis() + RETRY_MS[0]});
  changes_++;
  return true;
}

void Node::markRead(const uint8_t conversation) {
  if (store_.summary(conversation).unread == 0) return;
  // "Read" for the newest message from each other member covers the earlier ones.
  const auto recent = store_.load(conversation, 30);
  bool told[MAX_MEMBERS + 1] = {};
  for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
    if (it->sender == group_.me || !isMember(it->sender) || told[it->sender]) continue;
    told[it->sender] = true;
    sendAck(it->sender, it->sender, it->counter, AckState::Read);
  }
  store_.markRead(conversation);
  changes_++;
}

void Node::sendPosition(const Type type, const bool cancelled) {
  if (!group_.configured) return;
  const gps::Fix& fix = gps::lastFix();
  Position p;
  p.time = fix.valid ? fix.time : unixNow();
  p.latE7 = fix.latE7;
  p.lonE7 = fix.lonE7;
  p.altitude = fix.altitude;
  p.battery = battery();
  p.noFix = !fix.valid || millis() - gps::lastFixAtMs() > 5UL * 60 * 1000;
  p.cancelled = cancelled;
  uint8_t plain[POS_SIZE];
  writePosition(p, plain);
  Header h = header(type, GROUP);
  h.key = identity().senderNumber;
  if (type == Type::Sos) sosCounters_.push_back(h.counter);
  sendSealed(h, identity().senderKey, plain, sizeof(plain), false);
  if (fix.valid) {
    MemberStatus& self = members_[group_.me - 1];
    self.hasPosition = true;
    self.position = p;
    self.positionAtMs = gps::lastFixAtMs();
  }
}

void Node::startSos() {
  if (!group_.configured) return;
  sosActive_ = true;
  nextSosMs_ = millis();
  sosCounters_.clear();
  sosReceivedBy_ = sosSeenBy_ = 0;
  sosOnServer_ = false;
  gps::wake();
  changes_++;
}

void Node::cancelSos() {
  if (!sosActive_) return;
  sosActive_ = false;
  sendPosition(Type::Sos, true);
  if (!MESH_SETTINGS.onTrip) gps::sleep();
  changes_++;
}

bool Node::hasNewSos() const { return newSos_ != 0; }

void Node::acknowledgeSos(const uint8_t from) {
  const MemberStatus& st = member(from);
  if (isMember(from) && st.sosCounter) sendAck(from, from, st.sosCounter, AckState::Read);
}

uint8_t Node::takeNewSos() {
  const uint8_t n = newSos_;
  newSos_ = 0;
  return n;
}

const MemberStatus& Node::member(const uint8_t number) const {
  static const MemberStatus none;
  return isMember(number) ? members_[number - 1] : none;
}

void Node::requestSync() {
  if (!group_.configured || !radio::isUp()) return;
  SyncReq req;
  for (uint8_t m = 1; m <= MAX_MEMBERS; m++) req.lastCounter[m - 1] = store_.lastCounter(m);
  req.battery = battery();
  req.build = buildByte();
  req.flags = syncFlags();
  req.rosterVersion = group_.rosterVersion;
  uint8_t raw[HEADER_SIZE + SYNC_REQ_MAX];
  Header h = header(Type::SyncReq, GROUP);
  h.hops = 0;  // answered by whoever hears it; not relayed
  writeHeader(h, raw);
  const size_t len = HEADER_SIZE + writeSyncReq(req, raw + HEADER_SIZE);
  seen_.remember(h);
  queue(raw, len, 0, false);
  lastSyncReqMs_ = millis();
}

ServerState Node::serverState() const {
  if (serverAtMs_ && millis() - serverAtMs_ < SERVER_FRESH_MS) return ServerState::Online;
  if (wifiServerFailed_ || serverAtMs_) return ServerState::Offline;
  return ServerState::Unknown;
}

bool Node::busy() const {
  return !outbox_.empty() || wifi_ != WifiStep::Idle || int32_t(checkUntilMs_ - millis()) > 0;
}

uint32_t Node::msUntilWork() const {
  const uint32_t now = millis();
  uint32_t next = 10UL * 60 * 1000;
  auto consider = [&](const uint32_t at) { next = std::min(next, at > now ? at - now : 0); };
  for (const auto& o : outbox_) consider(o.dueMs);
  for (const auto& p : pending_) {
    if (p.tries < 3) consider(p.nextTryMs);
  }
  if (MESH_SETTINGS.onTrip) consider(nextPositionMs_);
  if (sosActive_) consider(nextSosMs_);
  return next;
}

// --- Receiving ----------------------------------------------------------------

const uint8_t* Node::keyFor(const Header& h) const {
  const Member* from = group_.member(h.sender);
  if (!from || !from->present) return nullptr;
  if (h.recipient == GROUP) return senders_.key(h.sender, h.key);  // kept until their key comes
  if (h.recipient == group_.me) return from->pairKey;
  if (h.recipient == GROUP_TO) {
    // Not (yet) mine: the packet is kept until the key comes. A member who was
    // taken out can no longer write.
    const int slot = groups_.find(h.groupId);
    if (slot < 0 || !groups_.at(slot).has(h.sender)) return nullptr;
    return groups_.at(slot).key(h.key);
  }
  return nullptr;  // a DM between two others
}

void Node::handle(const uint8_t* raw, const size_t len, const int rssi, const bool viaWifi) {
  Header h;
  if (!readHeader(raw, len, h) || h.group != group_.id || h.sender == group_.me) return;
  const uint32_t now = millis();
  if (int32_t(checkUntilMs_ - now) > 0 || viaWifi) lastCheckUnix_ = unixNow();
  if (h.sender == SERVER) {
    serverAtMs_ = now;
  } else if (!viaWifi && (h.hops == DEFAULT_HOPS || h.type == Type::SyncReq)) {
    // Straight from their radio (not relayed, not a stored copy): they are near.
    members_[h.sender - 1].heardAtMs = now;
    members_[h.sender - 1].rssi = rssi;
  }

  // Someone else already sent a relay or sync answer we had queued: skip ours.
  outbox_.erase(std::remove_if(outbox_.begin(), outbox_.end(),
                               [&](const Outgoing& o) { return o.cancelIfHeard && samePacket(o.raw.data(), raw); }),
                outbox_.end());

  if (!seen_.remember(h, raw + HEADER_SIZE, len - HEADER_SIZE)) {
    // A message we already have came again straight from its sender (a retry,
    // not someone's relay or a stored copy): the sender missed our ACK.
    // Only when the message is really here (a piece may be missing).
    if (h.type == Type::Msg && h.hops == DEFAULT_HOPS && h.pieceIndex == h.pieceCount - 1 &&
        canRead(group_.me, h.sender, h.recipient) && isMember(h.sender) && haveMessage(h)) {
      sendAck(h.sender, h.sender, h.counter, AckState::Delivered);
    }
    return;
  }

  // Pass it on so units out of each other's range still hear it.
  if (!viaWifi && h.hops > 0 && h.type != Type::SyncReq && h.recipient != group_.me) {
    std::vector<uint8_t> copy(raw, raw + len);
    copy[3] = uint8_t((copy[3] & 0xF0) | (h.hops - 1));
    queue(copy.data(), copy.size(), randomBetween(500, 2500), true);
  }

  const uint8_t* body = raw + HEADER_SIZE;
  const size_t bodyLen = len - HEADER_SIZE;
  switch (h.type) {
    case Type::Msg: {
      if (!isMember(h.sender) || store_.hasPacket(h, raw, len)) return;
      store_.savePacket(h.sender, raw, len);  // kept to help other units catch up
      if (h.pieceCount == 1) {
        handleMessage(h, body, bodyLen);
      } else {
        // From the pieces kept on the card, so a piece that comes much later
        // (a retry minutes after) still completes it.
        Reassembler pieces;
        const uint8_t* joined = nullptr;
        size_t joinedLen = 0;
        for (const auto& piece : store_.piecesOf(h.sender, h.counter)) {
          Header ph;
          if (readHeader(piece.data(), piece.size(), ph) &&
              pieces.add(ph, piece.data(), piece.size(), now, &joined, &joinedLen)) {
            handleMessage(h, joined, joinedLen);
            break;
          }
        }
      }
      break;
    }
    case Type::Pos:
    case Type::Sos:
      if (h.pieceCount == 1) handlePosition(h, body, bodyLen);
      break;
    case Type::Ack:
      handleAck(h, body, bodyLen);
      break;
    case Type::SyncReq:
      if (!viaWifi) handleSyncReq(h, body, bodyLen);
      break;
    case Type::Fetch:
      if (!viaWifi) handleFetch(h, body, bodyLen);
      break;
    case Type::Roster: {
      // From the admin's tool through the server (or a unit passing it on).
      // The pieces are kept, to pass the roster on to units that missed it.
      if (h.sender != SERVER || h.counter < ROSTER_COUNTER + group_.rosterVersion) break;
      Header first;
      if (!rosterPieces_.empty() &&
          (!readHeader(rosterPieces_[0].data(), rosterPieces_[0].size(), first) || first.counter != h.counter)) {
        rosterPieces_.clear();
      }
      if (rosterPieces_.size() < MAX_PIECES) rosterPieces_.emplace_back(raw, raw + len);
      const uint8_t* joined = nullptr;
      size_t joinedLen = 0;
      if (pieces_.add(h, raw, len, now, &joined, &joinedLen)) handleRoster(h, joined, joinedLen);
      break;
    }
  }
  changes_++;
}

bool Node::haveMessage(const Header& h) const {
  if (std::find(opened_.begin(), opened_.end(), uint64_t(h.sender) << 32 | h.counter) != opened_.end()) return true;
  const int conversation = conversationOf(h);
  return conversation >= 0 && store_.hasMessage(uint8_t(conversation), h.sender, h.counter);
}

void Node::handleMessage(const Header& h, const uint8_t* cipher, const size_t len) {
  const uint8_t* key = keyFor(h);
  if (!key) {
    missingKey(h);
    return;
  }
  if (len < TAG_SIZE + MSG_PREFIX) return;
  std::vector<uint8_t> plain(len - TAG_SIZE);
  if (!open(h, key, cipher, len, plain.data())) {
    LOG_ERR("MESH", "Message %u/%lu failed its check (wrong key or changed)", h.sender, (unsigned long)h.counter);
    return;
  }
  // Only now does its counter count for syncs (MessageStore::lastCounter).
  store_.confirmCounter(h.sender, h.counter);
  if (opened_.size() >= 64) opened_.erase(opened_.begin());
  opened_.push_back(uint64_t(h.sender) << 32 | h.counter);
  const int found = conversationOf(h);
  if (found < 0) return;
  const uint8_t conversation = uint8_t(found);
  ChatMessage m;
  m.sender = h.sender;
  m.counter = h.counter;
  m.time = get32(plain.data());
  memcpy(m.prevHash, plain.data() + 4, 4);
  m.text.assign(reinterpret_cast<const char*>(plain.data() + MSG_PREFIX), plain.size() - MSG_PREFIX);
  // A group invitation or a member leaving, not a message to show.
  if (!m.text.empty() && m.text[0] == '\0' && h.recipient == group_.me) {
    // Not confirmed when it cannot be taken yet: it comes again at their next check.
    if (handleControl(h.sender, m.text)) sendAck(h.sender, h.sender, h.counter, AckState::Delivered);
    return;
  }
  if (store_.hasMessage(conversation, h.sender, h.counter)) return;
  // The chain: the previous message from this sender should be the one we have.
  // A message older than our newest from them fills a gap and is not checked.
  uint8_t expected[4];
  store_.lastHash(conversation, h.sender, expected);
  const bool haveEarlier = expected[0] | expected[1] | expected[2] | expected[3];
  const uint32_t newest = store_.lastChainCounter(conversation, h.sender);
  const bool late = h.counter < newest;
  m.chainWarning = haveEarlier && !late && memcmp(expected, m.prevHash, 4) != 0;
  // Something is missing between our newest and this one: ask for exactly that.
  if (m.chainWarning) requestFetch(h.sender, newest, h.counter);
  store_.addMessage(conversation, m);
  if (late) fillGap(conversation, m);
  sendAck(h.sender, h.sender, h.counter, AckState::Delivered);
  // The radio's clock source when the unit has none yet.
  if (!unixNow() && m.time > 1600000000) halClock.setUtc(m.time);
}

void Node::handlePosition(const Header& h, const uint8_t* cipher, const size_t len) {
  const Member* from = group_.member(h.sender);
  const uint8_t* key = senders_.key(h.sender, h.key);
  if (!from || !from->present || h.recipient != GROUP || len != POS_SIZE + TAG_SIZE) return;
  if (!key) {
    missingKey(h);
    return;
  }
  uint8_t plain[POS_SIZE];
  if (!open(h, key, cipher, len, plain)) return;
  Position p;
  if (!readPosition(plain, sizeof(plain), p)) return;
  MemberStatus& st = members_[h.sender - 1];
  if (p.battery) st.battery = p.battery;
  if (!p.noFix) {
    st.hasPosition = true;
    st.position = p;
    st.positionAtMs = millis();
    savePositions();
  }
  if (h.type == Type::Sos) {
    if (p.cancelled) {
      st.sos = false;
    } else {
      if (!st.sos) newSos_ = h.sender;
      st.sos = true;
      st.sosPosition = p;
      st.sosCounter = h.counter;
      // Every repeat is answered, so a lost answer is made up a minute later.
      sendAck(h.sender, h.sender, h.counter, AckState::Delivered);
    }
  }
}

uint8_t Node::conversationOfMine(const uint32_t counter) {
  for (const auto& p : pending_) {
    if (p.counter == counter) return p.conversation;
  }
  // Older messages: look at the stored packet's recipient.
  const auto raw = store_.packetsAfter(group_.me, counter - 1, group_.me, 1);
  Header h;
  if (!raw.empty() && readHeader(raw[0].data(), raw[0].size(), h) && h.counter == counter) {
    const int c = conversationOf(h);
    return c < 0 ? CONV_GROUP : uint8_t(c);
  }
  return CONV_GROUP;
}

void Node::handleAck(const Header& h, const uint8_t* body, const size_t len) {
  Ack ack;
  if (!readAck(body, len, ack) || ack.sender != group_.me) return;
  controlWaiting_.erase(std::remove(controlWaiting_.begin(), controlWaiting_.end(), ack.counter), controlWaiting_.end());
  // An answer to my SOS: who has it, and who has seen it.
  if (std::find(sosCounters_.begin(), sosCounters_.end(), ack.counter) != sosCounters_.end()) {
    if (h.sender == SERVER) {
      sosOnServer_ = true;
    } else {
      sosReceivedBy_ |= memberBit(h.sender);
      if (ack.state == AckState::Read) sosSeenBy_ |= memberBit(h.sender);
    }
    return;
  }
  if (confirmControl(h.sender, ack.counter)) {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Pending& p) { return p.counter == ack.counter; }),
                   pending_.end());
    return;  // not a chat message: no "delivered" in the chat
  }
  const uint8_t conversation = conversationOfMine(ack.counter);
  const Delivery state = ack.state == AckState::Stored ? Delivery::Stored
                         : ack.state == AckState::Read ? Delivery::Read
                                                       : Delivery::Delivered;
  store_.addState(conversation, group_.me, ack.counter, state, h.sender);
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                [&](const Pending& p) { return p.counter == ack.counter; }),
                 pending_.end());
}

void Node::handleSyncReq(const Header& h, const uint8_t* body, const size_t len) {
  SyncReq req;
  if (!isMember(h.sender) || !readSyncReq(body, len, req)) return;
  if (req.battery) members_[h.sender - 1].battery = req.battery;
  size_t sent = 0;
  // Their roster is older (it may take someone out): it goes first, whole.
  if (req.rosterVersion < group_.rosterVersion) {
    for (const auto& raw : savedRosterPackets()) {
      std::vector<uint8_t> copy = raw;
      copy[3] &= 0xF0;
      queue(copy.data(), copy.size(), randomBetween(1500, 4000) + uint32_t(sent) * 300, true);
      sent++;
    }
  }
  for (uint8_t s = 1; s <= MAX_MEMBERS && sent < MAX_SYNC_ANSWER; s++) {
    if (s == h.sender) continue;
    for (const auto& raw : store_.packetsAfter(s, req.lastCounter[s - 1], h.sender, MAX_SYNC_ANSWER - sent)) {
      // Not relayed further, and skipped if another unit or the server answers first.
      std::vector<uint8_t> copy = raw;
      copy[3] &= 0xF0;
      queue(copy.data(), copy.size(), randomBetween(1500, 4000) + uint32_t(sent) * 300, true);
      sent++;
    }
  }
}

void Node::handleFetch(const Header& h, const uint8_t* body, const size_t len) {
  Fetch f;
  if (!isMember(h.sender) || !readFetch(body, len, f) || f.sender == h.sender) return;
  size_t sent = 0;
  for (const auto& raw : store_.packetsAfter(f.sender, f.after, h.sender, MAX_SYNC_ANSWER)) {
    Header ph;
    if (!readHeader(raw.data(), raw.size(), ph) || ph.counter >= f.before) continue;
    std::vector<uint8_t> copy = raw;
    copy[3] &= 0xF0;
    queue(copy.data(), copy.size(), randomBetween(1500, 4000) + uint32_t(sent) * 300, true);
    sent++;
  }
}

void Node::requestFetch(const uint8_t sender, const uint32_t after, const uint32_t before) {
  if (!isMember(sender) || before <= after + 1) return;
  const Fetch f{sender, after, before};
  // Over LoRa now (the server through the gateway, or another unit answers),
  // and over Wi-Fi with the next sync.
  if (fetches_.size() < 8) fetches_.push_back(f);
  if (!radio::isUp()) return;
  uint8_t raw[HEADER_SIZE + FETCH_SIZE];
  Header h = header(Type::Fetch, GROUP);
  h.hops = 0;
  writeHeader(h, raw);
  writeFetch(f, raw + HEADER_SIZE);
  seen_.remember(h);
  queue(raw, sizeof(raw), randomBetween(500, 1500), false);
  LOG_INF("MESH", "Asked for %u's messages %lu-%lu", sender, (unsigned long)after, (unsigned long)before);
}

void Node::fillGap(const uint8_t conversation, const ChatMessage& late) {
  // The next message from the same sender pointed at this one: its warning goes.
  uint8_t hash[4];
  messageHash(late, hash);
  for (const auto& m : store_.load(conversation, 40)) {
    if (m.sender == late.sender && m.counter > late.counter && m.chainWarning && memcmp(m.prevHash, hash, 4) == 0) {
      store_.clearWarning(conversation, m.sender, m.counter);
      break;
    }
  }
  fetches_.erase(std::remove_if(fetches_.begin(), fetches_.end(),
                                [&](const Fetch& f) {
                                  return f.sender == late.sender && f.after < late.counter && late.counter < f.before;
                                }),
                 fetches_.end());
}

uint8_t Node::syncFlags() const {
  const uint8_t check = MESH_SETTINGS.onTrip ? MESH_SETTINGS.tripCheck : MESH_SETTINGS.homeCheck;
  return uint8_t((MESH_SETTINGS.onTrip ? SYNC_ON_TRIP : 0) | (sosActive_ ? SYNC_SOS : 0) |
                 ((check & 7) << SYNC_CHECK_SHIFT));
}

// --- Groups and keys (protocol/PROTOCOL.md, "Control DMs", "Groups") ------------
//
// Only the remaining member with the lowest number makes a new group key, so
// two members never make one at once. Others ask it to (REMOVE), or it does so
// by itself when told someone left (LEAVE). Invitations from someone not in the
// group, or older than the key held, do not change who is in it.

namespace {
constexpr char SENDER_KEY = 'S';
constexpr char INVITE = 'G';
constexpr char LEAVE = 'L';
constexpr char REMOVE = 'R';                   // "please take this member out" to the lowest member
constexpr char ASK_KEY = 'K';                  // "send me this key": group id (0 = your sender key), number
constexpr uint32_t ASK_KEY_AGAIN_MS = 6 * 3600 * 1000UL;
// Key DMs to many members: spread over this long (all units hear a new member
// list at once, and LoRa has no "wait until the air is free").
constexpr uint32_t KEY_SPREAD_MS = 60000;
constexpr uint8_t NEW_GROUP = 1;               // invitation flags: a group starting afresh
constexpr size_t INVITE_SIZE = 10 + KEY_SIZE;  // 0, 'G', id (2), key number, flags, members (4), key; then the name

uint8_t lowest(const Members members) {
  for (uint8_t n = 1; n <= MAX_MEMBERS; n++) {
    if (members & memberBit(n)) return n;
  }
  return 0;
}

std::string idBytes(const uint16_t id) { return std::string{char(id & 0xFF), char(id >> 8)}; }
uint16_t readId(const std::string& text, const size_t at) {
  return uint16_t(uint8_t(text[at]) | (uint8_t(text[at + 1]) << 8));
}
std::string invitation(const ChatGroup& g, const GroupKey& k, const bool fresh) {
  std::string p = std::string{'\0', INVITE} + idBytes(g.id) + char(k.number) + char(fresh ? NEW_GROUP : 0);
  for (int i = 0; i < 4; i++) p += char((g.members >> (8 * i)) & 0xFF);
  p.append(reinterpret_cast<const char*>(k.key), KEY_SIZE);
  p += g.name;
  return p;
}
}  // namespace

uint32_t Node::sendControl(const uint8_t to, const std::string& payload, const uint32_t delayMs) {
  const Member* m = group_.member(to);
  if (!m || !m->present || to == group_.me) return 0;
  // A DM like any other (ACKs, retries, sync), never shown as a message.
  std::string plain(MSG_PREFIX, '\0');
  put32(reinterpret_cast<uint8_t*>(&plain[0]), unixNow());
  plain += payload;
  const Header h = header(Type::Msg, to);
  sendSealed(h, m->pairKey, reinterpret_cast<const uint8_t*>(plain.data()), plain.size(), true, delayMs);
  // Not in pending_ (31 key DMs would push the user's own retries out): the
  // ones that must arrive are sent again at each check until confirmed. Until
  // someone answers, it also goes up with each Wi-Fi sync.
  if (controlWaiting_.size() >= 64) controlWaiting_.erase(controlWaiting_.begin());
  controlWaiting_.push_back(h.counter);
  if (controlCounters_.size() >= 64) controlCounters_.erase(controlCounters_.begin());
  controlCounters_.push_back(h.counter);
  return h.counter;
}

void Node::sendSenderKey(const uint8_t to, const uint32_t delayMs) {
  const Member* m = group_.member(to);
  if (!m || !m->present || to == group_.me) return;
  const Identity& mine = identity();
  std::string p{'\0', SENDER_KEY, char(mine.senderNumber)};
  p.append(reinterpret_cast<const char*>(mine.senderKey), KEY_SIZE);
  senderKeyCounters_[to - 1] = sendControl(to, p, delayMs);
}

void Node::sendToMember(const int slot, const uint8_t to, const char kind, const uint8_t about) {
  ChatGroup& g = groups_.at(slot);
  std::string p;
  if (kind == INVITE) {
    const GroupKey* k = g.newest();
    if (!k) return;
    p = invitation(g, *k, k->number == 1);
  } else {
    p = std::string{'\0', kind} + idBytes(g.id) + char(about);
  }
  // Remembered until their unit confirms it, and sent again on each check.
  g.setNotice(to, kind, about, sendControl(to, p));
}

void Node::rotate(const int slot) {
  ChatGroup& g = groups_.at(slot);
  const GroupKey* newest = g.newest();
  uint8_t key[KEY_SIZE];
  esp_fill_random(key, KEY_SIZE);
  g.addKey(uint8_t((newest ? newest->number : 0) + 1), group_.me, key);
  for (uint8_t n = 1; n <= MAX_MEMBERS; n++) {
    if (n != group_.me && g.has(n)) sendToMember(slot, n, INVITE, 0);
  }
  groups_.save();
  LOG_INF("MESH", "Group %04x has key %u", g.id, g.newest()->number);
}

void Node::takeOut(const int slot, const uint8_t member) {
  ChatGroup& g = groups_.at(slot);
  if (!g.has(member)) return;
  g.members &= ~memberBit(member);
  for (auto& n : g.notices) {
    if (n.member == member) n = Notice{};
  }
  groups_.save();
  if (!g.left && lowest(g.members) == group_.me) rotate(slot);
}

bool Node::handleControl(const uint8_t from, const std::string& text) {
  if (text.size() < 3) return true;
  if (text[1] == SENDER_KEY) {
    if (text.size() < 3 + KEY_SIZE) return true;
    if (senders_.add(from, uint8_t(text[2]), reinterpret_cast<const uint8_t*>(&text[3]))) {
      senders_.save();
      LOG_INF("MESH", "Sender key %u from %u", uint8_t(text[2]), from);
      openWaitingFrom(from);
    }
    changes_++;
    return true;
  }
  if (text.size() < 5) return true;
  const uint16_t id = readId(text, 2);
  if (text[1] == ASK_KEY) {
    answerKeyRequest(from, id, uint8_t(text[4]));
    return true;
  }
  if (!isMember(groupCreator(id))) return true;
  int slot = groups_.find(id);
  if (text[1] == INVITE && text.size() >= INVITE_SIZE) {
    const uint8_t number = uint8_t(text[4]);
    const bool fresh = (uint8_t(text[5]) & NEW_GROUP) != 0;
    Members members = 0;
    for (int i = 0; i < 4; i++) members |= Members(uint8_t(text[6 + i])) << (8 * i);
    const uint8_t* key = reinterpret_cast<const uint8_t*>(&text[10]);
    if (!number || !(members & memberBit(from)) || !(members & memberBit(group_.me))) return true;
    // Only its creator starts a group, or starts an id of theirs afresh: nobody
    // else can make up a group under someone's id or take one over.
    const bool creator = from == groupCreator(id);
    if (slot < 0) {
      // A later key from another member may come before the creator's first:
      // not confirmed, so it comes again after the creator's.
      if (!creator) return false;
      slot = groups_.take(id);
      if (slot < 0) return true;
      groups_.at(slot) = ChatGroup{};
      groups_.at(slot).id = id;
      groups_.at(slot).members = members;
      store_.clear(uint8_t(CONV_GROUPS + slot));
    } else if (fresh && creator && groups_.at(slot).key(1) &&
               memcmp(groups_.at(slot).key(1), key, KEY_SIZE) != 0) {
      // The creator used an id again (they had left the old group).
      groups_.at(slot) = ChatGroup{};
      groups_.at(slot).id = id;
      groups_.at(slot).members = members;
      store_.clear(uint8_t(CONV_GROUPS + slot));
    }
    ChatGroup& g = groups_.at(slot);
    // Only a member can change the group, and only with a newer key.
    if (!g.has(from)) return true;
    const GroupKey* newest = g.newest();
    const uint8_t held = newest ? newest->number : 0;
    const bool replaced = g.addKey(number, from, key);
    if (number > held || (number == held && replaced)) {
      g.members = members;
      g.left = false;  // added (again) with a key made after I left
      if (text.size() > INVITE_SIZE) strlcpy(g.name, text.c_str() + INVITE_SIZE, sizeof(g.name));
    }
    groups_.save();
    LOG_INF("MESH", "Group %04x: key %u from %u", id, number, from);
    openWaiting(id);
    changes_++;
    return true;
  }
  if (slot < 0) return true;
  ChatGroup& g = groups_.at(slot);
  if (!g.has(from)) return true;
  if (text[1] == LEAVE) {
    takeOut(slot, from);
  } else if (text[1] == REMOVE) {
    // Asked to take someone out: only the lowest of those who stay does it.
    const uint8_t member = uint8_t(text[4]);
    if (!isMember(member) || member == group_.me || !g.has(member)) return true;
    if (lowest(g.members & ~memberBit(member)) == group_.me) takeOut(slot, member);
  }
  changes_++;
  return true;
}

// --- Asking for a key that is missing ----------------------------------------
//
// A key DM that got lost, or whose confirmation was forged (ACKs carry no
// proof), would leave a member without a key for good. So a unit that gets a
// message under a key number it lacks asks for it ('K'), at most every few hours.

void Node::missingKey(const Header& h) {
  const Member* from = group_.member(h.sender);
  if (!group_.configured || !from || !from->present || !h.key) return;
  if (h.recipient == GROUP) {
    askForKey(h.sender, 0, h.key);
  } else if (h.recipient == GROUP_TO) {
    const int slot = groups_.find(h.groupId);
    if (slot < 0) {
      // Its invitation never came: the creator sends it again if I am in it.
      // Its counter counts, so syncs move past groups I am not in.
      store_.confirmCounter(h.sender, h.counter);
      if (isMember(groupCreator(h.groupId)) && groupCreator(h.groupId) != group_.me) {
        askForKey(groupCreator(h.groupId), h.groupId, h.key);
      }
      return;
    }
    const ChatGroup& g = groups_.at(slot);
    if (!g.left && g.has(group_.me) && g.has(h.sender) && !g.key(h.key)) askForKey(h.sender, h.groupId, h.key);
  }
}

void Node::askForKey(const uint8_t to, const uint16_t groupId, const uint8_t number) {
  const uint32_t id = uint32_t(to) << 24 | uint32_t(groupId) << 8 | number;
  const uint32_t now = millis();
  auto it = std::find_if(askedKeys_.begin(), askedKeys_.end(), [&](const AskedKey& a) { return a.id == id; });
  if (it != askedKeys_.end()) {
    if (now - it->atMs < ASK_KEY_AGAIN_MS) return;
    it->atMs = now;
  } else {
    if (askedKeys_.size() >= 32) {
      // Full: the oldest goes, unless all are recent (a flood of made-up numbers).
      auto oldest = std::min_element(askedKeys_.begin(), askedKeys_.end(),
                                     [](const AskedKey& a, const AskedKey& b) { return a.atMs < b.atMs; });
      if (now - oldest->atMs < ASK_KEY_AGAIN_MS) return;
      askedKeys_.erase(oldest);
    }
    askedKeys_.push_back(AskedKey{id, now});
  }
  LOG_INF("MESH", "Asking %u for key %u of %04x", to, number, groupId);
  sendControl(to, std::string{'\0', ASK_KEY} + idBytes(groupId) + char(number));
}

void Node::answerKeyRequest(const uint8_t from, const uint16_t groupId, const uint8_t number) {
  if (!groupId) {
    // My sender key: sent again (and no longer counted as confirmed).
    if (number != identity().senderNumber) return;
    senderKeyConfirmed_ &= ~memberBit(from);
    saveKeyConfirmations();
    sendSenderKey(from);
    return;
  }
  const int slot = groups_.find(groupId);
  if (slot < 0) return;
  ChatGroup& g = groups_.at(slot);
  if (g.left || !g.has(from) || !g.has(group_.me)) return;
  const GroupKey* newest = g.newest();
  if (newest && newest->number == number) {
    sendToMember(slot, from, INVITE, 0);
    groups_.save();
    return;
  }
  for (const auto& k : g.keys) {
    // An older key, for history: once, not kept as a notice.
    if (k.number && k.number == number) sendControl(from, invitation(g, k, false));
  }
}

bool Node::confirmControl(const uint8_t from, const uint32_t counter) {
  if (!isMember(from)) return false;
  // My sender key reached them.
  if (senderKeyCounters_[from - 1] == counter) {
    senderKeyCounters_[from - 1] = 0;
    senderKeyConfirmed_ |= memberBit(from);
    saveKeyConfirmations();
    return true;
  }
  // A member's unit got my invitation, leave or remove: stop sending it.
  for (int slot = 0; slot < MAX_GROUPS; slot++) {
    if (groups_.at(slot).id && groups_.at(slot).confirm(from, counter)) {
      groups_.save();
      return true;
    }
  }
  return std::find(controlCounters_.begin(), controlCounters_.end(), counter) != controlCounters_.end();
}

void Node::resendUnconfirmed() {
  if (!group_.configured) return;
  // My sender key, to everyone who has not confirmed it.
  for (uint8_t n = 1; n <= MAX_MEMBERS; n++) {
    if (n != group_.me && group_.member(n)->present && !(senderKeyConfirmed_ & memberBit(n))) {
      sendSenderKey(n, randomBetween(0, KEY_SPREAD_MS));
    }
  }
  bool changed = false;
  for (int slot = 0; slot < MAX_GROUPS; slot++) {
    ChatGroup& g = groups_.at(slot);
    if (!g.id) continue;
    for (Notice& n : g.notices) {
      if (!n.member) continue;
      const Member* m = group_.member(n.member);
      if (!m || !m->present) {
        n = Notice{};  // gone from the network
      } else {
        sendToMember(slot, n.member, char(n.kind), n.about);
      }
      changed = true;
    }
  }
  if (changed) groups_.save();
}

void Node::openWaiting(const uint16_t groupId) {
  // Group messages that came before the key: kept as packets, opened now.
  Reassembler pieces;
  for (uint8_t s = 1; s <= MAX_MEMBERS; s++) {
    if (s == group_.me) continue;
    for (const auto& raw : store_.packetsTo(s, GROUP_TO)) {
      Header h;
      if (!readHeader(raw.data(), raw.size(), h) || h.groupId != groupId) continue;
      const uint8_t* joined = nullptr;
      size_t joinedLen = 0;
      if (pieces.add(h, raw.data(), raw.size(), millis(), &joined, &joinedLen)) {
        // handleMessage keeps what it needs; copy, the buffer goes on the next add().
        const std::vector<uint8_t> copy(joined, joined + joinedLen);
        handleMessage(h, copy.data(), copy.size());
      }
    }
  }
}

void Node::openWaitingFrom(const uint8_t sender) {
  // Messages to everyone that came before the sender's key.
  Reassembler pieces;
  for (const auto& raw : store_.packetsTo(sender, GROUP)) {
    Header h;
    if (!readHeader(raw.data(), raw.size(), h)) continue;
    const uint8_t* joined = nullptr;
    size_t joinedLen = 0;
    if (pieces.add(h, raw.data(), raw.size(), millis(), &joined, &joinedLen)) {
      const std::vector<uint8_t> copy(joined, joined + joinedLen);
      handleMessage(h, copy.data(), copy.size());
    }
  }
}

uint8_t Node::createGroup(const Members others, const std::string& name) {
  const Members members = (others & group_.everyone()) | memberBit(group_.me);
  if (!group_.configured || memberCount(members) < 2) return CONV_GROUP;
  const uint16_t id = groups_.nextId(group_.me);
  if (!id) return CONV_GROUP;
  const int slot = groups_.take(id);
  if (slot < 0) return CONV_GROUP;  // 16 groups already
  ChatGroup& g = groups_.at(slot);
  g = ChatGroup{};
  g.id = id;
  g.members = members;
  strlcpy(g.name, name.c_str(), sizeof(g.name));
  store_.clear(uint8_t(CONV_GROUPS + slot));
  rotate(slot);  // key number 1 (marked new), and the invitations
  changes_++;
  return uint8_t(CONV_GROUPS + slot);
}

void Node::leaveGroup(const uint8_t conversation) {
  if (!isGroupConversation(conversation)) return;
  const int slot = conversation - CONV_GROUPS;
  ChatGroup& g = groups_.at(slot);
  if (!g.id || g.left) return;
  for (uint8_t n = 1; n <= MAX_MEMBERS; n++) {
    if (n != group_.me && g.has(n)) sendToMember(slot, n, LEAVE, 0);
  }
  g.left = true;
  groups_.save();
  changes_++;
}

void Node::removeFromGroup(const uint8_t conversation, const uint8_t member) {
  if (!isGroupConversation(conversation) || member == group_.me) return;
  const int slot = conversation - CONV_GROUPS;
  ChatGroup& g = groups_.at(slot);
  if (!g.id || g.left || !g.has(member)) return;
  const uint8_t keeper = lowest(g.members & ~memberBit(member));
  if (keeper == group_.me) {
    takeOut(slot, member);
  } else {
    sendToMember(slot, keeper, REMOVE, member);  // the lowest member makes the new key
    groups_.save();
  }
  changes_++;
}

void Node::dropMissingMembers() {
  // Someone gone from the roster leaves every group.
  for (int slot = 0; slot < MAX_GROUPS; slot++) {
    ChatGroup& g = groups_.at(slot);
    if (!g.id) continue;
    for (uint8_t n = 1; n <= MAX_MEMBERS; n++) {
      if (g.has(n) && !(group_.everyone() & memberBit(n))) takeOut(slot, n);
    }
  }
}

// --- The roster (protocol/PROTOCOL.md, "The roster") -----------------------------

void Node::handleRoster(const Header& h, const uint8_t* cipher, const size_t len) {
  if (h.sender != SERVER || h.recipient != GROUP || len <= TAG_SIZE) return;
  std::vector<uint8_t> plain(len - TAG_SIZE);
  if (!open(h, group_.networkKey, cipher, len, plain.data())) return;
  const Members before = group_.everyone();
  const bool changed = applyRoster(group_, plain.data(), plain.size());
  // The pieces of the roster I have now, to pass on (handleSyncReq).
  if (h.counter == ROSTER_COUNTER + group_.rosterVersion && rosterPieces_.size() == h.pieceCount) {
    std::vector<uint8_t> framed;
    for (const auto& piece : rosterPieces_) {
      framed.push_back(uint8_t(piece.size()));
      framed.insert(framed.end(), piece.begin(), piece.end());
    }
    saveSealed(ROSTER_PACKETS_PATH, framed);
  }
  rosterPieces_.clear();
  if (changed) rosterChanged(before);
}

std::vector<std::vector<uint8_t>> Node::savedRosterPackets() const {
  std::vector<std::vector<uint8_t>> out;
  std::vector<uint8_t> framed;
  if (!loadSealed(ROSTER_PACKETS_PATH, framed)) return out;
  for (size_t at = 0; at < framed.size();) {
    const size_t n = framed[at];
    Header h;
    if (at + 1 + n > framed.size() || !readHeader(&framed[at + 1], n, h)) break;
    if (h.counter != ROSTER_COUNTER + group_.rosterVersion) return {};  // an older one
    out.emplace_back(framed.begin() + long(at + 1), framed.begin() + long(at + 1 + n));
    at += 1 + n;
  }
  return out;
}

void Node::rosterChanged(const Members before) {
  if (!group_.configured) {
    LOG_INF("MESH", "Taken out of the network");
    changes_++;
    return;
  }
  const Members now = group_.everyone();
  const Members gone = before & ~now;
  for (uint8_t n = 1; n <= MAX_MEMBERS; n++) {
    if (gone & memberBit(n)) {
      senders_.forget(n);
      members_[n - 1] = MemberStatus{};
    }
  }
  if (gone) {
    senders_.save();
    dropMissingMembers();
    // Those gone hold my sender key: a new one, for those who stay.
    newSenderKey();
    senderKeyConfirmed_ = 0;
  }
  // Newcomers (or everyone, after a new key) get my sender key.
  senderKeyConfirmed_ &= before & now;
  saveKeyConfirmations();
  for (uint8_t n = 1; n <= MAX_MEMBERS; n++) {
    if (n != group_.me && (now & memberBit(n)) && !(senderKeyConfirmed_ & memberBit(n))) {
      sendSenderKey(n, randomBetween(1000, KEY_SPREAD_MS));
    }
  }
  changes_++;
}

void Node::loadKeyConfirmations() {
  Preferences prefs;
  prefs.begin("waddlemesh", true);
  // Confirmations count only for the sender key they were for.
  const bool sameKey = prefs.getUInt("keySentNum", 0) == identity().senderNumber;
  senderKeyConfirmed_ = sameKey ? prefs.getUInt("keySent", 0) : 0;
  prefs.end();
}

void Node::saveKeyConfirmations() {
  Preferences prefs;
  prefs.begin("waddlemesh", false);
  prefs.putUInt("keySent", senderKeyConfirmed_);
  prefs.putUInt("keySentNum", identity().senderNumber);
  prefs.end();
}

// --- Sending and timers -------------------------------------------------------

void Node::sendOutbox() {
  if (!radio::isUp() || outbox_.empty()) return;
  const uint32_t now = millis();
  auto next = std::min_element(outbox_.begin(), outbox_.end(),
                               [](const Outgoing& a, const Outgoing& b) { return a.dueMs < b.dueMs; });
  if (int32_t(now - next->dueMs) < 0) return;
  const uint32_t airtime = airtimeMs(params_, next->raw.size());
  if (!budget_.allows(airtime, now)) return;  // waits until the hour has room
  budget_.spend(airtime, now);
  const std::vector<uint8_t> raw = std::move(next->raw);
  outbox_.erase(next);
  if (!radio::send(raw.data(), raw.size())) return;
  // My message has left the unit: show "sendt".
  Header h;
  if (readHeader(raw.data(), raw.size(), h) && h.type == Type::Msg && h.sender == group_.me &&
      h.pieceIndex == h.pieceCount - 1) {
    store_.addState(conversationOfMine(h.counter), group_.me, h.counter, Delivery::Sent, 0);
    changes_++;
  }
}

void Node::retryPending() {
  const uint32_t now = millis();
  bool needWifi = false;
  for (auto& p : pending_) {
    if (p.tries >= 3 || int32_t(now - p.nextTryMs) < 0) continue;
    for (const auto& raw : store_.packetsAfter(group_.me, p.counter - 1, group_.me, MAX_PIECES)) {
      Header h;
      if (readHeader(raw.data(), raw.size(), h) && h.counter == p.counter) queue(raw.data(), raw.size(), 0, false);
    }
    p.tries++;
    p.nextTryMs = now + RETRY_MS[std::min<int>(p.tries, 2)];
    if (p.tries >= 3) needWifi = true;
  }
  // Nobody heard it over LoRa: try the server over Wi-Fi (not on low battery).
  if (needWifi && !group_.serverUrl.empty() && battery() >= LOW_BATTERY &&
      (!lastWifiSyncMs_ || now - lastWifiSyncMs_ > WIFI_FALLBACK_GAP_MS)) {
    startWifiSync();
  }
}

void Node::startWifiSync() {
  if (!group_.configured || group_.serverUrl.empty() || wifi_ != WifiStep::Idle) return;
  if (WiFi.status() != WL_CONNECTED && !waddle::wifi::joinSaved()) return;
  wifi_ = WifiStep::Joining;
  wifiStartedMs_ = millis();
  changes_++;
}

void Node::runWifiSync() {
  lastWifiSyncMs_ = millis();
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP || group_.serverCert.empty()) {
    wifiServerFailed_ = true;
    return;
  }
  // My messages the server has not confirmed, then a sync request.
  std::string body;
  auto add = [&](const uint8_t* raw, const size_t len) {
    body += char(len);
    body.append(reinterpret_cast<const char*>(raw), len);
  };
  std::vector<uint32_t> mine;
  for (const auto& p : pending_) mine.push_back(p.counter);
  mine.insert(mine.end(), controlWaiting_.begin(), controlWaiting_.end());
  for (const uint32_t counter : mine) {
    for (const auto& raw : store_.packetsAfter(group_.me, counter - 1, group_.me, MAX_PIECES)) {
      Header h;
      if (readHeader(raw.data(), raw.size(), h) && h.counter == counter) add(raw.data(), raw.size());
    }
  }
  SyncReq req;
  for (uint8_t m = 1; m <= MAX_MEMBERS; m++) req.lastCounter[m - 1] = store_.lastCounter(m);
  req.battery = battery();
  req.build = buildByte();
  req.flags = syncFlags();
  req.rosterVersion = group_.rosterVersion;
  uint8_t sync[HEADER_SIZE + SYNC_REQ_MAX];
  Header h = header(Type::SyncReq, SERVER);
  h.hops = 0;
  writeHeader(h, sync);
  add(sync, HEADER_SIZE + writeSyncReq(req, sync + HEADER_SIZE));
  // Messages the chain says are missing.
  for (const auto& f : fetches_) {
    uint8_t raw[HEADER_SIZE + FETCH_SIZE];
    Header fh = header(Type::Fetch, SERVER);
    fh.hops = 0;
    writeHeader(fh, raw);
    writeFetch(f, raw + HEADER_SIZE);
    add(raw, sizeof(raw));
  }

  freeink::SecureHttpClient http;
  http.setCACert(group_.serverCert.c_str());  // only the Pi's own certificate is trusted
  http.setTimeout(10000);
  http.setReuse(false);
  if (!http.begin(group_.serverUrl + "/api/packets")) {
    wifiServerFailed_ = true;
    return;
  }
  http.addHeader("Authorization", "Bearer " + group_.serverToken);
  http.addHeader("Content-Type", "application/octet-stream");
  const int status = http.POST(body);
  if (status != 200) {
    LOG_ERR("MESH", "Server sync failed: %d", status);
    wifiServerFailed_ = true;
    return;
  }
  wifiServerFailed_ = false;
  serverAtMs_ = millis();
  lastCheckUnix_ = unixNow();
  fetches_.clear();  // asked; what came is in the answer
  // The server's clock, for a unit that has none (or one that drifted).
  const uint32_t serverTime = httpDate(http.getHeader("Date"));
  if (serverTime && (!unixNow() || std::abs(int64_t(serverTime) - int64_t(unixNow())) > 60)) {
    halClock.setUtc(serverTime);
  }
  const std::string& answer = http.getString();
  size_t i = 0, packets = 0;
  while (i < answer.size()) {
    const size_t n = uint8_t(answer[i]);
    if (n == 0 || i + 1 + n > answer.size()) break;
    handle(reinterpret_cast<const uint8_t*>(answer.data() + i + 1), n, 0, true);
    i += 1 + n;
    packets++;
  }
  LOG_INF("MESH", "Server sync: %u packets", unsigned(packets));
}

void Node::loop() {
  if (!group_.configured) return;
  const uint32_t now = millis();

  Received rx;
  while (radio::receive(rx)) handle(rx.data, rx.len, rx.rssi, false);
  if (nextCheckMs_ && int32_t(now - nextCheckMs_) >= 0) startCheck();

  if (wifi_ == WifiStep::Joining) {
    if (WiFi.status() == WL_CONNECTED) {
      runWifiSync();
      waddle::wifi::off();
      wifi_ = WifiStep::Idle;
      changes_++;
    } else if (now - wifiStartedMs_ > WIFI_JOIN_MS) {
      waddle::wifi::off();
      wifi_ = WifiStep::Idle;
      wifiServerFailed_ = true;
      changes_++;
    }
  }

  // Off-grid: the GPS runs and the position goes out every few minutes.
  if (MESH_SETTINGS.onTrip || sosActive_) {
    gps::wake();
    gps::poll();
    const gps::Fix& fix = gps::lastFix();
    if (fix.valid && !unixNow()) halClock.setUtc(fix.time);
  } else if (gps::awake()) {
    gps::sleep();
  }
  if (MESH_SETTINGS.onTrip && int32_t(now - nextPositionMs_) >= 0) {
    sendPosition(Type::Pos, false);
    uint32_t minutes = std::max<uint8_t>(MESH_SETTINGS.positionMinutes, 5);
    if (battery() < LOW_BATTERY) minutes *= 2;
    nextPositionMs_ = now + minutes * 60000UL;
  }
  if (sosActive_ && int32_t(now - nextSosMs_) >= 0) {
    sendPosition(Type::Sos, false);
    nextSosMs_ = now + SOS_REPEAT_MS;
  }

  retryPending();
  sendOutbox();
}

// --- Positions on the SD card ------------------------------------------------

void Node::savePositions() {
  uint8_t data[MAX_MEMBERS * (1 + POS_SIZE)] = {};
  for (int i = 0; i < MAX_MEMBERS; i++) {
    data[i * (1 + POS_SIZE)] = members_[i].hasPosition ? 1 : 0;
    writePosition(members_[i].position, data + i * (1 + POS_SIZE) + 1);
  }
  saveSealed(POSITIONS_PATH, std::vector<uint8_t>(data, data + sizeof(data)));
}

void Node::loadPositions() {
  std::vector<uint8_t> data;
  if (!loadSealed(POSITIONS_PATH, data) || data.size() != MAX_MEMBERS * (1 + POS_SIZE)) return;
  for (int i = 0; i < MAX_MEMBERS; i++) {
    members_[i].hasPosition = data[i * (1 + POS_SIZE)] != 0;
    readPosition(data.data() + i * (1 + POS_SIZE) + 1, POS_SIZE, members_[i].position);
  }
}

}  // namespace mesh
