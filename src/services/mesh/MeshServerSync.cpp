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
    for (const auto& raw : store_.packetsAfter(network_.myDevice(), counter - 1, network_.me, MAX_PIECES)) {
      Header h;
      if (readHeader(raw.data(), raw.size(), h) && h.counter == counter) add(raw.data(), raw.size());
    }
  }
  // My "delivered" and "read" waiting for the radio go up too. A device with
  // no radio (the phone) sends them only this way, so they leave its queue.
  for (const auto& o : outbox_) {
    if ((o.raw[1] & 0x0F) == uint8_t(Type::Ack) && o.raw[4] == network_.myDevice()) add(o.raw.data(), o.raw.size());
  }
  if (!radio::isUp()) {
    outbox_.erase(std::remove_if(outbox_.begin(), outbox_.end(),
                                 [&](const Outgoing& o) { return (o.raw[1] & 0x0F) == uint8_t(Type::Ack); }),
                  outbox_.end());
  }
  // What I heard over LoRa (others' packets), so the server and my phone have
  // it too; the body stays under the server's 64 KB.
  const auto heard = store_.toUpload(MAX_UPLOAD_PER_SYNC);
  for (const auto& raw : heard) add(raw.data(), raw.size());
  uploadedInRequest_ = heard.size();
  SyncReq req;
  for (int slot = 0; slot < MAX_DEVICES; slot++) req.lastCounter[slot] = store_.lastCounter(slotDevice(slot));
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
  // The server took the request: what I heard over LoRa is there now.
  if (uploadedInRequest_) store_.uploaded(uploadedInRequest_);
  uploadedInRequest_ = 0;
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
