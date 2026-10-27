#include "MeshWebApi.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include "MeshConfig.h"
#include "MeshNode.h"

namespace mesh::web {

namespace {
bool groupChanged = false;
constexpr size_t MAX_GROUP_JSON = 8192;  // three members and a certificate
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
    doc["senderKey"] = toHex(id.senderKey, KEY_SIZE);
    doc["configured"] = group.configured;
    if (group.configured) {
      doc["group"] = group.id;
      doc["me"] = group.me;
      doc["name"] = group.name(group.me);
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/mesh/group", HTTP_POST, [&server] {
    const String& body = server.arg("plain");
    if (body.length() == 0 || body.length() > MAX_GROUP_JSON) {
      server.send(400, "text/plain", "tom eller for stor");
      return;
    }
    if (const char* err = saveGroupJson(body.c_str(), body.length())) {
      LOG_ERR("MESH", "group rejected: %s", err);
      server.send(400, "text/plain", err);
      return;
    }
    groupChanged = true;
    server.send(200, "text/plain", "OK");
  });
}

}  // namespace mesh::web
