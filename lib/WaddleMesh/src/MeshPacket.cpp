#include "MeshPacket.h"

#include <cstring>

namespace mesh {

size_t writeHeader(const Header& h, uint8_t* out) {
  out[0] = MAGIC;
  out[1] = uint8_t((VERSION << 4) | (uint8_t(h.type) & 0x0F));
  out[2] = h.network;
  out[3] = uint8_t((h.hops & 0x0F) | (h.fragmented ? 0x10 : 0));
  out[4] = h.sender;
  out[5] = h.recipient;
  put32(out + 6, h.counter);
  out[10] = uint8_t(((h.pieceIndex & 0x0F) << 4) | (h.pieceCount & 0x0F));
  put16(out + 11, h.groupId);
  out[13] = h.key;
  return HEADER_SIZE;
}

bool readHeader(const uint8_t* in, size_t len, Header& h) {
  if (len < HEADER_SIZE || len > MAX_PACKET) return false;
  if (in[0] != MAGIC || (in[1] >> 4) != VERSION) return false;
  const uint8_t type = in[1] & 0x0F;
  if (type < uint8_t(Type::Msg) || type > uint8_t(Type::Roster)) return false;
  h.type = Type(type);
  h.network = in[2];
  h.hops = in[3] & 0x0F;
  h.fragmented = (in[3] & 0x10) != 0;
  h.sender = in[4];
  h.recipient = in[5];
  h.counter = get32(in + 6);
  h.pieceIndex = in[10] >> 4;
  h.pieceCount = in[10] & 0x0F;
  h.groupId = get16(in + 11);
  h.key = in[13];
  if (h.pieceCount == 0 || h.pieceIndex >= h.pieceCount) return false;
  if (!h.fragmented && h.pieceCount != 1) return false;
  if (h.sender != SERVER && !isMember(h.sender)) return false;
  if (h.recipient != EVERYONE && h.recipient != SERVER && h.recipient != TO_GROUP && !isMember(h.recipient)) return false;
  // A group id only on group packets, and made by a member.
  if ((h.recipient == TO_GROUP) != (h.groupId != 0)) return false;
  if (h.groupId && !isMember(groupCreator(h.groupId))) return false;
  return true;
}

void associatedData(const Header& h, uint8_t out[AAD_SIZE]) {
  uint8_t full[HEADER_SIZE];
  writeHeader(h, full);
  memcpy(out, full, 3);         // magic, version/type, group
  memcpy(out + 3, full + 4, 6);  // sender, recipient, counter
  memcpy(out + 9, full + 11, 3);  // group id, key number
}

void nonce(const Header& h, uint8_t out[12]) {
  memset(out, 0, 12);
  out[0] = h.sender;
  out[1] = h.recipient;
  put32(out + 2, h.counter);
  out[6] = uint8_t(h.type);
}

void writePosition(const Position& p, uint8_t out[POS_SIZE]) {
  put32(out, p.time);
  put32(out + 4, uint32_t(p.latE7));
  put32(out + 8, uint32_t(p.lonE7));
  put16(out + 12, uint16_t(p.altitude));
  out[14] = p.battery;
  out[15] = uint8_t((p.noFix ? 1 : 0) | (p.cancelled ? 2 : 0));
}

bool readPosition(const uint8_t* in, size_t len, Position& p) {
  if (len != POS_SIZE) return false;
  p.time = get32(in);
  p.latE7 = int32_t(get32(in + 4));
  p.lonE7 = int32_t(get32(in + 8));
  p.altitude = int16_t(get16(in + 12));
  p.battery = in[14];
  p.noFix = (in[15] & 1) != 0;
  p.cancelled = (in[15] & 2) != 0;
  return true;
}

void writeAck(const Ack& a, uint8_t out[ACK_SIZE]) {
  out[0] = a.sender;
  put32(out + 1, a.counter);
  out[5] = uint8_t(a.state);
}

bool readAck(const uint8_t* in, size_t len, Ack& a) {
  if (len != ACK_SIZE) return false;
  if (in[5] < uint8_t(AckState::Delivered) || in[5] > uint8_t(AckState::Read)) return false;
  a.sender = in[0];
  a.counter = get32(in + 1);
  a.state = AckState(in[5]);
  return true;
}

size_t writeSyncReq(const SyncReq& s, uint8_t out[SYNC_REQ_MAX]) {
  out[0] = s.battery;
  out[1] = s.build;
  out[2] = s.flags;
  put16(out + 3, s.rosterVersion);
  size_t n = SYNC_REQ_HEAD;
  for (uint8_t m = 1; m <= MAX_MEMBERS; m++) {
    if (!s.lastCounter[m - 1]) continue;
    out[n] = m;
    put32(out + n + 1, s.lastCounter[m - 1]);
    n += 5;
  }
  return n;
}

bool readSyncReq(const uint8_t* in, size_t len, SyncReq& s) {
  if (len < SYNC_REQ_HEAD || len > SYNC_REQ_MAX || (len - SYNC_REQ_HEAD) % 5) return false;
  s = SyncReq{};
  s.battery = in[0];
  s.build = in[1];
  s.flags = in[2];
  s.rosterVersion = get16(in + 3);
  for (size_t i = SYNC_REQ_HEAD; i < len; i += 5) {
    if (!isMember(in[i])) return false;
    s.lastCounter[in[i] - 1] = get32(in + i + 1);
  }
  return true;
}

void writeFetch(const Fetch& f, uint8_t out[FETCH_SIZE]) {
  out[0] = f.sender;
  put32(out + 1, f.after);
  put32(out + 5, f.before);
}

bool readFetch(const uint8_t* in, size_t len, Fetch& f) {
  if (len != FETCH_SIZE || !isMember(in[0])) return false;
  f.sender = in[0];
  f.after = get32(in + 1);
  f.before = get32(in + 5);
  return f.before > f.after;
}

}  // namespace mesh
