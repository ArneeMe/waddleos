#pragma once

// WaddleOS mesh packets: the binary format in protocol/PROTOCOL.md.
// Plain C++ with no Arduino dependency, so the host tests build it too.

#include <cstddef>
#include <cstdint>

namespace mesh {

constexpr uint8_t MAGIC = 0x57;  // "W"
constexpr uint8_t VERSION = 1;
constexpr size_t MAX_PACKET = 200;
constexpr size_t HEADER_SIZE = 12;
constexpr size_t TAG_SIZE = 16;
constexpr size_t MAX_PIECE = MAX_PACKET - HEADER_SIZE;  // ciphertext bytes per piece
constexpr uint8_t MAX_PIECES = 15;
constexpr uint8_t DEFAULT_HOPS = 3;

constexpr uint8_t SERVER = 0;
constexpr uint8_t GROUP = 0xFF;
constexpr uint8_t MAX_MEMBERS = 4;
// A group made on a unit: the recipient byte is GROUP_FLAG | its id. The id is
// the creating member (bits 4-6) and that member's own count (bits 0-3), so
// two units never pick the same one. Who is in it only the members know.
constexpr uint8_t GROUP_FLAG = 0x80;

constexpr size_t MSG_PREFIX = 8;  // time + previous hash, before the text
constexpr size_t MAX_TEXT = 1000;
constexpr size_t POS_SIZE = 16;
constexpr size_t ACK_SIZE = 6;
constexpr size_t SYNC_REQ_SIZE = 4 * MAX_MEMBERS + 3;
constexpr size_t FETCH_SIZE = 9;

enum class Type : uint8_t { Msg = 1, Ack = 2, SyncReq = 3, Pos = 4, Sos = 5, Fetch = 6 };

enum class AckState : uint8_t { Delivered = 1, Stored = 2, Read = 3 };

inline bool isEncrypted(Type t) { return t == Type::Msg || t == Type::Pos || t == Type::Sos; }
inline bool isMember(uint8_t id) { return id >= 1 && id <= MAX_MEMBERS; }
inline uint8_t memberBit(uint8_t id) { return isMember(id) ? uint8_t(1u << (id - 1)) : 0; }
inline int memberCount(uint8_t mask) {
  int n = 0;
  for (; mask; mask &= mask - 1) n++;
  return n;
}
inline uint8_t groupId(uint8_t creator, uint8_t index) { return uint8_t(((creator & 7) << 4) | (index & 15)); }
inline uint8_t groupCreator(uint8_t id) { return (id >> 4) & 7; }
// GROUP_FLAG | the id of a group made by a member.
inline bool isGroup(uint8_t recipient) {
  return recipient != 0xFF && (recipient & GROUP_FLAG) && isMember(groupCreator(recipient));
}
// Whether member may be given a packet to recipient that sender sent. Groups
// go to every member: only those with the key can open them.
inline bool canRead(uint8_t member, uint8_t sender, uint8_t recipient) {
  return recipient == 0xFF || recipient == member || sender == member || isGroup(recipient);
}

struct Header {
  Type type = Type::Msg;
  uint8_t group = 0;
  uint8_t hops = DEFAULT_HOPS;  // hops left
  bool fragmented = false;
  uint8_t sender = 0;
  uint8_t recipient = GROUP;
  uint32_t counter = 0;
  uint8_t pieceIndex = 0;
  uint8_t pieceCount = 1;
  uint8_t key = 0;  // a group's key number (1, 2, ...), 0 for everything else

  // The message id, unique per sender for ever.
  uint64_t messageId() const { return (uint64_t(sender) << 32) | counter; }
};

// Writes the 11 header bytes. Returns HEADER_SIZE.
size_t writeHeader(const Header& h, uint8_t* out);
// Reads and checks the header (magic, version, known type, sane pieces).
bool readHeader(const uint8_t* in, size_t len, Header& h);

// The bytes that are authenticated but not encrypted: header bytes 0-2, 4-9 and 11.
constexpr size_t AAD_SIZE = 10;
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
  uint32_t lastCounter[MAX_MEMBERS] = {};  // index 0 = member 1
  uint8_t battery = 0;
  uint8_t build = 0;
  uint8_t flags = 0;
};
void writeSyncReq(const SyncReq& s, uint8_t out[SYNC_REQ_SIZE]);
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
