// What the simulator's stand-ins expect, filled in for the phone: no radio and
// no GPS (the phone reaches the network through the Pi only), the app's own
// files as the "SD card" and "flash", and the clock.

#include <MeshPacket.h>

#include <random>
#include <string>
#include <vector>

#include "services/mesh/MeshGps.h"
#include "services/mesh/MeshRadio.h"

namespace sim {
uint32_t nowMs = 0;  // set from the monotonic clock before each call (waddle_jni.cpp)
std::string rootDir;
std::mt19937 rng;  // unused here: esp_random comes from platform/esp_random.h
bool wifiAvailable = false;
bool wifiOn = false;
// The app does HTTPS itself (Node::serverRequest / takeServerAnswer).
int httpExchange(const std::string&, std::string&, std::string&) { return -1; }
}  // namespace sim

namespace mesh::radio {
bool begin(const LoraParams&) { return false; }
void pause() {}
void resume() {}
bool paused() { return true; }
bool isUp() { return false; }
bool receive(Received&) { return false; }
bool send(const uint8_t*, size_t) { return false; }
int irqPin() { return -1; }
void rearm() {}
void end() {}
}  // namespace mesh::radio

namespace mesh::gps {
namespace {
Fix none;
}
void wake() {}
void poll() {}
void sleep() {}
bool awake() { return false; }
const Fix& lastFix() { return none; }
uint32_t lastFixAtMs() { return 0; }
const char* model() { return "mobil"; }
}  // namespace mesh::gps
