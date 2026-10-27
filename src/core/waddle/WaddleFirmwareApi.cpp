#include "WaddleFirmwareApi.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WebServer.h>
#include <esp_ota_ops.h>

#include "core/network/FirmwareFlasher.h"

namespace waddle::firmware {

namespace {

// Written by the web server handler and read by the File Transfer loop. Both
// run on the main loop task, so no locking is needed.
std::string pendingPath;

bool isSafeBinPath(const String& path) {
  return path.startsWith("/") && path.endsWith(".bin") && path.indexOf("..") < 0;
}

void sendJson(WebServer& server, const int code, const char* status, const char* detail) {
  String body = String("{\"status\":\"") + status + "\",\"detail\":\"" + detail + "\"}";
  server.send(code, "application/json", body);
}

void handleApply(WebServer& server) {
  const String path = server.arg("path");
  if (!isSafeBinPath(path)) {
    sendJson(server, 400, "error", "path must be an absolute .bin path");
    return;
  }
  if (!Storage.exists(path.c_str())) {
    sendJson(server, 404, "error", "file not found on the SD card");
    return;
  }
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    sendJson(server, 500, "error", "no OTA partition");
    return;
  }
  // Same full check the SD update runs, so the caller gets the error now.
  const auto result = firmware_flash::validateImageFile(path.c_str(), dest->size);
  if (result != firmware_flash::Result::OK) {
    LOG_ERR("WADDLE", "apply rejected %s: %s", path.c_str(), firmware_flash::resultName(result));
    sendJson(server, 422, "invalid", firmware_flash::resultName(result));
    return;
  }
  pendingPath = path.c_str();
  LOG_INF("WADDLE", "firmware queued: %s", pendingPath.c_str());
  sendJson(server, 202, "confirm-on-device", "confirm the update on the reader");
}

}  // namespace

void registerRoutes(WebServer& server) {
  server.on("/api/firmware/apply", HTTP_POST, [&server] { handleApply(server); });
}

bool takePending(std::string& path) {
  if (pendingPath.empty()) return false;
  path = std::move(pendingPath);
  pendingPath.clear();
  return true;
}

}  // namespace waddle::firmware
