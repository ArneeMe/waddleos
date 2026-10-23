#pragma once

#include <WebServer.h>

#include <string>

// WaddleOS: POST /api/firmware/apply?path=/file.bin
// Validates a .bin already uploaded to the SD card and queues it. The File
// Transfer screen then opens the SD firmware update with that file, which asks
// for confirmation on the reader before flashing.
namespace waddle::firmware {

void registerRoutes(WebServer& server);

// Returns true once per queued request and moves the path into `path`.
bool takePending(std::string& path);

}  // namespace waddle::firmware
