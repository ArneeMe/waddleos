#pragma once

#include <WebServer.h>

// The mesh unit's setup endpoints on the file transfer web server, used by
// `waddle mesh setup` on Arne's computer (local network only, while File
// Transfer is open on the unit):
//
//   GET  /api/mesh/identity   {"publicKey": hex, "senderKey": hex, "configured": bool, ...}
//   POST /api/mesh/group      the group JSON (see MeshConfig.h); saved and loaded at once
namespace mesh::web {

void registerRoutes(WebServer& server);
// True once after a new group was saved: the node should load it.
bool takeGroupChanged();

}  // namespace mesh::web
