#include "WaddlePrivacy.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>

#include <cstring>

namespace waddle::privacy {

namespace {

// SHA-256 of the factory MAC plus `salt`, so derived values are stable per
// device but do not reveal the factory MAC.
void deviceHash(const char* salt, uint8_t out[32]) {
  uint8_t factory[6] = {};
  esp_efuse_mac_get_default(factory);
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, factory, sizeof(factory));
  mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(salt), strlen(salt));
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

// Locally administered (bit 1 set), unicast (bit 0 clear), like phones use.
void setStationMac(uint8_t mac[6]) {
  mac[0] = (mac[0] & 0xFC) | 0x02;
  const esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, mac);
  if (err != ESP_OK) {
    LOG_ERR("PRIV", "esp_wifi_set_mac failed: %d", static_cast<int>(err));
  }
}

}  // namespace

void prepareStation(const char* ssid) {
  WiFi.setHostname(HOSTNAME);
  uint8_t hash[32];
  const std::string salt = std::string("waddle-mac:") + (ssid ? ssid : "");
  deviceHash(salt.c_str(), hash);
  setStationMac(hash);
}

void prepareScan() {
  uint8_t mac[6];
  esp_fill_random(mac, sizeof(mac));
  setStationMac(mac);
}

std::string hotspotPassword() {
  // No 0/O or 1/l/I, so it is easy to type from the screen.
  constexpr char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";
  uint8_t hash[32];
  deviceHash("waddle-hotspot", hash);
  std::string password;
  for (int i = 0; i < 10; i++) password += ALPHABET[hash[i] % (sizeof(ALPHABET) - 1)];
  return password;
}

}  // namespace waddle::privacy
