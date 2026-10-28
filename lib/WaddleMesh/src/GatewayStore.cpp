#include "GatewayStore.h"

#include <algorithm>
#include <cstring>

namespace mesh {

GatewayStore::GatewayStore(const uint8_t group, const uint32_t savedCounter)
    : group_(group), counters_(savedCounter < FIRST_COUNTER ? FIRST_COUNTER : savedCounter) {}

bool GatewayStore::ours(const uint8_t* raw, const size_t len, Header& h) const {
  return group_ != 0 && readHeader(raw, len, h) && h.group == group_ && h.sender != SERVER;
}

int GatewayStore::find(const Header& h) const {
  for (int i = 0; i < SLOTS; i++) {
    if (!slots_[i].seq) continue;
    const uint8_t* d = slots_[i].packet.data;
    if ((d[1] & 0x0F) == uint8_t(h.type) && d[4] == h.sender && get32(d + 6) == h.counter &&
        (d[10] >> 4) == h.pieceIndex) {
      return i;
    }
  }
  return -1;
}

int GatewayStore::newestOf(const Type type, const uint8_t sender) const {
  int newest = -1;
  for (int i = 0; i < SLOTS; i++) {
    const uint8_t* d = slots_[i].packet.data;
    if (slots_[i].seq && (d[1] & 0x0F) == uint8_t(type) && d[4] == sender &&
        (newest < 0 || get32(d + 6) > get32(slots_[newest].packet.data + 6))) {
      newest = i;
    }
  }
  return newest;
}

int GatewayStore::store(const Header& h, const uint8_t* raw, const size_t len) {
  if (find(h) >= 0) return -1;
  int index = -1;
  if (h.type == Type::Pos || h.type == Type::Sos) {
    // Only each member's newest position and SOS are kept, so an SOS sent
    // every minute does not push the messages out.
    index = newestOf(h.type, h.sender);
    if (index >= 0 && get32(slots_[index].packet.data + 6) > h.counter) return -1;
  }
  if (index < 0) {
    // Full: the oldest packet makes room.
    index = 0;
    for (int i = 0; i < SLOTS; i++) {
      if (slots_[i].seq < slots_[index].seq) index = i;
      if (!slots_[i].seq) break;
    }
  }
  Slot& s = slots_[index];
  s.seq = nextSeq_++;
  s.packet.len = uint8_t(len);
  memcpy(s.packet.data, raw, len);
  return index;
}

bool GatewayStore::hasAllPieces(const Header& h) const {
  int pieces = 0;
  for (const Slot& s : slots_) {
    const uint8_t* d = s.packet.data;
    if (s.seq && (d[1] & 0x0F) == uint8_t(Type::Msg) && d[4] == h.sender && get32(d + 6) == h.counter) pieces++;
  }
  return pieces == h.pieceCount;
}

void GatewayStore::addAck(const uint8_t to, const uint8_t ackedSender, const uint32_t ackedCounter, Result& r) {
  if (r.count >= MAX_ANSWER) return;
  Header h;
  h.type = Type::Ack;
  h.group = group_;
  h.sender = SERVER;
  h.recipient = to;
  uint32_t save = 0;
  h.counter = counters_.take(&save);
  if (save) r.saveCounter = save;
  RawPacket& p = r.packets[r.count++];
  writeHeader(h, p.data);
  writeAck(Ack{ackedSender, ackedCounter, AckState::Stored}, p.data + HEADER_SIZE);
  p.len = uint8_t(HEADER_SIZE + ACK_SIZE);
}

void GatewayStore::addSlot(const int index, Result& r) const {
  if (r.count >= MAX_ANSWER) return;
  RawPacket& p = r.packets[r.count++];
  p = slots_[index].packet;
  p.data[3] &= 0xF0;  // hops 0: the asking unit is in range, nobody relays an answer
}

void GatewayStore::answerMessages(const uint8_t member, const uint8_t sender, const uint32_t after,
                                  const uint64_t before, Result& r) const {
  int found[SLOTS];
  int n = 0;
  for (int i = 0; i < SLOTS; i++) {
    const uint8_t* d = slots_[i].packet.data;
    if (!slots_[i].seq || (d[1] & 0x0F) != uint8_t(Type::Msg) || d[4] != sender) continue;
    const uint32_t counter = get32(d + 6);
    if (counter > after && counter < before && canRead(member, sender, d[5])) found[n++] = i;
  }
  // Oldest first, pieces in order, as the server answers.
  std::sort(found, found + n, [this](const int a, const int b) {
    const uint8_t* x = slots_[a].packet.data;
    const uint8_t* y = slots_[b].packet.data;
    const uint32_t cx = get32(x + 6), cy = get32(y + 6);
    return cx != cy ? cx < cy : (x[10] >> 4) < (y[10] >> 4);
  });
  for (int i = 0; i < n; i++) addSlot(found[i], r);
}

void GatewayStore::answerSync(const uint8_t member, const SyncReq& req, Result& r) const {
  // Messages first, then the newest position (and SOS) of each other member,
  // then ACKs from the others for the asking unit's messages.
  for (uint8_t sender = 1; sender <= MAX_MEMBERS; sender++) {
    if (sender != member) answerMessages(member, sender, req.lastCounter[sender - 1], UINT64_MAX, r);
  }
  for (const Type type : {Type::Pos, Type::Sos}) {
    for (uint8_t sender = 1; sender <= MAX_MEMBERS; sender++) {
      if (sender == member) continue;
      const int newest = newestOf(type, sender);
      if (newest >= 0) addSlot(newest, r);
    }
  }
  const uint32_t own = req.lastCounter[member - 1];
  const uint32_t from = own > 50 ? own - 50 : 0;
  for (int i = 0; i < SLOTS; i++) {
    const uint8_t* d = slots_[i].packet.data;
    if (slots_[i].seq && (d[1] & 0x0F) == uint8_t(Type::Ack) && d[HEADER_SIZE] == member &&
        get32(d + HEADER_SIZE + 1) >= from) {
      addSlot(i, r);
    }
  }
}

void GatewayStore::handle(const uint8_t* raw, const size_t len, Result& r) {
  r.count = 0;
  r.savedSlot = -1;
  r.saveCounter = 0;
  Header h;
  if (!ours(raw, len, h)) return;
  const uint8_t* body = raw + HEADER_SIZE;
  const size_t bodyLen = len - HEADER_SIZE;
  switch (h.type) {
    case Type::Msg:
    case Type::Pos:
    case Type::Sos:
      r.savedSlot = store(h, raw, len);
      // Like the server: the sender's screen shows that its message or SOS is kept.
      if (h.type == Type::Sos || (h.type == Type::Msg && hasAllPieces(h))) addAck(h.sender, h.sender, h.counter, r);
      return;
    case Type::Ack: {
      Ack a;
      if (readAck(body, bodyLen, a)) r.savedSlot = store(h, raw, len);
      return;
    }
    case Type::SyncReq: {
      SyncReq req;
      if (readSyncReq(body, bodyLen, req)) answerSync(h.sender, req, r);
      return;
    }
    case Type::Fetch: {
      Fetch f;
      if (readFetch(body, bodyLen, f)) answerMessages(h.sender, f.sender, f.after, f.before, r);
      return;
    }
  }
}

bool GatewayStore::relay(const uint8_t* raw, const size_t len, RawPacket& out) {
  Header h;
  if (!ours(raw, len, h) || h.hops == 0 || h.type == Type::SyncReq || !seen_.remember(h)) return false;
  out.len = uint8_t(len);
  memcpy(out.data, raw, len);
  out.data[3] = uint8_t((out.data[3] & 0xF0) | (h.hops - 1));
  return true;
}

void GatewayStore::restore(const int index, const Slot& s) {
  if (index < 0 || index >= SLOTS) return;
  Header h;
  // A slot that does not read back (half written) stays empty.
  if (!s.seq || !readHeader(s.packet.data, s.packet.len, h)) return;
  slots_[index] = s;
  nextSeq_ = std::max(nextSeq_, s.seq + 1);
}

int GatewayStore::count() const {
  int n = 0;
  for (const Slot& s : slots_) n += s.seq ? 1 : 0;
  return n;
}

void GatewayStore::clear() {
  for (Slot& s : slots_) s = Slot();
}

}  // namespace mesh
