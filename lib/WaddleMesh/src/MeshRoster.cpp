#include "MeshRoster.h"

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include <cstdio>
#include <cstring>

namespace mesh {

namespace {
constexpr uint8_t FORMAT = 1;
constexpr size_t HEAD = 5;  // format, network, version (2), count
}  // namespace

const RosterEntry* Roster::member(const uint8_t number) const {
  for (const auto& m : members) {
    if (m.number == number) return &m;
  }
  return nullptr;
}

Members Roster::everyone() const {
  Members bits = 0;
  for (const auto& m : members) bits |= memberBit(m.number);
  return bits;
}

bool readRoster(const uint8_t* data, const size_t len, const uint8_t adminSignKey[SIGN_KEY_SIZE], Roster& out) {
  if (len < HEAD + SIGNATURE_SIZE || data[0] != FORMAT) return false;
  const size_t signedLen = len - SIGNATURE_SIZE;
  if (crypto_ed25519_check(data + signedLen, adminSignKey, data, signedLen) != 0) return false;
  Roster r;
  r.network = data[1];
  r.version = get16(data + 2);
  const uint8_t count = data[4];
  size_t i = HEAD;
  Members seen = 0;
  for (uint8_t k = 0; k < count; k++) {
    if (i + 3 > signedLen) return false;
    RosterEntry e;
    e.number = data[i];
    e.admin = (data[i + 1] & 1) != 0;
    const uint8_t nameLen = data[i + 2];
    i += 3;
    if (!isMember(e.number) || (seen & memberBit(e.number)) || nameLen > ROSTER_NAME ||
        i + nameLen + KEY_SIZE + TOKEN_HASH_SIZE > signedLen) {
      return false;
    }
    seen |= memberBit(e.number);
    e.name.assign(reinterpret_cast<const char*>(data + i), nameLen);
    i += nameLen;
    memcpy(e.publicKey, data + i, KEY_SIZE);
    i += KEY_SIZE;
    memcpy(e.tokenHash, data + i, TOKEN_HASH_SIZE);
    i += TOKEN_HASH_SIZE;
    r.members.push_back(std::move(e));
  }
  if (i != signedLen) return false;
  out = std::move(r);
  return true;
}

void signPublicKey(const uint8_t seed[32], uint8_t publicOut[SIGN_KEY_SIZE]) {
  uint8_t secret[64], s[32];
  memcpy(s, seed, 32);
  crypto_ed25519_key_pair(secret, publicOut, s);
  crypto_wipe(secret, sizeof(secret));
}

std::vector<uint8_t> writeRoster(const Roster& r, const uint8_t adminSeed[32]) {
  std::vector<uint8_t> out = {FORMAT, r.network, uint8_t(r.version), uint8_t(r.version >> 8), uint8_t(r.members.size())};
  for (const auto& m : r.members) {
    const size_t nameLen = m.name.size() < ROSTER_NAME ? m.name.size() : ROSTER_NAME;
    out.push_back(m.number);
    out.push_back(m.admin ? 1 : 0);
    out.push_back(uint8_t(nameLen));
    out.insert(out.end(), m.name.begin(), m.name.begin() + long(nameLen));
    out.insert(out.end(), m.publicKey, m.publicKey + KEY_SIZE);
    out.insert(out.end(), m.tokenHash, m.tokenHash + TOKEN_HASH_SIZE);
  }
  uint8_t secret[64], pub[32], s[32];
  memcpy(s, adminSeed, 32);
  crypto_ed25519_key_pair(secret, pub, s);
  uint8_t signature[SIGNATURE_SIZE];
  crypto_ed25519_sign(signature, secret, out.data(), out.size());
  crypto_wipe(secret, sizeof(secret));
  out.insert(out.end(), signature, signature + SIGNATURE_SIZE);
  return out;
}

void joinKey(const uint8_t unitSecret[KEY_SIZE], const uint8_t unitPublic[KEY_SIZE],
             const uint8_t adminPublic[KEY_SIZE], const uint8_t password[PASSWORD_SIZE], uint8_t keyOut[KEY_SIZE]) {
  uint8_t input[KEY_SIZE + PASSWORD_SIZE + 2 * KEY_SIZE];
  crypto_x25519(input, unitSecret, adminPublic);
  memcpy(input + KEY_SIZE, password, PASSWORD_SIZE);
  memcpy(input + KEY_SIZE + PASSWORD_SIZE, unitPublic, KEY_SIZE);
  memcpy(input + 2 * KEY_SIZE + PASSWORD_SIZE, adminPublic, KEY_SIZE);
  crypto_blake2b(keyOut, KEY_SIZE, input, sizeof(input));
  crypto_wipe(input, sizeof(input));
}

void joinCode(const uint8_t adminPublic[KEY_SIZE], const uint8_t unitPublic[KEY_SIZE], char out[8]) {
  uint8_t both[2 * KEY_SIZE], h[32];
  memcpy(both, adminPublic, KEY_SIZE);
  memcpy(both + KEY_SIZE, unitPublic, KEY_SIZE);
  crypto_blake2b(h, sizeof(h), both, sizeof(both));
  const uint32_t n = ((uint32_t(h[0]) << 16) | (uint32_t(h[1]) << 8) | h[2]) % 1000000;
  snprintf(out, 8, "%03u %03u", unsigned(n / 1000), unsigned(n % 1000));
}

}  // namespace mesh
