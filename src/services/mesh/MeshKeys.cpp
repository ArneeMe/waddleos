// The mesh node, part: Groups and keys: control DMs, group keys, sender keys, asking for a missing
// key, and the roster (protocol/PROTOCOL.md, "Control DMs", "Groups", "The roster").

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

// --- Groups and keys (protocol/PROTOCOL.md, "Control DMs", "Groups") ------------
//
// Only the remaining member with the lowest number makes a new group key, so
// two members never make one at once. Others ask it to (REMOVE), or it does so
// by itself when told someone left (LEAVE). Invitations from someone not in the
// group, or older than the key held, do not change who is in it.

namespace {
constexpr char INVITE = char(Control::Invite);  // the notice kinds kept per group (MeshGroups.h)
constexpr char LEAVE = char(Control::Leave);
constexpr char REMOVE = char(Control::Remove);
constexpr uint32_t ASK_KEY_AGAIN_MS = 6 * 3600 * 1000UL;
// Key DMs to many members: spread over this long (all units hear a new member
// list at once, and LoRa has no "wait until the air is free").
constexpr uint32_t KEY_SPREAD_MS = 60000;

uint8_t lowest(const Members members) {
  for (uint8_t n = 1; n <= MAX_MEMBERS; n++) {
    if (members & memberBit(n)) return n;
  }
  return 0;
}

std::string invitation(const ChatGroup& g, const GroupKey& k, const bool fresh) {
  ControlMsg c;
  c.kind = Control::Invite;
  c.groupId = g.id;
  c.keyNumber = k.number;
  c.fresh = fresh;
  c.members = g.members;
  memcpy(c.key, k.key, KEY_SIZE);
  c.name = g.name;
  return writeControl(c);
}
}  // namespace

uint32_t Node::sendControl(const uint8_t to, const std::string& payload, const uint32_t delayMs) {
  const Member* m = network_.member(to);
  if (!m || !m->present || to == network_.me) return 0;
  // A DM like any other (ACKs, retries, sync), never shown as a message.
  std::string plain(MSG_PREFIX, '\0');
  put32(reinterpret_cast<uint8_t*>(&plain[0]), unixNow());
  plain += payload;
  const Header h = header(Type::Msg, to);
  sendSealed(h, m->dmKey, reinterpret_cast<const uint8_t*>(plain.data()), plain.size(), true, delayMs);
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
  const Member* m = network_.member(to);
  if (!m || !m->present || to == network_.me) return;
  const Identity& mine = identity();
  ControlMsg c;
  c.kind = Control::SenderKey;
  c.keyNumber = mine.senderKeyNumber;
  memcpy(c.key, mine.senderKey, KEY_SIZE);
  senderKeyCounters_[to - 1] = sendControl(to, writeControl(c), delayMs);
}

void Node::sendToMember(const int slot, const uint8_t to, const char kind, const uint8_t about) {
  ChatGroup& g = groups_.at(slot);
  std::string p;
  if (kind == INVITE) {
    const GroupKey* k = g.newest();
    if (!k) return;
    p = invitation(g, *k, k->number == 1);
  } else {
    ControlMsg c;
    c.kind = Control(kind);
    c.groupId = g.id;
    c.member = about;
    p = writeControl(c);
  }
  // Remembered until their unit confirms it, and sent again on each check.
  g.setNotice(to, kind, about, sendControl(to, p));
}

void Node::rotate(const int slot) {
  ChatGroup& g = groups_.at(slot);
  const GroupKey* newest = g.newest();
  uint8_t key[KEY_SIZE];
  esp_fill_random(key, KEY_SIZE);
  g.addKey(uint8_t((newest ? newest->number : 0) + 1), network_.me, key);
  for (uint8_t n = 1; n <= MAX_MEMBERS; n++) {
    if (n != network_.me && g.has(n)) sendToMember(slot, n, INVITE, 0);
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
  if (!g.left && lowest(g.members) == network_.me) rotate(slot);
}

bool Node::handleControl(const uint8_t from, const std::string& text) {
  ControlMsg c;
  if (!readControl(text, c)) return true;  // not one we know: confirmed, never retried
  if (c.kind == Control::SenderKey) {
    if (senders_.add(from, c.keyNumber, c.key)) {
      senders_.save();
      LOG_INF("MESH", "Sender key %u from %u", c.keyNumber, from);
      openWaitingFrom(from);
    }
    changes_++;
    return true;
  }
  if (c.kind == Control::AskKey) {
    answerKeyRequest(from, c.groupId, c.keyNumber);
    return true;
  }
  const uint16_t id = c.groupId;
  if (!isMember(groupCreator(id))) return true;
  int slot = groups_.find(id);
  if (c.kind == Control::Invite) {
    if (!c.keyNumber || !(c.members & memberBit(from)) || !(c.members & memberBit(network_.me))) return true;
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
      groups_.at(slot).members = c.members;
      store_.clear(uint8_t(CONV_FIRST_GROUP + slot));
    } else if (c.fresh && creator && groups_.at(slot).key(1) &&
               memcmp(groups_.at(slot).key(1), c.key, KEY_SIZE) != 0) {
      // The creator used an id again (they had left the old group).
      groups_.at(slot) = ChatGroup{};
      groups_.at(slot).id = id;
      groups_.at(slot).members = c.members;
      store_.clear(uint8_t(CONV_FIRST_GROUP + slot));
    }
    ChatGroup& g = groups_.at(slot);
    // Only a member can change the group, and only with a newer key.
    if (!g.has(from)) return true;
    const GroupKey* newest = g.newest();
    const uint8_t held = newest ? newest->number : 0;
    const bool replaced = g.addKey(c.keyNumber, from, c.key);
    if (c.keyNumber > held || (c.keyNumber == held && replaced)) {
      g.members = c.members;
      g.left = false;  // added (again) with a key made after I left
      if (!c.name.empty()) strlcpy(g.name, c.name.c_str(), sizeof(g.name));
    }
    groups_.save();
    LOG_INF("MESH", "Group %04x: key %u from %u", id, c.keyNumber, from);
    openWaiting(id);
    changes_++;
    return true;
  }
  if (slot < 0) return true;
  ChatGroup& g = groups_.at(slot);
  if (!g.has(from)) return true;
  if (c.kind == Control::Leave) {
    takeOut(slot, from);
  } else if (c.kind == Control::Remove) {
    // Asked to take someone out: only the lowest of those who stay does it.
    if (!isMember(c.member) || c.member == network_.me || !g.has(c.member)) return true;
    if (lowest(g.members & ~memberBit(c.member)) == network_.me) takeOut(slot, c.member);
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
  const Member* from = network_.member(h.sender);
  if (!network_.configured || !from || !from->present || !h.key) return;
  if (h.recipient == EVERYONE) {
    askForKey(h.sender, 0, h.key);
  } else if (h.recipient == TO_GROUP) {
    const int slot = groups_.find(h.groupId);
    if (slot < 0) {
      // Its invitation never came: the creator sends it again if I am in it.
      // Its counter counts, so syncs move past groups I am not in.
      store_.confirmCounter(h.sender, h.counter);
      if (isMember(groupCreator(h.groupId)) && groupCreator(h.groupId) != network_.me) {
        askForKey(groupCreator(h.groupId), h.groupId, h.key);
      }
      return;
    }
    const ChatGroup& g = groups_.at(slot);
    if (!g.left && g.has(network_.me) && g.has(h.sender) && !g.key(h.key)) askForKey(h.sender, h.groupId, h.key);
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
  ControlMsg c;
  c.kind = Control::AskKey;
  c.groupId = groupId;
  c.keyNumber = number;
  sendControl(to, writeControl(c));
}

void Node::answerKeyRequest(const uint8_t from, const uint16_t groupId, const uint8_t number) {
  if (!groupId) {
    // My sender key: sent again (and no longer counted as confirmed).
    if (number != identity().senderKeyNumber) return;
    senderKeyConfirmed_ &= ~memberBit(from);
    saveKeyConfirmations();
    sendSenderKey(from);
    return;
  }
  const int slot = groups_.find(groupId);
  if (slot < 0) return;
  ChatGroup& g = groups_.at(slot);
  if (g.left || !g.has(from) || !g.has(network_.me)) return;
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

bool Node::resendStored(const uint32_t counter, const uint32_t delayMs) {
  // The same packet again (same counter): no new copy on the card or the server.
  const auto pieces = counter ? store_.piecesOf(network_.me, counter) : std::vector<std::vector<uint8_t>>{};
  for (const auto& raw : pieces) queue(raw.data(), raw.size(), delayMs, false);
  return !pieces.empty();
}

void Node::resendUnconfirmed() {
  if (!network_.configured) return;
  // At most every half hour, however often the unit checks.
  const uint32_t now = millis();
  if (lastResendMs_ && now - lastResendMs_ < RESEND_GAP_MS) return;
  lastResendMs_ = now;
  // My sender key, to everyone who has not confirmed it.
  for (uint8_t n = 1; n <= MAX_MEMBERS; n++) {
    if (n != network_.me && network_.member(n)->present && !(senderKeyConfirmed_ & memberBit(n))) {
      const uint32_t delay = randomBetween(0, KEY_SPREAD_MS);
      if (!resendStored(senderKeyCounters_[n - 1], delay)) sendSenderKey(n, delay);
    }
  }
  bool changed = false;
  for (int slot = 0; slot < MAX_GROUPS; slot++) {
    ChatGroup& g = groups_.at(slot);
    if (!g.id) continue;
    for (Notice& n : g.notices) {
      if (!n.member) continue;
      const Member* m = network_.member(n.member);
      if (!m || !m->present) {
        n = Notice{};  // gone from the network
        changed = true;
      } else if (!resendStored(n.counter, randomBetween(0, KEY_SPREAD_MS))) {
        sendToMember(slot, n.member, char(n.kind), n.about);
        changed = true;
      }
    }
  }
  if (changed) groups_.save();
}

void Node::openWaiting(const uint16_t groupId) {
  // Group messages that came before the key: kept as packets, opened now.
  Reassembler pieces;
  for (uint8_t s = 1; s <= MAX_MEMBERS; s++) {
    if (s == network_.me) continue;
    for (const auto& raw : store_.packetsTo(s, TO_GROUP)) {
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
  for (const auto& raw : store_.packetsTo(sender, EVERYONE)) {
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

int Node::createGroup(const Members others, const std::string& name) {
  const Members members = (others & network_.everyone()) | memberBit(network_.me);
  if (!network_.configured || memberCount(members) < 2) return -1;
  const uint16_t id = groups_.nextId(network_.me);
  if (!id) return -1;
  const int slot = groups_.take(id);
  if (slot < 0) return -1;  // 16 groups already
  ChatGroup& g = groups_.at(slot);
  g = ChatGroup{};
  g.id = id;
  g.members = members;
  strlcpy(g.name, name.c_str(), sizeof(g.name));
  store_.clear(uint8_t(CONV_FIRST_GROUP + slot));
  rotate(slot);  // key number 1 (marked new), and the invitations
  changes_++;
  return CONV_FIRST_GROUP + slot;
}

void Node::leaveGroup(const uint8_t conversation) {
  if (!isGroupConversation(conversation)) return;
  const int slot = conversation - CONV_FIRST_GROUP;
  ChatGroup& g = groups_.at(slot);
  if (!g.id || g.left) return;
  for (uint8_t n = 1; n <= MAX_MEMBERS; n++) {
    if (n != network_.me && g.has(n)) sendToMember(slot, n, LEAVE, 0);
  }
  g.left = true;
  groups_.save();
  changes_++;
}

void Node::removeFromGroup(const uint8_t conversation, const uint8_t member) {
  if (!isGroupConversation(conversation) || member == network_.me) return;
  const int slot = conversation - CONV_FIRST_GROUP;
  ChatGroup& g = groups_.at(slot);
  if (!g.id || g.left || !g.has(member)) return;
  const uint8_t keeper = lowest(g.members & ~memberBit(member));
  if (keeper == network_.me) {
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
      if (g.has(n) && !(network_.everyone() & memberBit(n))) takeOut(slot, n);
    }
  }
}

// --- The roster (protocol/PROTOCOL.md, "The roster") -----------------------------

void Node::handleRoster(const Header& h, const uint8_t* cipher, const size_t len) {
  if (h.sender != SERVER || h.recipient != EVERYONE || len <= TAG_SIZE) return;
  std::vector<uint8_t> plain(len - TAG_SIZE);
  if (!open(h, network_.networkKey, cipher, len, plain.data())) return;
  const Members before = network_.everyone();
  const bool changed = applyRoster(network_, plain.data(), plain.size());
  // The pieces of the roster I have now, to pass on (handleSyncReq).
  if (h.counter == ROSTER_COUNTER + network_.rosterVersion && rosterPieces_.size() == h.pieceCount) {
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
    if (h.counter != ROSTER_COUNTER + network_.rosterVersion) return {};  // an older one
    out.emplace_back(framed.begin() + long(at + 1), framed.begin() + long(at + 1 + n));
    at += 1 + n;
  }
  return out;
}

void Node::rosterChanged(const Members before) {
  if (!network_.configured) {
    LOG_INF("MESH", "Taken out of the network");
    changes_++;
    return;
  }
  const Members now = network_.everyone();
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
    if (n != network_.me && (now & memberBit(n)) && !(senderKeyConfirmed_ & memberBit(n))) {
      sendSenderKey(n, randomBetween(1000, KEY_SPREAD_MS));
    }
  }
  changes_++;
}

void Node::loadKeyConfirmations() {
  Preferences prefs;
  prefs.begin("waddlemesh", true);
  // Confirmations count only for the sender key they were for.
  const bool sameKey = prefs.getUInt("keySentNum", 0) == identity().senderKeyNumber;
  senderKeyConfirmed_ = sameKey ? prefs.getUInt("keySent", 0) : 0;
  prefs.end();
}

void Node::saveKeyConfirmations() {
  Preferences prefs;
  prefs.begin("waddlemesh", false);
  prefs.putUInt("keySent", senderKeyConfirmed_);
  prefs.putUInt("keySentNum", identity().senderKeyNumber);
  prefs.end();
}

}  // namespace mesh
