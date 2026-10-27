#pragma once

// WaddleOS mesh packets: the binary format in protocol/PROTOCOL.md.
// Plain C++ with no Arduino dependency, so the host tests build it too.

#include <cstddef>
#include <cstdint>

namespace mesh {

constexpr uint8_t MAGIC = 0x57;  // "W"
constexpr uint8_t VERSION = 1;
constexpr size_t MAX_PACKET = 200;
constexpr size_t HEADER_SIZE = 11;
constexpr size_t TAG_SIZE = 16;
constexpr size_t MAX_PIECE = MAX_PACKET - HEADER_SIZE;  // ciphertext bytes per piece
constexpr uint8_t MAX_PIECES = 15;
constexpr uint8_t DEFAULT_HOPS = 3;

constexpr uint8_t SERVER = 0;
constexpr uint8_t GROUP = 0xFF;
constexpr uint8_t MAX_MEMBERS = 3;

constexpr size_t MSG_PREFIX = 8;  // time + previous hash, before the text
constexpr size_t MAX_TEXT = 1000;
constexpr size_t POS_SIZE = 16;
constexpr size_t ACK_SIZE = 6;
constexpr size_t SYNC_REQ_SIZE = 14;

enum class Type : uint8_t { Msg = 1, Ack = 2, SyncReq = 3, Pos = 4, Sos = 5 };

enum class AckState : uint8_t { Delivered = 1, Stored = 2, Read = 3 };

inline bool isEncrypted(Type t) { return t == Type::Msg || t == Type::Pos || t == Type::Sos; }
inline bool isMember(uint8_t id) { return id >= 1 && id <= MAX_MEMBERS; }

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

  // The message id, unique per sender for ever.
  uint64_t messageId() const { return (uint64_t(sender) << 32) | counter; }
};

// Writes the 11 header bytes. Returns HEADER_SIZE.
size_t writeHeader(const Header& h, uint8_t* out);
// Reads and checks the header (magic, version, known type, sane pieces).
bool readHeader(const uint8_t* in, size_t len, Header& h);

// The bytes that are authenticated but not encrypted: header bytes 0-2 and 4-9.
constexpr size_t AAD_SIZE = 9;
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

struct SyncReq {
  uint32_t lastCounter[MAX_MEMBERS] = {0, 0, 0};  // index 0 = member 1
  uint8_t battery = 0;
  uint8_t build = 0;
};
void writeSyncReq(const SyncReq& s, uint8_t out[SYNC_REQ_SIZE]);
bool readSyncReq(const uint8_t* in, size_t len, SyncReq& s);

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
