#pragma once

// WaddleOS mesh packets: the binary format in protocol/PROTOCOL.md.
// Plain C++ with no Arduino dependency, so the host tests build it too.

#include <cstddef>
#include <cstdint>

namespace mesh {

constexpr uint8_t MAGIC = 0x57;  // "W"
constexpr uint8_t VERSION = 3;  // 3: up to 32 members, a signed roster, 2-byte group ids
constexpr size_t MAX_PACKET = 200;
constexpr size_t HEADER_SIZE = 14;
constexpr size_t TAG_SIZE = 16;
constexpr size_t MAX_PIECE = MAX_PACKET - HEADER_SIZE;  // ciphertext bytes per piece
constexpr uint8_t MAX_PIECES = 15;
constexpr uint8_t DEFAULT_HOPS = 3;

constexpr uint8_t SERVER = 0;
constexpr uint8_t EVERYONE = 0xFF;  // everyone
// A group made on a unit: recipient TO_GROUP, its id in the header. The id is
// the creating member × 256 + that member's own count, so two units never pick
// the same one. Who is in it only the members know.
constexpr uint8_t TO_GROUP = 0xFE;
// ROSTER packets: counter = ROSTER_COUNTER + the roster's version (sender 0).
constexpr uint32_t ROSTER_COUNTER = 0x40000000;
constexpr uint8_t MAX_MEMBERS = 32;

constexpr size_t MSG_PREFIX = 8;  // time + previous hash, before the text
constexpr size_t MAX_TEXT = 1000;
constexpr size_t POS_SIZE = 16;
constexpr size_t ACK_SIZE = 6;
constexpr size_t SYNC_REQ_HEAD = 5;  // then 5 bytes per member listed
constexpr size_t SYNC_REQ_MAX = SYNC_REQ_HEAD + 5 * MAX_MEMBERS;
constexpr size_t FETCH_SIZE = 9;

enum class Type : uint8_t { Msg = 1, Ack = 2, SyncReq = 3, Pos = 4, Sos = 5, Fetch = 6, Roster = 7 };

enum class AckState : uint8_t { Delivered = 1, Stored = 2, Read = 3 };

inline bool isMember(uint8_t id) { return id >= 1 && id <= MAX_MEMBERS; }
// Sets of members: a bit per member (bit 0 = member 1).
using Members = uint32_t;
inline Members memberBit(uint8_t id) { return isMember(id) ? Members(1u << (id - 1)) : 0; }
inline int memberCount(Members mask) {
  int n = 0;
  for (; mask; mask &= mask - 1) n++;
  return n;
}
inline uint16_t groupId(uint8_t creator, uint8_t index) { return uint16_t((creator << 8) | index); }
inline uint8_t groupCreator(uint16_t id) { return uint8_t(id >> 8); }
// Whether member may be given a packet to recipient that sender sent. Groups
// go to every member: only those with the key can open them.
inline bool canRead(uint8_t member, uint8_t sender, uint8_t recipient) {
  return recipient == EVERYONE || recipient == member || sender == member || recipient == TO_GROUP;
}

struct Header {
  Type type = Type::Msg;
  uint8_t network = 0;
  uint8_t hops = DEFAULT_HOPS;  // hops left
  bool fragmented = false;
  uint8_t sender = 0;
  uint8_t recipient = EVERYONE;
  uint32_t counter = 0;
  uint8_t pieceIndex = 0;
  uint8_t pieceCount = 1;
  uint16_t groupId = 0;  // recipient TO_GROUP: which group
  uint8_t key = 0;       // which of the sender's or the group's keys sealed it

  // The message id, unique per sender for ever.
  uint64_t messageId() const { return (uint64_t(sender) << 32) | counter; }
};

// Writes the HEADER_SIZE (14) header bytes. Returns HEADER_SIZE.
size_t writeHeader(const Header& h, uint8_t* out);
// Reads and checks the header (magic, version, known type, sane pieces).
bool readHeader(const uint8_t* in, size_t len, Header& h);

// The bytes that are authenticated but not encrypted: header bytes 0-2, 4-9 and 11-13.
constexpr size_t AAD_SIZE = 12;
void associatedData(const Header& h, uint8_t out[AAD_SIZE]);
// The 12-byte ChaCha20-Poly1305 nonce: sender, recipient, counter, type, zeros.
void nonce(const Header& h, uint8_t out[12]);

// --- Bodies ---------------------------------------------------------------

struct Position {
  uint32_t time = 0;
  int32_t latE7 = 0;
  int32_t lonE7 = 0;
  int16_t altitude = 0;
  uint8_t battery = 0;
  bool noFix = false;
  bool cancelled = false;  // SOS only: the alarm is over
};
void writePosition(const Position& p, uint8_t out[POS_SIZE]);
bool readPosition(const uint8_t* in, size_t len, Position& p);

struct Ack {
  uint8_t sender = 0;
  uint32_t counter = 0;
  AckState state = AckState::Delivered;
};
void writeAck(const Ack& a, uint8_t out[ACK_SIZE]);
bool readAck(const uint8_t* in, size_t len, Ack& a);

// How the unit is set up, sent with each sync request for the server's status page.
constexpr uint8_t SYNC_ON_TRIP = 1;      // bit 0: "På tur"
constexpr uint8_t SYNC_SOS = 2;          // bit 1: its SOS is going on
constexpr uint8_t SYNC_CHECK_SHIFT = 2;  // bits 2-4: how often it checks (MeshSettings::Check)

struct SyncReq {
  uint32_t lastCounter[MAX_MEMBERS] = {};  // index 0 = member 1; only those not 0 are sent
  uint8_t battery = 0;
  uint8_t build = 0;
  uint8_t flags = 0;
  uint16_t rosterVersion = 0;
};
// Returns the bytes written (at most SYNC_REQ_MAX).
size_t writeSyncReq(const SyncReq& s, uint8_t out[SYNC_REQ_MAX]);
bool readSyncReq(const uint8_t* in, size_t len, SyncReq& s);

// "Send me sender's messages between these two counters": a message the
// chain says is missing.
struct Fetch {
  uint8_t sender = 0;
  uint32_t after = 0;
  uint32_t before = 0;
};
void writeFetch(const Fetch& f, uint8_t out[FETCH_SIZE]);
bool readFetch(const uint8_t* in, size_t len, Fetch& f);

// --- Little-endian helpers --------------------------------------------------

inline void put16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}
inline void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF;
}
inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

}  // namespace mesh
