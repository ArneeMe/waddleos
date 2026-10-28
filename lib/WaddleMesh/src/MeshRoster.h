#pragma once

// The roster: who is in the network, signed by the admin (protocol/PROTOCOL.md,
// "The roster"), and the keys for joining ("Joining"). Plain C++ like the rest
// of the library, so the host tests build it too.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "MeshCrypto.h"

namespace mesh {

constexpr size_t SIGN_KEY_SIZE = 32;
constexpr size_t SIGNATURE_SIZE = 64;
constexpr size_t TOKEN_HASH_SIZE = 16;
constexpr size_t ROSTER_NAME = 20;
constexpr size_t PASSWORD_SIZE = 16;

struct RosterEntry {
  uint8_t number = 0;
  bool admin = false;
  std::string name;
  uint8_t publicKey[KEY_SIZE] = {};
  uint8_t tokenHash[TOKEN_HASH_SIZE] = {};
};

struct Roster {
  uint8_t network = 0;
  uint16_t version = 0;
  std::vector<RosterEntry> members;

  const RosterEntry* member(uint8_t number) const;
  Members everyone() const;
};

// Reads a signed roster. False when it is malformed or not signed by adminSignKey.
bool readRoster(const uint8_t* data, size_t len, const uint8_t adminSignKey[SIGN_KEY_SIZE], Roster& out);
// Writes and signs one (for tests and tools; the admin's secret is the 32-byte Ed25519 seed).
std::vector<uint8_t> writeRoster(const Roster& r, const uint8_t adminSeed[32]);
// The admin's Ed25519 public key from its seed.
void signPublicKey(const uint8_t seed[32], uint8_t publicOut[SIGN_KEY_SIZE]);

// The join key K: BLAKE2b-256(X25519(unit secret, admin public) || password ||
// unit public || admin public). The admin's side gets the same from its secret.
void joinKey(const uint8_t unitSecret[KEY_SIZE], const uint8_t unitPublic[KEY_SIZE],
             const uint8_t adminPublic[KEY_SIZE], const uint8_t password[PASSWORD_SIZE], uint8_t keyOut[KEY_SIZE]);
// The six digits both sides can compare: from both public keys, "482 913".
void joinCode(const uint8_t adminPublic[KEY_SIZE], const uint8_t unitPublic[KEY_SIZE], char out[8]);

}  // namespace mesh
