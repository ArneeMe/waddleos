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
#include "MeshRadio.h"
#include "core/waddle/WaddleVersion.h"

namespace mesh::web {

namespace {
bool networkChanged = false;

// The setup code (MeshCrypto.h, pairCode): the tool's commitment, my nonce,
// then its key and nonce, and the six digits for the screen.
struct Pairing {
  uint8_t commit[32] = {};
  uint8_t unitNonce[PAIR_NONCE_SIZE] = {};
  uint8_t tool[KEY_SIZE] = {};
  bool committed = false;
  bool paired = false;
  char code[8] = "";
};
Pairing pairing;
}  // namespace

const char* setupCode() { return pairing.paired ? pairing.code : nullptr; }

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
  uint8_t tool[KEY_SIZE];
  const char* boxHex = nullptr;
  if (body.length() == 0 || body.length() > 4096 || deserializeJson(sealed, body.c_str(), body.length()) ||
      !fromHex(sealed["tool"] | "", tool, KEY_SIZE) || !(boxHex = sealed["box"] | nullptr)) {
    return "";
  }
  // Only from the tool whose code the person compared on the screen.
  if (!pairing.paired || memcmp(tool, pairing.tool, KEY_SIZE) != 0) return "";
  const size_t boxLen = strlen(boxHex) / 2;
  if (boxLen <= TAG_SIZE) return "";
  std::vector<uint8_t> box(boxLen);
  std::string plain(boxLen - TAG_SIZE, '\0');
  uint8_t key[KEY_SIZE];
  const Identity& id = identity();
  boxKey(id.secret, id.publicKey, tool, key);
  if (!fromHex(boxHex, box.data(), boxLen) ||
      !openBox(key, 2, box.data(), boxLen, reinterpret_cast<uint8_t*>(&plain[0]))) {
    return "";
  }
  return plain;
}

}  // namespace

void registerRoutes(WebServer& server) {
  server.on("/api/mesh/identity", HTTP_GET, [&server] {
    const Identity& id = identity();
    const Network& group = Node::get().network();
    JsonDocument doc;
    doc["publicKey"] = toHex(id.publicKey, KEY_SIZE);
    // A tool starting setup: its commitment in, my nonce out (a new one each time).
    if (fromHex(server.arg("commit").c_str(), pairing.commit, sizeof(pairing.commit))) {
      esp_fill_random(pairing.unitNonce, sizeof(pairing.unitNonce));
      pairing.committed = true;
      pairing.paired = false;
      doc["nonce"] = toHex(pairing.unitNonce, sizeof(pairing.unitNonce));
    }
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
    uint8_t tool[KEY_SIZE], toolNonce[PAIR_NONCE_SIZE], commit[32];
    if (body.length() > 1024 || deserializeJson(in, body.c_str(), body.length()) ||
        !fromHex(in["tool"] | "", tool, KEY_SIZE) || !fromHex(in["nonce"] | "", toolNonce, PAIR_NONCE_SIZE)) {
      server.send(400, "text/plain", "ikke JSON");
      return;
    }
    pairCommit(tool, toolNonce, commit);
    if (!pairing.committed || memcmp(commit, pairing.commit, sizeof(commit)) != 0) {
      server.send(409, "text/plain", "passer ikke med det verktøyet lovet");
      return;
    }
    pairing.committed = false;
    memcpy(pairing.tool, tool, KEY_SIZE);
    pairCode(tool, identity().publicKey, toolNonce, pairing.unitNonce, pairing.code);
    pairing.paired = true;
    server.send(200, "text/plain", "OK");
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
