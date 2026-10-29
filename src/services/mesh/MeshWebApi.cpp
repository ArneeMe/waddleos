#include "MeshWebApi.h"

#include <ArduinoJson.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include <cstring>
#include <vector>

#include "MeshConfig.h"
#include "MeshGps.h"
#include "MeshNode.h"
#include "MeshRadio.h"
#include "core/waddle/WaddleVersion.h"

namespace mesh::web {

namespace {
bool groupChanged = false;
}  // namespace

bool takeGroupChanged() {
  const bool changed = groupChanged;
  groupChanged = false;
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
  const size_t boxLen = strlen(boxHex) / 2;
  if (boxLen <= TAG_SIZE) return "";
  std::vector<uint8_t> box(boxLen);
  std::string plain(boxLen - TAG_SIZE, '\0');
  uint8_t key[KEY_SIZE];
  const Identity& id = identity();
  setupKey(id.secret, id.publicKey, tool, key);
  if (!fromHex(boxHex, box.data(), boxLen) ||
      !setupOpen(key, 2, box.data(), boxLen, reinterpret_cast<uint8_t*>(&plain[0]))) {
    return "";
  }
  return plain;
}

}  // namespace

void registerRoutes(WebServer& server) {
  server.on("/api/mesh/identity", HTTP_GET, [&server] {
    const Identity& id = identity();
    const Group& group = Node::get().group();
    JsonDocument doc;
    doc["publicKey"] = toHex(id.publicKey, KEY_SIZE);
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
    groupChanged = true;
    server.send(200, "text/plain", "OK");
  });
}

}  // namespace mesh::web
