#include "MessageStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MeshCrypto.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mesh {

namespace {

constexpr const char* DIR = "/.mesh";
constexpr const char* READ_PATH = "/.mesh/read.txt";
constexpr size_t PREVIEW = 60;

// Chat log records.
constexpr uint8_t REC_MESSAGE = 'M';  // M sender counter(4) time(4) prev(4) flags len(2) text
constexpr uint8_t REC_STATE = 'S';    // S sender counter(4) state by
constexpr uint8_t REC_CHAIN_OK = 'W';  // W sender counter(4): its missing previous message came

std::string packetPath(const uint8_t sender) {
  char p[32];
  snprintf(p, sizeof(p), "/.mesh/packets/%u.bin", sender);
  return p;
}

std::string chatPath(const uint8_t conversation) {
  char p[32];
  snprintf(p, sizeof(p), "/.mesh/chat/%u.log", conversation);
  return p;
}

bool append(const std::string& path, const uint8_t* data, const size_t len) {
  HalFile f = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!f) {
    LOG_ERR("STORE", "cannot write %s", path.c_str());
    return false;
  }
  const bool ok = f.write(data, len) == len;
  f.close();
  return ok;
}

// Calls fn(header, raw, len) for every stored packet of sender.
template <typename Fn>
void forEachPacket(const uint8_t sender, Fn fn) {
  HalFile f = Storage.open(packetPath(sender).c_str(), O_RDONLY);
  if (!f) return;
  uint8_t raw[MAX_PACKET];
  while (true) {
    const int n = f.read();
    if (n <= 0 || n > int(MAX_PACKET)) break;
    if (f.read(raw, n) != n) break;
    Header h;
    if (readHeader(raw, n, h)) fn(h, raw, size_t(n));
  }
  f.close();
}

struct Record {
  uint8_t kind = 0;
  ChatMessage message;  // REC_MESSAGE
  uint8_t sender = 0;   // REC_STATE
  uint32_t counter = 0;
  Delivery state = Delivery::Sending;
  uint8_t by = 0;
};

bool readRecord(HalFile& f, Record& r) {
  const int kind = f.read();
  if (kind < 0) return false;
  r.kind = uint8_t(kind);
  uint8_t head[16];
  if (kind == REC_MESSAGE) {
    if (f.read(head, 16) != 16) return false;
    ChatMessage& m = r.message;
    m = ChatMessage{};
    m.sender = head[0];
    m.counter = get32(head + 1);
    m.time = get32(head + 5);
    memcpy(m.prevHash, head + 9, 4);
    m.chainWarning = (head[13] & 1) != 0;
    const uint16_t len = get16(head + 14);
    if (len > MAX_TEXT) return false;
    m.text.resize(len);
    return len == 0 || f.read(&m.text[0], len) == len;
  }
  if (kind == REC_CHAIN_OK) {
    if (f.read(head, 5) != 5) return false;
    r.sender = head[0];
    r.counter = get32(head + 1);
    return true;
  }
  if (kind == REC_STATE) {
    if (f.read(head, 7) != 7) return false;
    r.sender = head[0];
    r.counter = get32(head + 1);
    r.state = Delivery(head[5]);
    r.by = head[6];
    return true;
  }
  return false;
}

void applyState(ChatMessage& m, const Delivery state, const uint8_t by) {
  const uint8_t bit = isMember(by) ? uint8_t(1u << (by - 1)) : 0;
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
  Storage.mkdir("/.mesh/packets");
  Storage.mkdir("/.mesh/chat");
  for (uint8_t s = 1; s <= MAX_MEMBERS; s++) {
    uint32_t last = 0;
    forEachPacket(s, [&](const Header& h, const uint8_t*, size_t) {
      if (h.counter > last) last = h.counter;
    });
    lastCounter_[s - 1] = last;
  }
  if (Storage.exists(READ_PATH)) {
    const String text = Storage.readFile(READ_PATH);
    const char* p = text.c_str();
    for (int c = 0; c < CONVERSATIONS && *p; c++) {
      char* end = nullptr;
      seen_[c] = uint32_t(strtoul(p, &end, 10));
      if (end == p) break;
      p = end;
    }
  }
  for (uint8_t c = 0; c < CONVERSATIONS; c++) {
    ConversationSummary& sum = summaries_[c];
    sum = ConversationSummary{};
    uint32_t fromOthers = 0;
    HalFile f = Storage.open(chatPath(c).c_str(), O_RDONLY);
    if (!f) continue;
    Record r;
    while (readRecord(f, r)) {
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
    f.close();
    sum.unread = fromOthers > seen_[c] ? fromOthers - seen_[c] : 0;
  }
  (void)DIR;
}

void MessageStore::savePacket(const uint8_t sender, const uint8_t* raw, const size_t len) {
  if (!isMember(sender) || len > MAX_PACKET) return;
  uint8_t n = uint8_t(len);
  HalFile f = Storage.open(packetPath(sender).c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return;
  f.write(&n, 1);
  f.write(raw, len);
  f.close();
  Header h;
  if (readHeader(raw, len, h) && h.counter > lastCounter_[sender - 1]) lastCounter_[sender - 1] = h.counter;
}

bool MessageStore::hasPacket(const Header& h) {
  bool found = false;
  forEachPacket(h.sender, [&](const Header& s, const uint8_t*, size_t) {
    if (s.counter == h.counter && s.pieceIndex == h.pieceIndex && s.type == h.type) found = true;
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
  HalFile f = Storage.open(chatPath(conversation).c_str(), O_RDONLY);
  if (!f) return all;
  Record r;
  while (readRecord(f, r)) {
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
  f.close();
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
  HalFile f = Storage.open(chatPath(conversation).c_str(), O_RDONLY);
  if (!f) return false;
  Record r;
  bool found = false;
  while (!found && readRecord(f, r)) {
    found = r.kind == REC_MESSAGE && r.message.sender == sender && r.message.counter == counter;
  }
  f.close();
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
  memset(chain_[conversation], 0, sizeof(chain_[conversation]));
  memset(chainCounter_[conversation], 0, sizeof(chainCounter_[conversation]));
  saveSeen();
}

void MessageStore::lastHash(const uint8_t conversation, const uint8_t sender, uint8_t out[4]) const {
  if (conversation >= CONVERSATIONS || !isMember(sender)) {
    memset(out, 0, 4);
    return;
  }
  memcpy(out, chain_[conversation][sender - 1], 4);
}

void MessageStore::updateChain(const uint8_t conversation, const ChatMessage& m) {
  if (!isMember(m.sender) || m.counter < chainCounter_[conversation][m.sender - 1]) return;
  chainCounter_[conversation][m.sender - 1] = m.counter;
  messageHash(m, chain_[conversation][m.sender - 1]);
}

uint32_t MessageStore::lastChainCounter(const uint8_t conversation, const uint8_t sender) const {
  return conversation < CONVERSATIONS && isMember(sender) ? chainCounter_[conversation][sender - 1] : 0;
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
  std::string text;
  char n[12];
  for (const uint32_t s : seen_) {
    snprintf(n, sizeof(n), "%u ", unsigned(s));
    text += n;
  }
  Storage.writeFile(READ_PATH, String(text.c_str()));
}

}  // namespace mesh
