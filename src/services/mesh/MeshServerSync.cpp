// The mesh node, part: Talking to the Pi server over Wi-Fi (protocol/PROTOCOL.md, "Server over Wi-Fi").

#include "MeshNode.h"

#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Preferences.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <esp_random.h>
#ifdef BOARD_HAS_PSRAM
#include <esp_heap_caps.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>

#include "MeshGps.h"
#include "MeshRadio.h"
#include "MeshSettings.h"
#include "MeshTime.h"
#include "core/network/HttpDownloader.h"
#include "core/waddle/WaddleVersion.h"
#include "core/waddle/WaddleWifi.h"

#include "MeshNodeInternal.h"

namespace mesh {

using namespace internal;

void Node::startWifiSync() {
  if (!network_.configured || network_.serverUrl.empty() || wifi_ != WifiStep::Idle) return;
  // File Transfer's own hotspot: leave it alone.
  if (WiFi.getMode() & WIFI_AP) return;
  // Already on (File Transfer, an update): used, and left on afterwards.
  wifiWasOn_ = WiFi.status() == WL_CONNECTED;
  if (!wifiWasOn_ && !waddle::wifi::joinSaved()) return;
  wifi_ = WifiStep::Joining;
  wifiStartedMs_ = millis();
  changes_++;
}

std::string Node::serverRequest() {
  // My messages the server has not confirmed, then a sync request.
  std::string body;
  auto add = [&](const uint8_t* raw, const size_t len) {
    body += char(len);
    body.append(reinterpret_cast<const char*>(raw), len);
  };
  std::vector<uint32_t> mine;
  for (const auto& p : pending_) mine.push_back(p.counter);
  mine.insert(mine.end(), controlWaiting_.begin(), controlWaiting_.end());
  for (const uint32_t counter : mine) {
    for (const auto& raw : store_.packetsAfter(network_.me, counter - 1, network_.me, MAX_PIECES)) {
      Header h;
      if (readHeader(raw.data(), raw.size(), h) && h.counter == counter) add(raw.data(), raw.size());
    }
  }
  SyncReq req;
  for (uint8_t m = 1; m <= MAX_MEMBERS; m++) req.lastCounter[m - 1] = store_.lastCounter(m);
  req.battery = battery();
  req.build = buildByte();
  req.flags = syncFlags();
  req.rosterVersion = network_.rosterVersion;
  uint8_t sync[HEADER_SIZE + SYNC_REQ_MAX];
  Header h = header(Type::SyncReq, SERVER);
  h.hops = 0;
  writeHeader(h, sync);
  add(sync, HEADER_SIZE + writeSyncReq(req, sync + HEADER_SIZE));
  // Messages the chain says are missing.
  for (const auto& f : fetches_) {
    uint8_t raw[HEADER_SIZE + FETCH_SIZE];
    Header fh = header(Type::Fetch, SERVER);
    fh.hops = 0;
    writeHeader(fh, raw);
    writeFetch(f, raw + HEADER_SIZE);
    add(raw, sizeof(raw));
  }
  return body;
}

size_t Node::takeServerAnswer(const std::string& answer, const uint32_t serverTime) {
  wifiServerFailed_ = false;
  serverAtMs_ = millis();
  lastCheckUnix_ = unixNow();
  fetches_.clear();  // asked; what came is in the answer
  // The server's clock, for a unit that has none (or one that drifted).
  if (serverTime && (!unixNow() || std::abs(int64_t(serverTime) - int64_t(unixNow())) > 60)) {
    halClock.setUtc(serverTime);
  }
  size_t i = 0, packets = 0;
  while (i < answer.size()) {
    const size_t n = uint8_t(answer[i]);
    if (n == 0 || i + 1 + n > answer.size()) break;
    handle(reinterpret_cast<const uint8_t*>(answer.data() + i + 1), n, 0, true);
    i += 1 + n;
    packets++;
  }
  LOG_INF("MESH", "Server sync: %u packets", unsigned(packets));
  return packets;
}

size_t Node::runWifiSync() {
  lastWifiSyncMs_ = millis();
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP || network_.serverCert.empty()) {
    wifiServerFailed_ = true;
    return 0;
  }
  const std::string body = serverRequest();
  freeink::SecureHttpClient http;
  http.setCACert(network_.serverCert.c_str());  // only the Pi's own certificate is trusted
  http.setTimeout(10000);
  http.setReuse(false);
  if (!http.begin(network_.serverUrl + "/api/packets")) {
    wifiServerFailed_ = true;
    return 0;
  }
  http.addHeader("Authorization", "Bearer " + network_.serverToken);
  http.addHeader("Content-Type", "application/octet-stream");
  const int status = http.POST(body);
  if (status != 200) {
    LOG_ERR("MESH", "Server sync failed: %d", status);
    wifiServerFailed_ = true;
    return 0;
  }
  return takeServerAnswer(http.getString(), httpDate(http.getHeader("Date")));
}
}  // namespace mesh
