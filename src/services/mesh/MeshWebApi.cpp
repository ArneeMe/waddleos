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
constexpr size_t MAX_GROUP_JSON = 8192;  // four members, three small groups and a certificate
}  // namespace

bool takeGroupChanged() {
  const bool changed = groupChanged;
  groupChanged = false;
  return changed;
}

void registerRoutes(WebServer& server) {
  server.on("/api/mesh/identity", HTTP_GET, [&server] {
    const Identity& id = identity();
    const Group& group = Node::get().group();
    JsonDocument doc;
    doc["publicKey"] = toHex(id.publicKey, KEY_SIZE);
    // The sender key only sealed for the asking tool (MeshCrypto.h, "Setup"),
    // never in the clear on the Wi-Fi.
    uint8_t tool[KEY_SIZE];
    if (fromHex(server.arg("tool").c_str(), tool, KEY_SIZE)) {
      uint8_t key[KEY_SIZE], box[KEY_SIZE + TAG_SIZE];
      setupKey(id.secret, id.publicKey, tool, key);
      setupSeal(key, 1, id.senderKey, KEY_SIZE, box);
      doc["senderKeyBox"] = toHex(box, sizeof(box));
    }
    doc["configured"] = group.configured;
    if (group.configured) {
      doc["group"] = group.id;
      doc["me"] = group.me;
      doc["name"] = group.name(group.me);
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

  // `waddle mesh reset-keys`: new keys, no group. Used after a unit is lost,
  // then everyone is set up again without it.
  server.on("/api/mesh/reset-keys", HTTP_POST, [&server] {
    resetIdentity();
    groupChanged = true;
    LOG_INF("MESH", "Made new keys; waiting for setup");
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/mesh/group", HTTP_POST, [&server] {
    const String& body = server.arg("plain");
    if (body.length() == 0 || body.length() > 2 * MAX_GROUP_JSON + 200) {
      server.send(400, "text/plain", "tom eller for stor");
      return;
    }
    // {"tool": public key, "box": the group, sealed with the setup key}.
    JsonDocument sealed;
    uint8_t tool[KEY_SIZE];
    const char* boxHex = nullptr;
    if (deserializeJson(sealed, body.c_str(), body.length()) ||
        !fromHex(sealed["tool"] | "", tool, KEY_SIZE) || !(boxHex = sealed["box"] | nullptr)) {
      server.send(400, "text/plain", "ikke forseglet (gammelt waddle-verktøy?)");
      return;
    }
    const size_t boxLen = strlen(boxHex) / 2;
    if (boxLen <= TAG_SIZE || boxLen > MAX_GROUP_JSON + TAG_SIZE) {
      server.send(400, "text/plain", "feil lengde");
      return;
    }
    std::vector<uint8_t> box(boxLen), json(boxLen - TAG_SIZE);
    uint8_t key[KEY_SIZE];
    const Identity& id = identity();
    setupKey(id.secret, id.publicKey, tool, key);
    if (!fromHex(boxHex, box.data(), boxLen) || !setupOpen(key, 2, box.data(), boxLen, json.data())) {
      server.send(400, "text/plain", "kunne ikke åpne (endret underveis?)");
      return;
    }
    if (const char* err = saveGroupJson(reinterpret_cast<const char*>(json.data()), json.size())) {
      LOG_ERR("MESH", "group rejected: %s", err);
      server.send(400, "text/plain", err);
      return;
    }
    groupChanged = true;
    server.send(200, "text/plain", "OK");
  });
}

}  // namespace mesh::web
