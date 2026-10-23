#include "WaddlePrivacy.h"

#include <esp_mac.h>
#include <mbedtls/sha256.h>

#include <cstring>

namespace waddle::privacy {

std::string hotspotPassword() {
  // SHA-256 of the factory MAC and a label: stable per device, and it does not
  // reveal the MAC. No 0/O or 1/l/I, so it is easy to type from the screen.
  constexpr char LABEL[] = "waddle-hotspot";
  constexpr char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";
  uint8_t factory[6] = {};
  esp_efuse_mac_get_default(factory);
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, factory, sizeof(factory));
  mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(LABEL), strlen(LABEL));
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  std::string password;
  for (int i = 0; i < 10; i++) password += ALPHABET[hash[i] % (sizeof(ALPHABET) - 1)];
  return password;
}

}  // namespace waddle::privacy
