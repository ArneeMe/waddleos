#pragma once

// The other side of the setup code (MeshCrypto.h, pairCode), for the member's
// phone (or any client that is not the unit): commit, read the unit's answer,
// reveal, compare the six digits, then seal requests for the unit and open what
// it seals back.

#include <cstdint>
#include <string>

#include "MeshCrypto.h"

namespace mesh {

class PairClient {
 public:
  // random: KEY_SIZE + PAIR_NONCE_SIZE fresh random bytes (the one-time key and nonce).
  explicit PairClient(const uint8_t random[KEY_SIZE + PAIR_NONCE_SIZE]);
  ~PairClient();
  // For GET /api/mesh/identity?commit=...
  std::string commitHex() const;
  // The unit's public key and nonce from that answer. Then: the six digits to
  // compare with the unit's screen, and toolHex()/nonceHex() for POST /api/mesh/pair.
  bool unitAnswer(const std::string& publicHex, const std::string& nonceHex, char code[8]);
  std::string toolHex() const;
  std::string nonceHex() const;
  // A body for the unit: {"tool": ..., "box": ...} sealed with nonce byte 2.
  std::string sealRequest(const std::string& plain) const;
  // A box from the unit, sealed with nonceByte; false when it does not open.
  bool open(const std::string& boxHex, uint8_t nonceByte, std::string& plain) const;

 private:
  uint8_t secret_[KEY_SIZE];
  uint8_t public_[KEY_SIZE];
  uint8_t nonce_[PAIR_NONCE_SIZE];
  uint8_t key_[KEY_SIZE] = {};
  bool answered_ = false;
};

}  // namespace mesh
