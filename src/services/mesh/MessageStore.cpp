#include "MessageStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MeshCrypto.h>

#include "MeshConfig.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mesh {

namespace {

constexpr const char* READ_PATH = "/.mesh/read.bin";
constexpr const char* LAST_PATH = "/.mesh/last.bin";
constexpr size_t PREVIEW = 60;

// Chat log records.
constexpr uint8_t REC_MESSAGE = 'M';  // M sender counter(4) time(4) prev(4) flags len(2) text
constexpr uint8_t REC_STATE = 'S';    // S sender counter(4) state by
constexpr uint8_t REC_CHAIN_OK = 'W';  // W sender counter(4): its missing previous message came

std::string packetPath(const uint8_t sender) {
  char p[32];
  snprintf(p, sizeof(p), "/.mesh/packets/%u.sealed", sender);
  return p;
}

std::string chatPath(const uint8_t conversation) {
  char p[32];
  snprintf(p, sizeof(p), "/.mesh/chat/%u.sealed", conversation);
  return p;
}

bool append(const std::string& path, const uint8_t* data, const size_t len) {
  return appendSealed(path.c_str(), data, len);
}

// Calls fn(header, raw, len) for every stored packet of sender.
template <typename Fn>
void forEachPacket(const uint8_t sender, Fn fn) {
  SealedLog log(packetPath(sender).c_str());
  std::vector<uint8_t> raw;
  while (log.next(raw)) {
    Header h;
    if (raw.size() <= MAX_PACKET && readHeader(raw.data(), raw.size(), h)) fn(h, raw.data(), raw.size());
  }
}

struct Record {
  uint8_t kind = 0;
  ChatMessage message;  // REC_MESSAGE
  uint8_t sender = 0;   // REC_STATE
  uint32_t counter = 0;
  Delivery state = Delivery::Sending;
  uint8_t by = 0;
};

// The next record of a chat log; false at the end.
bool readRecord(SealedLog& log, Record& r) {
  std::vector<uint8_t> rec;
  if (!log.next(rec) || rec.empty()) return false;
  const int kind = rec[0];
  r.kind = uint8_t(kind);
  const uint8_t* head = rec.data() + 1;
  const size_t size = rec.size() - 1;
  if (kind == REC_MESSAGE) {
    if (size < 16) return false;
    ChatMessage& m = r.message;
    m = ChatMessage{};
    m.sender = head[0];
    m.counter = get32(head + 1);
    m.time = get32(head + 5);
    memcpy(m.prevHash, head + 9, 4);
    m.chainWarning = (head[13] & 1) != 0;
    const uint16_t len = get16(head + 14);
    if (len > MAX_TEXT || size != 16u + len) return false;
    m.text.assign(reinterpret_cast<const char*>(head + 16), len);
    return true;
  }
  if (kind == REC_CHAIN_OK) {
    if (size != 5) return false;
    r.sender = head[0];
    r.counter = get32(head + 1);
    return true;
  }
  if (kind == REC_STATE) {
    if (size != 7) return false;
    r.sender = head[0];
    r.counter = get32(head + 1);
    r.state = Delivery(head[5]);
    r.by = head[6];
    return true;
  }
  return false;
}

void applyState(ChatMessage& m, const Delivery state, const uint8_t by) {
  const Members bit = memberBit(by);
  switch (state) {
    case Delivery::Sent:
      m.sent = true;
      break;
    case Delivery::Stored:
      m.sent = m.stored = true;
      break;
    case Delivery::Delivered:
      m.sent = true;
      m.deliveredTo |= bit;
      break;
    case Delivery::Read:
      m.sent = true;
      m.deliveredTo |= bit;
      m.readBy |= bit;
      break;
    default:
      break;
  }
}

}  // namespace

void messageHash(const ChatMessage& m, uint8_t out[4]) {
  std::string plain(MSG_PREFIX, '\0');
  put32(reinterpret_cast<uint8_t*>(&plain[0]), m.time);
  memcpy(&plain[4], m.prevHash, 4);
  plain += m.text;
  shortHash(reinterpret_cast<const uint8_t*>(plain.data()), plain.size(), out);
}

void MessageStore::begin(const uint8_t me) {
  me_ = me;
  chain_.clear();
  Storage.mkdir("/.mesh/packets");
  Storage.mkdir("/.mesh/chat");
  std::vector<uint8_t> last;
  memset(lastCounter_, 0, sizeof(lastCounter_));
  if (loadSealed(LAST_PATH, last) && last.size() == sizeof(lastCounter_)) {
    memcpy(lastCounter_, last.data(), sizeof(lastCounter_));
  }
  std::vector<uint8_t> seen;
  if (loadSealed(READ_PATH, seen) && seen.size() == sizeof(seen_)) memcpy(seen_, seen.data(), sizeof(seen_));
  for (uint8_t c = 0; c < CONVERSATIONS; c++) {
    ConversationSummary& sum = summaries_[c];
    sum = ConversationSummary{};
    uint32_t fromOthers = 0;
    SealedLog log(chatPath(c).c_str());
    if (!log) continue;
    Record r;
    while (readRecord(log, r)) {
      if (r.kind != REC_MESSAGE) continue;
      const ChatMessage& m = r.message;
      sum.count++;
      if (m.sender != me_) fromOthers++;
      if (m.time >= sum.lastTime) {
        sum.lastTime = m.time;
        sum.lastSender = m.sender;
        sum.lastText = m.text.substr(0, PREVIEW);
      }
      updateChain(c, m);
    }
    sum.unread = fromOthers > seen_[c] ? fromOthers - seen_[c] : 0;
  }
}

void MessageStore::savePacket(const uint8_t sender, const uint8_t* raw, const size_t len) {
  if (!isMember(sender) || len > MAX_PACKET) return;
  append(packetPath(sender), raw, len);
}

void MessageStore::confirmCounter(const uint8_t sender, const uint32_t counter) {
  if (!isMember(sender) || counter <= lastCounter_[sender - 1]) return;
  lastCounter_[sender - 1] = counter;
  const auto* bytes = reinterpret_cast<const uint8_t*>(lastCounter_);
  saveSealed(LAST_PATH, std::vector<uint8_t>(bytes, bytes + sizeof(lastCounter_)));
}

bool MessageStore::hasPacket(const Header& h, const uint8_t* raw, const size_t len) {
  bool found = false;
  forEachPacket(h.sender, [&](const Header& s, const uint8_t* stored, const size_t storedLen) {
    // The same packet: everything but the hops byte (3), which relays change.
    if (found || s.counter != h.counter || s.pieceIndex != h.pieceIndex || s.type != h.type || storedLen != len) return;
    found = memcmp(stored, raw, 3) == 0 && memcmp(stored + 4, raw + 4, len - 4) == 0;
  });
  return found;
}

std::vector<std::vector<uint8_t>> MessageStore::packetsAfter(const uint8_t sender, const uint32_t after,
                                                             const uint8_t readableBy, const size_t limit) {
  std::vector<std::vector<uint8_t>> out;
  forEachPacket(sender, [&](const Header& h, const uint8_t* raw, const size_t len) {
    if (out.size() >= limit || h.type != Type::Msg || h.counter <= after) return;
    if (!canRead(readableBy, h.sender, h.recipient)) return;
    out.emplace_back(raw, raw + len);
  });
  return out;
}

std::vector<std::vector<uint8_t>> MessageStore::packetsTo(const uint8_t sender, const uint8_t recipient) {
  std::vector<std::vector<uint8_t>> out;
  forEachPacket(sender, [&](const Header& h, const uint8_t* raw, const size_t len) {
    if (h.type == Type::Msg && h.recipient == recipient) out.emplace_back(raw, raw + len);
  });
  return out;
}

void MessageStore::addMessage(const uint8_t conversation, const ChatMessage& m) {
  if (conversation >= CONVERSATIONS) return;
  std::vector<uint8_t> rec(17 + m.text.size());
  rec[0] = REC_MESSAGE;
  rec[1] = m.sender;
  put32(&rec[2], m.counter);
  put32(&rec[6], m.time);
  memcpy(&rec[10], m.prevHash, 4);
  rec[14] = m.chainWarning ? 1 : 0;
  put16(&rec[15], uint16_t(m.text.size()));
  memcpy(&rec[17], m.text.data(), m.text.size());
  append(chatPath(conversation), rec.data(), rec.size());

  ConversationSummary& sum = summaries_[conversation];
  sum.count++;
  if (m.sender != me_) sum.unread++;
  // A late message (caught up from others) does not replace the newest one.
  if (m.time >= sum.lastTime) {
    sum.lastTime = m.time;
    sum.lastSender = m.sender;
    sum.lastText = m.text.substr(0, PREVIEW);
  }
  updateChain(conversation, m);
}

void MessageStore::addState(const uint8_t conversation, const uint8_t sender, const uint32_t counter,
                            const Delivery state, const uint8_t byMember) {
  if (conversation >= CONVERSATIONS) return;
  uint8_t rec[8] = {REC_STATE, sender};
  put32(rec + 2, counter);
  rec[6] = uint8_t(state);
  rec[7] = byMember;
  append(chatPath(conversation), rec, sizeof(rec));
}

std::vector<ChatMessage> MessageStore::load(const uint8_t conversation, const size_t limit) const {
  std::vector<ChatMessage> all;
  SealedLog log(chatPath(conversation).c_str());
  if (!log) return all;
  Record r;
  while (readRecord(log, r)) {
    if (r.kind == REC_MESSAGE) {
      all.push_back(std::move(r.message));
      // Keep memory bounded on long histories: drop the oldest half when far over.
      if (all.size() > limit * 2) all.erase(all.begin(), all.begin() + long(all.size() - limit));
    } else if (r.kind == REC_CHAIN_OK) {
      for (auto& m : all) {
        if (m.sender == r.sender && m.counter == r.counter) m.chainWarning = false;
      }
    } else if (r.kind == REC_STATE) {
      // A read or delivered message also means every earlier one from that sender.
      for (auto it = all.rbegin(); it != all.rend(); ++it) {
        if (it->sender != r.sender || it->counter > r.counter) continue;
        if (it->counter == r.counter || r.state == Delivery::Read || r.state == Delivery::Delivered) {
          applyState(*it, r.state, r.by);
        }
        if (r.state != Delivery::Read && r.state != Delivery::Delivered) break;
      }
    }
  }
  // In the order they were written, not the order they arrived (catching up
  // from several units mixes them).
  std::stable_sort(all.begin(), all.end(), [](const ChatMessage& a, const ChatMessage& b) {
    if (a.time != b.time) return a.time < b.time;
    if (a.sender != b.sender) return a.sender < b.sender;
    return a.counter < b.counter;
  });
  if (all.size() > limit) all.erase(all.begin(), all.begin() + long(all.size() - limit));
  return all;
}

bool MessageStore::hasMessage(const uint8_t conversation, const uint8_t sender, const uint32_t counter) const {
  SealedLog log(chatPath(conversation).c_str());
  Record r;
  bool found = false;
  while (!found && readRecord(log, r)) {
    found = r.kind == REC_MESSAGE && r.message.sender == sender && r.message.counter == counter;
  }
  return found;
}

void MessageStore::clearWarning(const uint8_t conversation, const uint8_t sender, const uint32_t counter) {
  if (conversation >= CONVERSATIONS) return;
  uint8_t rec[6] = {REC_CHAIN_OK, sender};
  put32(rec + 2, counter);
  append(chatPath(conversation), rec, sizeof(rec));
}

void MessageStore::clear(const uint8_t conversation) {
  if (conversation >= CONVERSATIONS) return;
  Storage.remove(chatPath(conversation).c_str());
  summaries_[conversation] = ConversationSummary{};
  seen_[conversation] = 0;
  for (auto it = chain_.begin(); it != chain_.end();) {
    it = (it->first >> 8) == conversation ? chain_.erase(it) : std::next(it);
  }
  saveSeen();
}

void MessageStore::lastHash(const uint8_t conversation, const uint8_t sender, uint8_t out[4]) const {
  const auto it = chain_.find(uint16_t(conversation << 8 | sender));
  if (it == chain_.end()) {
    memset(out, 0, 4);
    return;
  }
  memcpy(out, it->second.hash, 4);
}

void MessageStore::updateChain(const uint8_t conversation, const ChatMessage& m) {
  if (!isMember(m.sender) || conversation >= CONVERSATIONS) return;
  ChainLink& link = chain_[uint16_t(conversation << 8 | m.sender)];
  if (m.counter < link.counter) return;
  link.counter = m.counter;
  messageHash(m, link.hash);
}

uint32_t MessageStore::lastChainCounter(const uint8_t conversation, const uint8_t sender) const {
  const auto it = chain_.find(uint16_t(conversation << 8 | sender));
  return it == chain_.end() ? 0 : it->second.counter;
}

uint32_t MessageStore::totalUnread() const {
  uint32_t n = 0;
  for (const auto& s : summaries_) n += s.unread;
  return n;
}

void MessageStore::markRead(const uint8_t conversation) {
  if (conversation >= CONVERSATIONS || summaries_[conversation].unread == 0) return;
  seen_[conversation] += summaries_[conversation].unread;
  summaries_[conversation].unread = 0;
  saveSeen();
}

void MessageStore::saveSeen() {
  const auto* bytes = reinterpret_cast<const uint8_t*>(seen_);
  saveSealed(READ_PATH, std::vector<uint8_t>(bytes, bytes + sizeof(seen_)));
}

}  // namespace mesh
