#pragma once

// Shared by the parts of the node (MeshNode.cpp, MeshReceive.cpp, MeshKeys.cpp,
// MeshServerSync.cpp): limits, timings and small helpers. Not for other code.

#include <HalPowerManager.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "MeshConfig.h"
#include "MeshTime.h"
#include "core/waddle/WaddleVersion.h"

namespace mesh::internal {
inline constexpr const char* POSITIONS_PATH = "/.mesh/positions.sealed";
inline constexpr const char* ROSTER_PACKETS_PATH = "/.mesh/rosterpk.bin";
inline constexpr size_t MAX_OUTBOX = 40;
inline constexpr size_t MAX_PENDING = 20;
inline constexpr size_t MAX_SYNC_ANSWER = 8;
inline constexpr uint32_t RETRY_MS[] = {30000, 120000, 480000};
inline constexpr uint32_t WIFI_JOIN_MS = 20000;
inline constexpr uint32_t WIFI_FALLBACK_GAP_MS = 15UL * 60 * 1000;
inline constexpr uint32_t SERVER_FRESH_MS = 30UL * 60 * 1000;
inline constexpr uint32_t SOS_REPEAT_MS = 60000;
inline constexpr uint8_t LOW_BATTERY = 20;
inline constexpr uint32_t CHECK_LISTEN_MS = 20000;  // how long a check listens for answers
inline constexpr uint32_t CHECK_WIFI_GAP_MS = 10UL * 60 * 1000;
inline constexpr uint32_t BUSY_SOON_MS = 5000;  // packets due sooner keep the unit awake
inline constexpr size_t MAX_UPLOAD_PER_SYNC = 250;  // heard packets per request (about 50 KB)
inline constexpr size_t SERVER_ANSWER_FULL = 200;  // the server's most packets per Wi-Fi answer
inline constexpr uint32_t RESEND_GAP_MS = 30UL * 60 * 1000;  // unconfirmed keys and invitations

inline uint32_t randomBetween(const uint32_t lo, const uint32_t hi) { return lo + esp_random() % (hi - lo + 1); }

inline uint32_t unixNow() {
  const time_t t = time(nullptr);
  return t > 1600000000 ? uint32_t(t) : 0;
}

inline uint8_t battery() { return uint8_t(std::min<uint16_t>(powerManager.getBatteryPercentage(), 100)); }

// The first byte of the short git hash, for the server's status page.
inline uint8_t buildByte() {
  uint8_t b = 0;
  const char* hash = waddle::build();
  return hash && strlen(hash) >= 2 && fromHex(std::string(hash, 2).c_str(), &b, 1) ? b : 0;
}

// "Tue, 27 Oct 2026 18:02:11 GMT" -> unix seconds, or 0.
inline uint32_t httpDate(const std::string& text) {
  static const char* MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char month[4] = {};
  int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
  if (sscanf(text.c_str(), "%*3s, %d %3s %d %d:%d:%d", &day, month, &year, &hh, &mm, &ss) != 6) return 0;
  const char* found = strstr(MONTHS, month);
  if (!found || year < 2020) return 0;
  const int mon = int(found - MONTHS) / 3 + 1;
  return unixFromCivil(year, mon, day, hh, mm, ss);
}

// Two packets are the same when type, sender, counter and piece match (hops may differ).
inline bool samePacket(const uint8_t* a, const uint8_t* b) {
  return a[1] == b[1] && a[4] == b[4] && memcmp(a + 6, b + 6, 5) == 0;
}

}  // namespace mesh::internal
