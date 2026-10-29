#pragma once

#include <WebServer.h>

// The mesh unit's endpoints on the file transfer web server, used by `waddle
// mesh join` and `check` on a computer (local network only, while File
// Transfer is open on the unit; protocol/PROTOCOL.md, "Joining"):
//
//   GET  /api/mesh/identity   {"publicKey": hex, "configured": bool, "radio", "gps", ...}
//   POST /api/mesh/join       the invitation, sealed by the tool -> the sealed request
//   POST /api/mesh/welcome    the admin's sealed welcome; the unit starts in the network
namespace mesh::web {

void registerRoutes(WebServer& server);
// True once after a new group was saved: the node should load it.
bool takeGroupChanged();

}  // namespace mesh::web
