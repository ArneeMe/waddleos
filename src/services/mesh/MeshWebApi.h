#pragma once

#include <WebServer.h>

// The mesh unit's endpoints on the file transfer web server, used by `waddle
// mesh join` and `check` on a computer (local network only, while File
// Transfer is open on the unit; protocol/PROTOCOL.md, "Joining"):
//
//   GET  /api/mesh/identity   {"publicKey": hex, "configured": bool, "radio", "gps", ...};
//                             with ?commit=hex, starts the setup code: {"nonce": hex}
//   POST /api/mesh/pair       {"tool", "nonce"}: the screen shows the setup code
//   POST /api/mesh/join       the invitation, sealed by that tool -> the sealed request
//   POST /api/mesh/welcome    the admin's sealed welcome; the unit starts in the network
namespace mesh::web {

void registerRoutes(WebServer& server);
// True once after a new group was saved: the node should load it.
bool takeGroupChanged();
// The setup code to show on the File Transfer screen, or nullptr before a tool pairs.
const char* setupCode();

}  // namespace mesh::web
