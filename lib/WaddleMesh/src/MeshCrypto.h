#pragma once

// Encryption for MSG, POS, SOS and ROSTER packets, and the sealed boxes of
// setup and joining (protocol/PROTOCOL.md, "Encryption", "Joining").

#include <cstddef>
#include <cstdint>

#include "MeshPacket.h"

namespace mesh {

constexpr size_t KEY_SIZE = 32;

// A unit's own X25519 key pair for DMs. The secret never leaves the unit.
void publicKey(const uint8_t secret[KEY_SIZE], uint8_t publicOut[KEY_SIZE]);
// The DM key two members share: BLAKE2b-256(X25519 || lower public || higher public).
void dmKey(const uint8_t mySecret[KEY_SIZE], const uint8_t myPublic[KEY_SIZE], const uint8_t theirPublic[KEY_SIZE],
             uint8_t keyOut[KEY_SIZE]);
// First 4 bytes of BLAKE2b-256: the "previous hash" in MSG.
void shortHash(const uint8_t* data, size_t len, uint8_t out[4]);

// Setup over the home Wi-Fi (`waddle mesh setup`): the tool sends a one-time
// public key, and both sides derive BLAKE2b-256(X25519 || tool public || unit
// public). What travels is sealed with ChaCha20-Poly1305 under that key, with
// nonce = the given byte and 11 zeros (1: the unit's sender key, 2: the group).
void boxKey(const uint8_t unitSecret[KEY_SIZE], const uint8_t unitPublic[KEY_SIZE],
              const uint8_t toolPublic[KEY_SIZE], uint8_t keyOut[KEY_SIZE]);
// out gets len + TAG_SIZE bytes.
void sealBox(const uint8_t key[KEY_SIZE], uint8_t nonceByte, const uint8_t* plain, size_t len, uint8_t* out);
// plainOut gets len - TAG_SIZE bytes. False when it was changed or the key is wrong.
bool openBox(const uint8_t key[KEY_SIZE], uint8_t nonceByte, const uint8_t* sealed, size_t len, uint8_t* plainOut);
// The six digits a unit and the tool both show during setup ("482 913"), so
// the person can tell nothing on the Wi-Fi sits between them. The tool first
// sends only commit = BLAKE2b-256(tool public || tool nonce), the unit then
// picks its nonce, and only then does the tool reveal its key and nonce: neither
// a fake tool nor a fake unit can choose its key to make the codes match.
constexpr size_t PAIR_NONCE_SIZE = 16;
void pairCommit(const uint8_t toolPublic[KEY_SIZE], const uint8_t toolNonce[PAIR_NONCE_SIZE], uint8_t out[32]);
void pairCode(const uint8_t toolPublic[KEY_SIZE], const uint8_t unitPublic[KEY_SIZE],
              const uint8_t toolNonce[PAIR_NONCE_SIZE], const uint8_t unitNonce[PAIR_NONCE_SIZE], char out[8]);

// The packets one message becomes: pieces back to back, MAX_PACKET apart.
struct Sealed {
  uint8_t packets[MAX_PIECES][MAX_PACKET];
  size_t lengths[MAX_PIECES] = {};
  uint8_t count = 0;
};

// Encrypts plain under key with base's header (type, group, sender, recipient,
// counter, hops) and cuts the result into pieces. False when it is too long.
bool seal(const Header& base, const uint8_t key[KEY_SIZE], const uint8_t* plain, size_t plainLen, Sealed& out);

// Decrypts a whole message (all pieces joined, tag last). plainOut needs
// cipherLen - TAG_SIZE bytes. False when the tag does not match: wrong key,
// forged or changed.
bool open(const Header& h, const uint8_t key[KEY_SIZE], const uint8_t* cipher, size_t cipherLen, uint8_t* plainOut);

// Collects the pieces of split messages until all have arrived.
class Reassembler {
 public:
  static constexpr int SLOTS = 4;
  static constexpr size_t MAX_MESSAGE = MAX_PIECES * MAX_PIECE;

  ~Reassembler();

  // Adds one received packet. When it completes a message, returns true and
  // points data/len at the joined ciphertext (valid until the next add()).
  // nowMs drops half-finished messages older than a minute.
  bool add(const Header& h, const uint8_t* packet, size_t len, uint32_t nowMs, const uint8_t** data, size_t* outLen);

 private:
  struct Slot {
    uint8_t* buffer = nullptr;
    uint64_t id = 0;
    Type type = Type::Msg;
    uint16_t received = 0;  // bit per piece
    uint8_t count = 0;
    size_t lastLen = 0;
    uint32_t startedMs = 0;
  };
  Slot slots_[SLOTS];
  uint8_t* done_ = nullptr;  // the last completed message, freed on the next add()

  Slot* find(const Header& h, uint32_t nowMs);
};

}  // namespace mesh
