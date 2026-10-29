#include "MeshCrypto.h"

#include <monocypher.h>

#include <cstdio>
#include <cstring>

namespace mesh {

void publicKey(const uint8_t secret[KEY_SIZE], uint8_t publicOut[KEY_SIZE]) {
  crypto_x25519_public_key(publicOut, secret);
}

void pairKey(const uint8_t mySecret[KEY_SIZE], const uint8_t myPublic[KEY_SIZE], const uint8_t theirPublic[KEY_SIZE],
             uint8_t keyOut[KEY_SIZE]) {
  uint8_t input[3 * KEY_SIZE];
  crypto_x25519(input, mySecret, theirPublic);
  const bool mineFirst = memcmp(myPublic, theirPublic, KEY_SIZE) < 0;
  memcpy(input + KEY_SIZE, mineFirst ? myPublic : theirPublic, KEY_SIZE);
  memcpy(input + 2 * KEY_SIZE, mineFirst ? theirPublic : myPublic, KEY_SIZE);
  crypto_blake2b(keyOut, KEY_SIZE, input, sizeof(input));
  crypto_wipe(input, sizeof(input));
}

void shortHash(const uint8_t* data, const size_t len, uint8_t out[4]) {
  uint8_t full[32];
  crypto_blake2b(full, sizeof(full), data, len);
  memcpy(out, full, 4);
}

void setupKey(const uint8_t unitSecret[KEY_SIZE], const uint8_t unitPublic[KEY_SIZE],
              const uint8_t toolPublic[KEY_SIZE], uint8_t keyOut[KEY_SIZE]) {
  uint8_t input[3 * KEY_SIZE];
  crypto_x25519(input, unitSecret, toolPublic);
  memcpy(input + KEY_SIZE, toolPublic, KEY_SIZE);
  memcpy(input + 2 * KEY_SIZE, unitPublic, KEY_SIZE);
  crypto_blake2b(keyOut, KEY_SIZE, input, sizeof(input));
  crypto_wipe(input, sizeof(input));
}

namespace {
void setupNonce(const uint8_t nonceByte, uint8_t iv[12]) {
  memset(iv, 0, 12);
  iv[0] = nonceByte;
}
}  // namespace

void setupSeal(const uint8_t key[KEY_SIZE], const uint8_t nonceByte, const uint8_t* plain, const size_t len,
               uint8_t* out) {
  uint8_t iv[12];
  setupNonce(nonceByte, iv);
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key, iv);
  crypto_aead_write(&ctx, out, out + len, nullptr, 0, plain, len);
  crypto_wipe(&ctx, sizeof(ctx));
}

bool setupOpen(const uint8_t key[KEY_SIZE], const uint8_t nonceByte, const uint8_t* sealed, const size_t len,
               uint8_t* plainOut) {
  if (len < TAG_SIZE) return false;
  uint8_t iv[12];
  setupNonce(nonceByte, iv);
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key, iv);
  const int bad = crypto_aead_read(&ctx, plainOut, sealed + len - TAG_SIZE, nullptr, 0, sealed, len - TAG_SIZE);
  crypto_wipe(&ctx, sizeof(ctx));
  return bad == 0;
}

void pairCommit(const uint8_t toolPublic[KEY_SIZE], const uint8_t toolNonce[PAIR_NONCE_SIZE], uint8_t out[32]) {
  uint8_t input[KEY_SIZE + PAIR_NONCE_SIZE];
  memcpy(input, toolPublic, KEY_SIZE);
  memcpy(input + KEY_SIZE, toolNonce, PAIR_NONCE_SIZE);
  crypto_blake2b(out, 32, input, sizeof(input));
}

void pairCode(const uint8_t toolPublic[KEY_SIZE], const uint8_t unitPublic[KEY_SIZE],
              const uint8_t toolNonce[PAIR_NONCE_SIZE], const uint8_t unitNonce[PAIR_NONCE_SIZE], char out[8]) {
  uint8_t input[2 * KEY_SIZE + 2 * PAIR_NONCE_SIZE], h[32];
  memcpy(input, toolPublic, KEY_SIZE);
  memcpy(input + KEY_SIZE, unitPublic, KEY_SIZE);
  memcpy(input + 2 * KEY_SIZE, toolNonce, PAIR_NONCE_SIZE);
  memcpy(input + 2 * KEY_SIZE + PAIR_NONCE_SIZE, unitNonce, PAIR_NONCE_SIZE);
  crypto_blake2b(h, sizeof(h), input, sizeof(input));
  const uint32_t n = ((uint32_t(h[0]) << 16) | (uint32_t(h[1]) << 8) | h[2]) % 1000000;
  snprintf(out, 8, "%03u %03u", unsigned(n / 1000), unsigned(n % 1000));
}

bool seal(const Header& base, const uint8_t key[KEY_SIZE], const uint8_t* plain, const size_t plainLen, Sealed& out) {
  const size_t total = plainLen + TAG_SIZE;
  const size_t count = (total + MAX_PIECE - 1) / MAX_PIECE;
  if (count > MAX_PIECES) return false;

  Header h = base;
  h.fragmented = count > 1;
  h.pieceCount = uint8_t(count);
  uint8_t aad[AAD_SIZE];
  uint8_t iv[12];
  associatedData(h, aad);
  nonce(h, iv);

  // Encrypt into one run, then copy the pieces out behind their headers.
  static_assert(MAX_PIECES * MAX_PIECE <= sizeof(Sealed::packets), "pieces fit");
  uint8_t* run = new uint8_t[total];
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key, iv);
  crypto_aead_write(&ctx, run, run + plainLen, aad, sizeof(aad), plain, plainLen);
  crypto_wipe(&ctx, sizeof(ctx));

  out.count = uint8_t(count);
  for (size_t i = 0; i < count; i++) {
    h.pieceIndex = uint8_t(i);
    const size_t offset = i * MAX_PIECE;
    const size_t len = (total - offset) < MAX_PIECE ? (total - offset) : MAX_PIECE;
    writeHeader(h, out.packets[i]);
    memcpy(out.packets[i] + HEADER_SIZE, run + offset, len);
    out.lengths[i] = HEADER_SIZE + len;
  }
  delete[] run;
  return true;
}

bool open(const Header& h, const uint8_t key[KEY_SIZE], const uint8_t* cipher, const size_t cipherLen,
          uint8_t* plainOut) {
  if (cipherLen < TAG_SIZE) return false;
  const size_t plainLen = cipherLen - TAG_SIZE;
  uint8_t aad[AAD_SIZE];
  uint8_t iv[12];
  associatedData(h, aad);
  nonce(h, iv);
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key, iv);
  const int bad = crypto_aead_read(&ctx, plainOut, cipher + plainLen, aad, sizeof(aad), cipher, plainLen);
  crypto_wipe(&ctx, sizeof(ctx));
  return bad == 0;
}

Reassembler::~Reassembler() {
  for (auto& s : slots_) delete[] s.buffer;
  delete[] done_;
}

Reassembler::Slot* Reassembler::find(const Header& h, const uint32_t nowMs) {
  Slot* empty = nullptr;
  Slot* oldest = nullptr;
  for (auto& s : slots_) {
    if (s.buffer && nowMs - s.startedMs > 60000) {
      delete[] s.buffer;
      s = Slot{};
    }
    if (s.buffer && s.id == h.messageId() && s.type == h.type) return &s;
    if (!s.buffer && !empty) empty = &s;
    if (s.buffer && (!oldest || s.startedMs < oldest->startedMs)) oldest = &s;
  }
  Slot* s = empty ? empty : oldest;
  delete[] s->buffer;
  *s = Slot{};
  s->buffer = new uint8_t[MAX_MESSAGE];
  s->id = h.messageId();
  s->type = h.type;
  s->count = h.pieceCount;
  s->startedMs = nowMs;
  return s;
}

bool Reassembler::add(const Header& h, const uint8_t* packet, const size_t len, const uint32_t nowMs,
                      const uint8_t** data, size_t* outLen) {
  delete[] done_;
  done_ = nullptr;
  if (len < HEADER_SIZE) return false;
  const uint8_t* body = packet + HEADER_SIZE;
  const size_t bodyLen = len - HEADER_SIZE;

  if (h.pieceCount == 1) {
    done_ = new uint8_t[bodyLen ? bodyLen : 1];
    memcpy(done_, body, bodyLen);
    *data = done_;
    *outLen = bodyLen;
    return true;
  }

  // Every piece but the last is full.
  const bool last = h.pieceIndex == h.pieceCount - 1;
  if ((!last && bodyLen != MAX_PIECE) || bodyLen == 0 || bodyLen > MAX_PIECE) return false;

  Slot* s = find(h, nowMs);
  if (s->count != h.pieceCount) return false;
  memcpy(s->buffer + h.pieceIndex * MAX_PIECE, body, bodyLen);
  s->received |= uint16_t(1u << h.pieceIndex);
  if (last) s->lastLen = bodyLen;
  if (s->received != uint16_t((1u << s->count) - 1)) return false;

  done_ = s->buffer;
  *data = done_;
  *outLen = (s->count - 1) * MAX_PIECE + s->lastLen;
  *s = Slot{};
  return true;
}

}  // namespace mesh
