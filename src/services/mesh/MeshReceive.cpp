// The mesh node, part: Receiving: packets heard over LoRa or from the server, ACKs, and answering
// other units' sync requests and fetches.

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

// --- Receiving ----------------------------------------------------------------

const uint8_t* Node::keyFor(const Header& h) const {
  const Member* from = network_.member(h.sender);
  if (!from || !from->present) return nullptr;
  const bool mine = person(h.sender) == network_.me;  // from my other device
  if (h.recipient == EVERYONE) {
    // My other device sends with my own sender key; others' are kept until their key comes.
    if (mine) return h.key == identity().senderKeyNumber ? identity().senderKey : nullptr;
    return senders_.key(person(h.sender), h.key);
  }
  if (h.recipient == network_.me) return from->dmKey;
  if (h.recipient == TO_GROUP) {
    // Not (yet) mine: the packet is kept until the key comes. A member who was
    // taken out can no longer write.
    const int slot = groups_.find(h.groupId);
    if (slot < 0 || !groups_.at(slot).has(person(h.sender))) return nullptr;
    return groups_.at(slot).key(h.key);
  }
  // My other device's DM to someone: our DM key with them.
  if (mine && isMember(h.recipient)) {
    const Member* to = network_.member(h.recipient);
    return to && to->present ? to->dmKey : nullptr;
  }
  return nullptr;  // a DM between two others
}

void Node::handle(const uint8_t* raw, const size_t len, const int rssi, const bool viaWifi) {
  Header h;
  if (!readHeader(raw, len, h) || h.network != network_.id || h.sender == network_.myDevice()) return;
  const uint32_t now = millis();
  if (listeningForCheck() || viaWifi) lastCheckUnix_ = unixNow();
  if (h.sender == SERVER) {
    serverAtMs_ = now;
  } else if (!viaWifi && (h.hops == DEFAULT_HOPS || h.type == Type::SyncReq)) {
    // Straight from their radio (not relayed, not a stored copy): they are near.
    members_[person(h.sender) - 1].heardAtMs = now;
    members_[person(h.sender) - 1].rssi = rssi;
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
        canRead(network_.me, h.sender, h.recipient) && isDevice(h.sender) && haveMessage(h)) {
      sendAck(person(h.sender), h.sender, h.counter, AckState::Delivered);
    }
    return;
  }

  // Pass it on so units out of each other's range still hear it.
  if (!viaWifi && h.hops > 0 && h.type != Type::SyncReq && h.recipient != network_.me) {
    std::vector<uint8_t> copy(raw, raw + len);
    copy[3] = uint8_t((copy[3] & 0xF0) | (h.hops - 1));
    queue(copy.data(), copy.size(), randomBetween(500, 2500), true);
  }

  const uint8_t* body = raw + HEADER_SIZE;
  const size_t bodyLen = len - HEADER_SIZE;
  switch (h.type) {
    case Type::Msg: {
      if (!isDevice(h.sender) || store_.hasPacket(h, raw, len)) return;
      store_.savePacket(h.sender, raw, len);  // kept to help other units catch up
      // Heard over the radio away from home (no word from the server lately):
      // the server may not have it.
      if (!viaWifi && serverState() != ServerState::Online) store_.keepForUpload(raw, len);
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
      if (h.sender != SERVER || h.counter < ROSTER_COUNTER + network_.rosterVersion) break;
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
  // A group invitation or a member leaving, not a message to show. (My other
  // device's control DMs to others come too: nothing to do with them here.)
  if (isControl(m.text)) {
    // Not confirmed when it cannot be taken yet: it comes again at their next check.
    if (h.recipient == network_.me && handleControl(person(h.sender), m.text)) {
      sendAck(person(h.sender), h.sender, h.counter, AckState::Delivered);
    }
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
  // My other device's own messages need no "delivered" from me.
  if (person(h.sender) != network_.me) sendAck(person(h.sender), h.sender, h.counter, AckState::Delivered);
  // The radio's clock source when the unit has none yet.
  if (!unixNow() && m.time > 1600000000) halClock.setUtc(m.time);
}

void Node::handlePosition(const Header& h, const uint8_t* cipher, const size_t len) {
  const Member* from = network_.member(h.sender);
  const uint8_t* key = senders_.key(person(h.sender), h.key);
  if (!from || !from->present || person(h.sender) == network_.me || h.recipient != EVERYONE ||
      len != POS_SIZE + TAG_SIZE) {
    return;
  }
  if (!key) {
    missingKey(h);
    return;
  }
  uint8_t plain[POS_SIZE];
  if (!open(h, key, cipher, len, plain)) return;
  Position p;
  if (!readPosition(plain, sizeof(plain), p)) return;
  MemberStatus& st = members_[person(h.sender) - 1];
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
      if (!st.sos) newSos_ = person(h.sender);
      st.sos = true;
      st.sosPosition = p;
      st.sosCounter = h.counter;
      // Every repeat is answered, so a lost answer is made up a minute later.
      sendAck(person(h.sender), h.sender, h.counter, AckState::Delivered);
    }
  }
}

uint8_t Node::conversationOfMine(const uint32_t counter) {
  for (const auto& p : pending_) {
    if (p.counter == counter) return p.conversation;
  }
  // Older messages: look at the stored packet's recipient.
  const auto raw = store_.packetsAfter(network_.myDevice(), counter - 1, network_.me, 1);
  Header h;
  if (!raw.empty() && readHeader(raw[0].data(), raw[0].size(), h) && h.counter == counter) {
    const int c = conversationOf(h);
    return c < 0 ? CONV_EVERYONE : uint8_t(c);
  }
  return CONV_EVERYONE;
}

void Node::handleAck(const Header& h, const uint8_t* body, const size_t len) {
  Ack ack;
  if (!readAck(body, len, ack)) return;
  // My other device read someone's message: read here too (PHONE.md, R10).
  if (isDevice(h.sender) && person(h.sender) == network_.me && h.sender != network_.myDevice()) {
    if (ack.state == AckState::Read && isDevice(ack.sender)) readOnOtherDevice(ack.sender, ack.counter);
    return;
  }
  if (ack.sender != network_.myDevice()) return;
  controlWaiting_.erase(std::remove(controlWaiting_.begin(), controlWaiting_.end(), ack.counter), controlWaiting_.end());
  // An answer to my SOS: who has it, and who has seen it.
  if (std::find(sosCounters_.begin(), sosCounters_.end(), ack.counter) != sosCounters_.end()) {
    if (h.sender == SERVER) {
      sosOnServer_ = true;
    } else {
      sosReceivedBy_ |= memberBit(person(h.sender));
      if (ack.state == AckState::Read) sosSeenBy_ |= memberBit(person(h.sender));
    }
    return;
  }
  if (confirmControl(person(h.sender), ack.counter)) {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Pending& p) { return p.counter == ack.counter; }),
                   pending_.end());
    return;  // not a chat message: no "delivered" in the chat
  }
  const uint8_t conversation = conversationOfMine(ack.counter);
  const Delivery state = ack.state == AckState::Stored ? Delivery::Stored
                         : ack.state == AckState::Read ? Delivery::Read
                                                       : Delivery::Delivered;
  store_.addState(conversation, network_.myDevice(), ack.counter, state, person(h.sender));
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                [&](const Pending& p) { return p.counter == ack.counter; }),
                 pending_.end());
}

void Node::readOnOtherDevice(const uint8_t sender, const uint32_t counter) {
  const auto pieces = store_.piecesOf(sender, counter);
  Header h;
  if (pieces.empty() || !readHeader(pieces[0].data(), pieces[0].size(), h)) return;
  const int conversation = conversationOf(h);
  if (conversation < 0 || !store_.summary(uint8_t(conversation)).unread) return;
  store_.markRead(uint8_t(conversation));  // "read" covers the earlier ones too
  changes_++;
}

void Node::handleSyncReq(const Header& h, const uint8_t* body, const size_t len) {
  SyncReq req;
  if (!isDevice(h.sender) || !readSyncReq(body, len, req)) return;
  if (req.battery && !isPhone(h.sender)) members_[person(h.sender) - 1].battery = req.battery;
  size_t sent = 0;
  // Their roster is older (it may take someone out): it goes first, whole.
  if (req.rosterVersion < network_.rosterVersion) {
    for (const auto& raw : savedRosterPackets()) {
      std::vector<uint8_t> copy = raw;
      copy[3] &= 0xF0;
      queue(copy.data(), copy.size(), randomBetween(1500, 4000) + uint32_t(sent) * 300, true);
      sent++;
    }
  }
  const bool listedOnly = (req.flags & SYNC_LISTED_ONLY) != 0;
  for (int slot = 0; slot < MAX_DEVICES && sent < MAX_SYNC_ANSWER; slot++) {
    const uint8_t s = slotDevice(slot);
    if (s == h.sender || (listedOnly && !req.lastCounter[slot])) continue;
    for (const auto& raw : store_.packetsAfter(s, req.lastCounter[slot], person(h.sender), MAX_SYNC_ANSWER - sent)) {
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
  if (!isDevice(h.sender) || !readFetch(body, len, f) || f.sender == h.sender) return;
  size_t sent = 0;
  for (const auto& raw : store_.packetsAfter(f.sender, f.after, person(h.sender), MAX_SYNC_ANSWER)) {
    Header ph;
    if (!readHeader(raw.data(), raw.size(), ph) || ph.counter >= f.before) continue;
    std::vector<uint8_t> copy = raw;
    copy[3] &= 0xF0;
    queue(copy.data(), copy.size(), randomBetween(1500, 4000) + uint32_t(sent) * 300, true);
    sent++;
  }
}

void Node::requestFetch(const uint8_t sender, const uint32_t after, const uint32_t before) {
  if (!isDevice(sender) || before <= after + 1) return;
  const Fetch f{sender, after, before};
  // Over LoRa now (the server through the gateway, or another unit answers),
  // and over Wi-Fi with the next sync.
  if (fetches_.size() < 8) fetches_.push_back(f);
  if (!radio::isUp()) return;
  uint8_t raw[HEADER_SIZE + FETCH_SIZE];
  Header h = header(Type::Fetch, EVERYONE);
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

}  // namespace mesh
