#include "MeshWebApi.h"

#include <ArduinoJson.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <Logging.h>

#include <esp_random.h>

#include <cstring>
#include <ctime>
#include <vector>

#include "MeshConfig.h"
#include "MeshGps.h"
#include "MeshNode.h"
#include "MeshPairing.h"
#include "MeshRadio.h"
#include "core/waddle/WaddleVersion.h"

namespace mesh::web {

namespace {
bool networkChanged = false;
}  // namespace

const char* setupCode() { return pairing::code(); }

bool takeNetworkChanged() {
  const bool changed = networkChanged;
  networkChanged = false;
  return changed;
}

namespace {

// A body sealed by the tool for this unit: {"tool": its one-time public key,
// "box": hex, sealed with the setup key, nonce byte 2}. Empty on failure.
std::string openFromTool(const String& body) {
  JsonDocument sealed;
  if (body.length() == 0 || body.length() > 8192 || deserializeJson(sealed, body.c_str(), body.length())) return "";
  return pairing::open(sealed["tool"] | "", sealed["box"] | "");
}

}  // namespace

void registerRoutes(WebServer& server) {
  server.on("/api/mesh/identity", HTTP_GET, [&server] {
    const Identity& id = identity();
    const Network& group = Node::get().network();
    JsonDocument doc;
    doc["publicKey"] = toHex(id.publicKey, KEY_SIZE);
    // A tool starting setup: its commitment in, my nonce out (a new one each time).
    const std::string nonce = pairing::start(server.arg("commit").c_str());
    if (!nonce.empty()) doc["nonce"] = nonce;
    doc["configured"] = group.configured;
    if (group.configured) {
      doc["network"] = group.id;
      doc["me"] = group.me;
      doc["name"] = group.name(group.me);
      doc["roster"] = group.rosterVersion;
    }
    // For `waddle mesh new-unit` and `waddle mesh check`: is the hardware alive?
    doc["radio"] = radio::isUp();
    doc["gps"] = gps::model();
    doc["battery"] = powerManager.getBatteryPercentage();
    doc["version"] = waddle::version();
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // How the unit is, for `waddle mesh check` (no keys, no message text).
  server.on("/api/mesh/diag", HTTP_GET, [&server] {
    Node& node = Node::get();
    const Network& group = node.network();
    JsonDocument doc;
    doc["version"] = waddle::version();
    doc["uptimeS"] = millis() / 1000;
    doc["resetReason"] = HalSystem::resetReason();
    doc["crashReport"] = Storage.exists("/crash_report.txt");
    const time_t now = time(nullptr);
    doc["clockSet"] = now > 1600000000;
    doc["time"] = uint32_t(now);
    doc["sd"] = Storage.ready();
    const auto internal = HalMemory::getInternalHeap();
    doc["heapFree"] = internal.freeBytes;
    doc["heapLargest"] = internal.largestBlockBytes;
    doc["heapMin"] = internal.minFreeBytes;
    doc["psramFree"] = HalMemory::getPsramHeap().freeBytes;
    doc["configured"] = group.configured;
    doc["me"] = group.me;
    doc["roster"] = group.rosterVersion;
    doc["radio"] = radio::isUp();
    doc["gps"] = gps::model();
    doc["battery"] = powerManager.getBatteryPercentage();
    doc["outbox"] = node.outboxSize();
    doc["pending"] = node.pendingSize();
    doc["airtimeMs"] = node.airtimeLastHourMs();  // of 216000 a rolling hour
    doc["server"] = node.serverState() == ServerState::Online ? "online"
                    : node.serverState() == ServerState::Offline ? "offline"
                                                                  : "unknown";
    doc["lastCheck"] = node.lastCheckUnix();
    doc["unread"] = node.store().totalUnread();
    doc["logs"] = getLastLogs();
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // The tool shows its key and nonce; the screen shows the code to compare.
  server.on("/api/mesh/pair", HTTP_POST, [&server] {
    JsonDocument in;
    const String& body = server.arg("plain");
    if (body.length() > 1024 || deserializeJson(in, body.c_str(), body.length())) {
      server.send(400, "text/plain", "ikke JSON");
      return;
    }
    if (!pairing::reveal(in["tool"] | "", in["nonce"] | "")) {
      server.send(409, "text/plain", "passer ikke med det verktøyet lovet");
      return;
    }
    server.send(200, "text/plain", "OK");
  });

  // The member's phone, after the code on this screen matched the phone's: a
  // sealed copy of this unit's keys and network (protocol/PROTOCOL.md, "Devices").
  server.on("/api/mesh/phone", HTTP_POST, [&server] {
    if (openFromTool(server.arg("plain")).empty()) {
      server.send(400, "text/plain", "ikke forseglet");
      return;
    }
    const std::string copy = phoneCopy();
    if (copy.empty()) {
      server.send(409, "text/plain", "enheten er ikke med i et nettverk");
      return;
    }
    JsonDocument out;
    out["box"] = pairing::seal(copy, PHONE_COPY_NONCE);
    String text;
    serializeJson(out, text);
    server.send(200, "application/json", text);
  });

  // `waddle mesh join`, step 1: the invitation (sealed by the tool) in, the
  // sealed request for the admin out (protocol/PROTOCOL.md, "Joining").
  server.on("/api/mesh/join", HTTP_POST, [&server] {
    const std::string plain = openFromTool(server.arg("plain"));
    JsonDocument in;
    if (plain.empty() || deserializeJson(in, plain)) {
      server.send(400, "text/plain", "ikke forseglet (gammelt waddle-verktøy?)");
      return;
    }
    std::string box;
    char code[8];
    if (const char* err = startJoin(in["invite"] | "", in["name"] | "", box, code)) {
      server.send(400, "text/plain", err);
      return;
    }
    JsonDocument out;
    out["from"] = toHex(identity().publicKey, KEY_SIZE);
    out["box"] = box;
    out["code"] = code;
    String text;
    serializeJson(out, text);
    server.send(200, "application/json", text);
  });

  // Step 2: the admin's welcome (sealed with the join key) in; the unit starts.
  server.on("/api/mesh/welcome", HTTP_POST, [&server] {
    JsonDocument in;
    const String& body = server.arg("plain");
    if (body.length() == 0 || body.length() > 20000 || deserializeJson(in, body.c_str(), body.length())) {
      server.send(400, "text/plain", "ikke JSON");
      return;
    }
    if (const char* err = finishJoin(in["box"] | "")) {
      LOG_ERR("MESH", "welcome refused: %s", err);
      server.send(400, "text/plain", err);
      return;
    }
    networkChanged = true;
    server.send(200, "text/plain", "OK");
  });
}

}  // namespace mesh::web
