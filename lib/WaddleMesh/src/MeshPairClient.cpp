#include "MeshPairClient.h"

#include <monocypher.h>

#include <cstring>
#include <vector>

namespace mesh {

namespace {
std::string hex(const uint8_t* data, const size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < len; i++) {
    s += digits[data[i] >> 4];
    s += digits[data[i] & 15];
  }
  return s;
}

bool unhex(const std::string& text, uint8_t* out, const size_t len) {
  if (text.size() != 2 * len) return false;
  for (size_t i = 0; i < len; i++) {
    int v = 0;
    for (int k = 0; k < 2; k++) {
      const char c = text[2 * i + k];
      const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                                           : -1;
      if (d < 0) return false;
      v = v * 16 + d;
    }
    out[i] = uint8_t(v);
  }
  return true;
}
}  // namespace

PairClient::PairClient(const uint8_t random[KEY_SIZE + PAIR_NONCE_SIZE]) {
  memcpy(secret_, random, KEY_SIZE);
  memcpy(nonce_, random + KEY_SIZE, PAIR_NONCE_SIZE);
  publicKey(secret_, public_);
}

PairClient::~PairClient() {
  crypto_wipe(secret_, sizeof(secret_));
  crypto_wipe(key_, sizeof(key_));
}

std::string PairClient::commitHex() const {
  uint8_t commit[32];
  pairCommit(public_, nonce_, commit);
  return hex(commit, sizeof(commit));
}

bool PairClient::unitAnswer(const std::string& publicHex, const std::string& nonceHex, char code[8]) {
  uint8_t unit[KEY_SIZE], unitNonce[PAIR_NONCE_SIZE];
  if (!unhex(publicHex, unit, KEY_SIZE) || !unhex(nonceHex, unitNonce, PAIR_NONCE_SIZE)) return false;
  pairCode(public_, unit, nonce_, unitNonce, code);
  // The same key as the unit's boxKey(unit secret, unit public, my public).
  uint8_t input[3 * KEY_SIZE];
  crypto_x25519(input, secret_, unit);
  memcpy(input + KEY_SIZE, public_, KEY_SIZE);
  memcpy(input + 2 * KEY_SIZE, unit, KEY_SIZE);
  crypto_blake2b(key_, KEY_SIZE, input, sizeof(input));
  crypto_wipe(input, sizeof(input));
  answered_ = true;
  return true;
}

std::string PairClient::toolHex() const { return hex(public_, KEY_SIZE); }
std::string PairClient::nonceHex() const { return hex(nonce_, PAIR_NONCE_SIZE); }

std::string PairClient::sealRequest(const std::string& plain) const {
  if (!answered_) return "";
  std::vector<uint8_t> box(plain.size() + TAG_SIZE);
  sealBox(key_, 2, reinterpret_cast<const uint8_t*>(plain.data()), plain.size(), box.data());
  return "{\"tool\":\"" + toolHex() + "\",\"box\":\"" + hex(box.data(), box.size()) + "\"}";
}

bool PairClient::open(const std::string& boxHex, const uint8_t nonceByte, std::string& plain) const {
  const size_t len = boxHex.size() / 2;
  if (!answered_ || len <= TAG_SIZE) return false;
  std::vector<uint8_t> box(len);
  if (!unhex(boxHex, box.data(), len)) return false;
  plain.assign(len - TAG_SIZE, '\0');
  return openBox(key_, nonceByte, box.data(), len, reinterpret_cast<uint8_t*>(&plain[0]));
}

}  // namespace mesh
