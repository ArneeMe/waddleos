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

#include "MeshNodeInternal.h"

namespace mesh {

using namespace internal;

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
  loadNetwork(network_);
  groups_.load();
  senders_.load();
  store_.begin(network_.me);
  // Above the floor from joining, so a unit that gets a former member's number
  // never repeats their counters.
  if (network_.configured && network_.counterFloor > savedCounterLimit()) saveCounterLimit(network_.counterFloor);
  counters_ = std::make_unique<CounterReserve>(savedCounterLimit());
  loadPositions();
  pending_.clear();
  if (network_.configured) {
    // My messages nobody confirmed before the restart: try them again.
    for (uint8_t c = 0; c < CONVERSATIONS; c++) {
      if (!canWrite(c)) continue;
      for (const auto& m : store_.load(c, 20)) {
        if (m.sender != network_.myDevice() || m.stored || m.deliveredTo) continue;
        if (pending_.size() >= MAX_PENDING) pending_.erase(pending_.begin());
        pending_.push_back(Pending{m.counter, c, 0, millis() + 10000});
      }
    }
  }
  if (!network_.configured) {
    LOG_INF("MESH", "Not in a network yet: `waddle mesh join`. Public key %s", toHex(identity().publicKey, 8).c_str());
    return;
  }
  LOG_INF("MESH", "Member %u of network %u (roster %u)", network_.me, network_.id, network_.rosterVersion);
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
  if (!network_.configured) return;
  const uint32_t now = millis();
  // A radio that did not start (a loose cable, a cold start) gets another go.
  if (!radio::isUp()) radio::begin(params_);
  radio::resume();
  requestSync();
  // At home the server is one Wi-Fi hop away; on a trip Wi-Fi is rarely there.
  // Retries of unanswered messages still try Wi-Fi in both modes.
  if (!MESH_SETTINGS.onTrip && MESH_SETTINGS.homeWifi && !network_.serverUrl.empty() && battery() >= LOW_BATTERY &&
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
  h.network = network_.id;
  h.sender = network_.myDevice();
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
    if (store) store_.savePacket(network_.myDevice(), sealed->packets[i], sealed->lengths[i]);
    queue(sealed->packets[i], sealed->lengths[i], delayMs, false);
  }
  if (store) store_.confirmCounter(network_.myDevice(), h.counter);
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
  if (!network_.configured) return nullptr;
  if (conversation == CONV_EVERYONE) {
    // A phone after someone left: its unit makes the new key; until it comes
    // the old one is not used (the one who left has it).
    if (waitingForSenderKey_) return nullptr;
    if (keyNumber) *keyNumber = identity().senderKeyNumber;
    return identity().senderKey;
  }
  if (isGroupConversation(conversation)) {
    const ChatGroup& g = groups_.at(conversation - CONV_FIRST_GROUP);
    const GroupKey* newest = g.newest();
    if (!g.id || g.left || !(g.members & memberBit(network_.me)) || !newest) return nullptr;
    if (keyNumber) *keyNumber = newest->number;
    return newest->key;
  }
  const Member* m = network_.member(conversation);
  return m && m->present && conversation != network_.me ? m->dmKey : nullptr;
}

bool Node::canWrite(const uint8_t conversation) const { return sendingKey(conversation) != nullptr; }

uint8_t Node::recipientOf(const uint8_t conversation) const {
  if (conversation == CONV_EVERYONE) return EVERYONE;
  if (isGroupConversation(conversation)) return TO_GROUP;
  return conversation;
}

int Node::conversationOf(const Header& h) const {
  if (h.recipient == EVERYONE) return CONV_EVERYONE;
  if (h.recipient == TO_GROUP) {
    const int slot = groups_.find(h.groupId);
    return slot < 0 ? -1 : CONV_FIRST_GROUP + slot;
  }
  // My own messages (from this device or my other one) are in the chat with
  // their recipient.
  return person(h.sender) == network_.me ? h.recipient : person(h.sender);
}

bool Node::sendText(const uint8_t conversation, const std::string& text) {
  uint8_t keyNumber = 0;
  const uint8_t* key = sendingKey(conversation, &keyNumber);
  if (!key || text.empty() || text.size() > MAX_TEXT) return false;
  const uint8_t recipient = recipientOf(conversation);
  ChatMessage msg;
  msg.sender = network_.myDevice();
  msg.time = unixNow();
  store_.lastHash(conversation, network_.myDevice(), msg.prevHash);
  msg.text = text;

  std::string plain(MSG_PREFIX, '\0');
  put32(reinterpret_cast<uint8_t*>(&plain[0]), msg.time);
  memcpy(&plain[4], msg.prevHash, 4);
  plain += text;
  Header h = header(Type::Msg, recipient);
  h.key = keyNumber;
  if (recipient == TO_GROUP) h.groupId = groups_.at(conversation - CONV_FIRST_GROUP).id;
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
  bool told[MAX_DEVICES] = {};
  for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
    if (!isDevice(it->sender) || person(it->sender) == network_.me || told[deviceSlot(it->sender)]) continue;
    told[deviceSlot(it->sender)] = true;
    sendAck(person(it->sender), it->sender, it->counter, AckState::Read);
  }
  store_.markRead(conversation);
  changes_++;
}

void Node::sendPosition(const Type type, const bool cancelled) {
  if (!network_.configured) return;
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
  Header h = header(type, EVERYONE);
  h.key = identity().senderKeyNumber;
  if (type == Type::Sos) sosCounters_.push_back(h.counter);
  sendSealed(h, identity().senderKey, plain, sizeof(plain), false);
  if (fix.valid) {
    MemberStatus& self = members_[network_.me - 1];
    self.hasPosition = true;
    self.position = p;
    self.positionAtMs = gps::lastFixAtMs();
  }
}

void Node::startSos() {
  if (!network_.configured) return;
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
  if (!network_.configured || !radio::isUp()) return;
  SyncReq req;
  for (int slot = 0; slot < MAX_DEVICES; slot++) req.lastCounter[slot] = store_.lastCounter(slotDevice(slot));
  req.battery = battery();
  req.build = buildByte();
  req.flags = syncFlags();
  req.rosterVersion = network_.rosterVersion;
  uint8_t raw[HEADER_SIZE + SYNC_REQ_MAX];
  Header h = header(Type::SyncReq, EVERYONE);
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

bool Node::listeningForCheck() const { return checkUntilMs_ && int32_t(checkUntilMs_ - millis()) > 0; }

bool Node::busy() const {
  if (wifi_ != WifiStep::Idle || listeningForCheck()) return true;
  // Packets that can go out within a few seconds; later ones (a relay delay,
  // a full airtime budget) and a radio that is down do not keep the unit awake.
  if (!radio::isUp()) return false;
  const uint32_t now = millis();
  return std::any_of(outbox_.begin(), outbox_.end(), [&](const Outgoing& o) {
    return int32_t(o.dueMs - now) <= int32_t(BUSY_SOON_MS) &&
           budget_.allows(airtimeMs(params_, o.raw.size()), now);
  });
}

uint32_t Node::msUntilWork() const {
  const uint32_t now = millis();
  uint32_t next = 10UL * 60 * 1000;
  auto consider = [&](const uint32_t at) { next = std::min(next, at > now ? at - now : 0); };
  if (radio::isUp()) {
    for (const auto& o : outbox_) {
      const uint32_t airtime = airtimeMs(params_, o.raw.size());
      consider(budget_.allows(airtime, now) ? o.dueMs : now + AirtimeBudget::msUntilRetry(now));
    }
  }
  for (const auto& p : pending_) {
    if (p.tries < 3) consider(p.nextTryMs);
  }
  if (MESH_SETTINGS.onTrip) consider(nextPositionMs_);
  if (sosActive_) consider(nextSosMs_);
  return next;
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
  if (readHeader(raw.data(), raw.size(), h) && h.type == Type::Msg && h.sender == network_.myDevice() &&
      h.pieceIndex == h.pieceCount - 1) {
    store_.addState(conversationOfMine(h.counter), network_.myDevice(), h.counter, Delivery::Sent, 0);
    changes_++;
  }
}

void Node::retryPending() {
  const uint32_t now = millis();
  bool needWifi = false;
  for (auto& p : pending_) {
    if (p.tries >= 3 || int32_t(now - p.nextTryMs) < 0) continue;
    for (const auto& raw : store_.packetsAfter(network_.myDevice(), p.counter - 1, network_.me, MAX_PIECES)) {
      Header h;
      if (readHeader(raw.data(), raw.size(), h) && h.counter == p.counter) queue(raw.data(), raw.size(), 0, false);
    }
    p.tries++;
    p.nextTryMs = now + RETRY_MS[std::min<int>(p.tries, 2)];
    if (p.tries >= 3) needWifi = true;
  }
  // Nobody heard it over LoRa: try the server over Wi-Fi (not on low battery).
  if (needWifi && !network_.serverUrl.empty() && battery() >= LOW_BATTERY &&
      (!lastWifiSyncMs_ || now - lastWifiSyncMs_ > WIFI_FALLBACK_GAP_MS)) {
    startWifiSync();
  }
}

void Node::loop() {
  if (!network_.configured) return;
  const uint32_t now = millis();

  Received rx;
  while (radio::receive(rx)) handle(rx.data, rx.len, rx.rssi, false);
  // Done listening: cleared, so the time compare cannot turn true again when
  // millis() has moved on 24.8 days.
  if (checkUntilMs_ && !listeningForCheck()) checkUntilMs_ = 0;
  if (nextCheckMs_ && int32_t(now - nextCheckMs_) >= 0) startCheck();

  if (wifi_ == WifiStep::Joining) {
    if (WiFi.status() == WL_CONNECTED) {
      // A full answer means more is waiting (a unit away for long): ask again.
      for (int round = 0; round < 10 && runWifiSync() >= SERVER_ANSWER_FULL; round++) {
      }
      if (!wifiWasOn_) waddle::wifi::off();
      wifi_ = WifiStep::Idle;
      changes_++;
    } else if (now - wifiStartedMs_ > WIFI_JOIN_MS) {
      if (!wifiWasOn_) waddle::wifi::off();
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
