// One simulated mesh unit: the real MeshNode, MessageStore and MeshConfig code
// with the radio, GPS, SD card and clock replaced (test/sim/stubs). The test
// driver (test/sim/mesh_sim.py) runs three of these and plays the air between them.
//
// Commands on stdin, one per line; every command answers with lines ending in "ok":
//   identity               print this unit's public key and sender key
//   begin                  load the group and start (sends a SYNC_REQ)
//   time <ms>              set the simulated millis()
//   rx <hex>               a packet arrives over the radio
//   loop                   run the node once; radio sends come out as "tx <hex>"
//   send <conv> <text>     write a message (conv 0 = everyone, 1-4 = DM, 5+ = a group)
//   group <bits> <name>    make a group of me and these members; prints "conv <n>"
//   leave <conv> | remove <conv> <member>
//   read <conv>            the user opened the conversation
//   sos | cancel | seen <member>
//   wifi <0|1>             a known Wi-Fi network is (not) in range
//   wifisync               start a sync over Wi-Fi (HTTP goes to the driver)
//   gps <lat> <lon>        give the GPS a fix
//   trip <0|1>             mode: 0 = Hjemme, 1 = På tur (GPS on, position sharing)
//   checks <home> <trip>   check intervals (MeshSettings::Check)
//   check                  check for messages now (as when woken)
//   pause | resume         the radio sleeps (hears nothing) or listens
//   dump                   print the unit's state as one JSON line

#include <ArduinoJson.h>
#include <SecureHttpClient.h>

#include <filesystem>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "services/mesh/MeshGps.h"
#include "services/mesh/MeshNode.h"
#include "services/mesh/MeshRadio.h"
#include "services/mesh/MeshSettings.h"

namespace sim {
uint32_t nowMs = 1000;
std::string rootDir;
std::mt19937 rng;
bool wifiAvailable = false;
bool wifiOn = false;
std::vector<std::vector<uint8_t>> rxQueue;
}  // namespace sim

namespace {

std::string hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}

std::vector<uint8_t> unhex(const std::string& s) {
  std::vector<uint8_t> out(s.size() / 2);
  mesh::fromHex(s.c_str(), out.data(), out.size());
  return out;
}

mesh::gps::Fix simFix;

}  // namespace

// --- The radio and GPS, simulated ---------------------------------------------

namespace mesh::radio {
static bool up = false;
static bool sleeping = false;
bool begin(const LoraParams&) { return up = true; }
void pause() { sleeping = true; }
void resume() { sleeping = false; }
bool paused() { return sleeping; }
bool isUp() { return up; }
bool receive(Received& out) {
  if (sim::rxQueue.empty()) return false;
  const auto p = sim::rxQueue.front();
  sim::rxQueue.erase(sim::rxQueue.begin());
  out.len = p.size();
  memcpy(out.data, p.data(), p.size());
  out.rssi = -90;
  out.snr = 5;
  return true;
}
bool send(const uint8_t* data, size_t len) {
  sleeping = false;
  std::cout << "tx " << hex(data, len) << "\n";
  return true;
}
int irqPin() { return 10; }
void rearm() {}
void end() { up = false; }
}  // namespace mesh::radio

namespace mesh::gps {
static bool isAwake = false;
void wake() { isAwake = true; }
void poll() {}
void sleep() { isAwake = false; }
bool awake() { return isAwake; }
const Fix& lastFix() { return simFix; }
uint32_t lastFixAtMs() { return sim::nowMs; }
const char* model() { return "sim"; }
}  // namespace mesh::gps

// HTTP to the Pi goes to the driver, which runs the real server code.
int sim::httpExchange(const std::string& body, std::string& answer, std::string& date) {
  std::cout << "http " << hex(reinterpret_cast<const uint8_t*>(body.data()), body.size()) << std::endl;
  std::string line;
  if (!std::getline(std::cin, line)) return -1;
  std::istringstream in(line);
  std::string word, answerHex;
  int status = 0;
  in >> word >> status >> std::ws;
  std::getline(in, date, '|');
  in >> answerHex;
  const auto bytes = unhex(answerHex);
  answer.assign(bytes.begin(), bytes.end());
  return status;
}

static void dump() {
  auto& node = mesh::Node::get();
  const auto& group = node.group();
  JsonDocument doc;
  doc["me"] = group.me;
  doc["configured"] = group.configured;
  doc["unread"] = node.store().totalUnread();
  doc["sosActive"] = node.sosActive();
  doc["sosReceivedBy"] = node.sosReceivedBy();
  doc["sosSeenBy"] = node.sosSeenBy();
  doc["busy"] = node.busy();
  doc["listensAlways"] = node.listensAlways();
  doc["msUntilCheck"] = node.msUntilCheck();
  doc["lastCheck"] = node.lastCheckUnix();
  for (uint8_t c = 0; c < mesh::CONVERSATIONS; c++) {
    JsonArray msgs = doc["chats"][std::to_string(c)].to<JsonArray>();
    for (const auto& m : node.store().load(c, 100)) {
      JsonObject o = msgs.add<JsonObject>();
      o["sender"] = m.sender;
      o["counter"] = m.counter;
      o["text"] = m.text;
      o["delivered"] = m.deliveredTo;
      o["read"] = m.readBy;
      o["stored"] = m.stored;
      o["sent"] = m.sent;
      o["warning"] = m.chainWarning;
    }
  }
  for (int slot = 0; slot < mesh::MAX_GROUPS; slot++) {
    const auto& g = node.groups().at(slot);
    if (!g.id) continue;
    JsonObject o = doc["groups"][std::to_string(mesh::CONV_GROUPS + slot)].to<JsonObject>();
    o["id"] = g.id;
    o["members"] = g.members;
    o["left"] = g.left;
    o["name"] = g.name;
    o["key"] = g.newest() ? g.newest()->number : 0;
  }
  for (uint8_t n = 1; n <= mesh::MAX_MEMBERS; n++) {
    const auto& st = node.member(n);
    JsonObject o = doc["members"][std::to_string(n)].to<JsonObject>();
    o["hasPosition"] = st.hasPosition;
    o["lat"] = st.position.latE7;
    o["sos"] = st.sos;
  }
  serializeJson(doc, std::cout);
  std::cout << "\n";
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: sim_node <dir>\n";
    return 2;
  }
  sim::rootDir = argv[1];
  std::filesystem::create_directories(sim::rootDir);
  sim::rng.seed(std::hash<std::string>{}(sim::rootDir));
  auto& node = mesh::Node::get();

  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "identity") {
      const auto& id = mesh::identity();
      std::cout << "identity " << hex(id.publicKey, 32) << " " << hex(id.senderKey, 32) << "\n";
    } else if (cmd == "begin") {
      node.begin();
    } else if (cmd == "time") {
      in >> sim::nowMs;
    } else if (cmd == "rx") {
      std::string h;
      in >> h;
      if (!mesh::radio::paused()) sim::rxQueue.push_back(unhex(h));  // a sleeping radio hears nothing
    } else if (cmd == "loop") {
      node.loop();
    } else if (cmd == "send") {
      int conv = 0;
      in >> conv >> std::ws;
      std::string text;
      std::getline(in, text);
      std::cout << (node.sendText(uint8_t(conv), text) ? "sent" : "refused") << "\n";
    } else if (cmd == "read") {
      int conv = 0;
      in >> conv;
      node.markRead(uint8_t(conv));
    } else if (cmd == "sos") {
      node.startSos();
    } else if (cmd == "cancel") {
      node.cancelSos();
    } else if (cmd == "seen") {
      int from = 0;
      in >> from;
      node.acknowledgeSos(uint8_t(from));
    } else if (cmd == "takesos") {
      std::cout << "newsos " << int(node.takeNewSos()) << "\n";
    } else if (cmd == "wifi") {
      int on = 0;
      in >> on;
      sim::wifiAvailable = on != 0;
    } else if (cmd == "wifisync") {
      node.startWifiSync();
    } else if (cmd == "gps") {
      double lat = 0, lon = 0;
      in >> lat >> lon;
      simFix.valid = true;
      simFix.latE7 = int32_t(lat * 1e7);
      simFix.lonE7 = int32_t(lon * 1e7);
      simFix.time = uint32_t(time(nullptr));
    } else if (cmd == "group") {
      int bits = 0;
      in >> bits >> std::ws;
      std::string name;
      std::getline(in, name);
      std::cout << "conv " << int(node.createGroup(uint8_t(bits), name)) << "\n";
    } else if (cmd == "leave") {
      int conv = 0;
      in >> conv;
      node.leaveGroup(uint8_t(conv));
    } else if (cmd == "remove") {
      int conv = 0, member = 0;
      in >> conv >> member;
      node.removeFromGroup(uint8_t(conv), uint8_t(member));
    } else if (cmd == "trip") {
      int on = 0;
      in >> on;
      MESH_SETTINGS.onTrip = on ? 1 : 0;
    } else if (cmd == "checks") {
      int home = 0, trip = 0;
      in >> home >> trip;
      MESH_SETTINGS.homeCheck = uint8_t(home);
      MESH_SETTINGS.tripCheck = uint8_t(trip);
    } else if (cmd == "check") {
      node.startCheck();
    } else if (cmd == "pause") {
      mesh::radio::pause();
    } else if (cmd == "resume") {
      mesh::radio::resume();
    } else if (cmd == "dump") {
      dump();
    } else if (cmd == "quit") {
      break;
    }
    std::cout << "ok" << std::endl;
  }
  return 0;
}
