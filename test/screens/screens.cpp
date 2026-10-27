// The T5 mesh unit's screens, drawn by the real firmware code to PNG files.
//
//   screens <output dir>
//
// Arne's unit (member 1) runs the real mesh::Node, MessageStore and screens.
// Kari, Ola and Per are played here: their messages, positions, acks and an
// SOS reach the node as real encrypted packets over the simulated radio, so
// every screen shows what it would show on the device. The clock is fixed at
// tuesday 27 October 2026 14:32 (Europe/Oslo).

#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <MeshCrypto.h>
#include <builtinFonts/all.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "Stubs.h"
#include "apps/map/MapActivity.h"
#include "apps/messages/MeshUi.h"
#include "apps/messages/MessagesActivity.h"
#include "apps/sos/SosAlertActivity.h"
#include "core/MappedInputManager.h"
#include "core/activities/ActivityManager.h"
#include "core/activities/RenderLock.h"
#include "core/fontIds.h"
#include "core/waddle/WaddleDraw.h"
#include "products/t5s3/T5HomeActivity.h"
#include "services/mesh/MeshNode.h"
#include "services/mesh/MeshSettings.h"

// --- The device: renderer and fonts, as in WaddleMain.cpp --------------------------

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());

EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont smallFont(&ubuntu_8_regular);
EpdFontFamily smallFontFamily(&smallFont);
EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);
EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

namespace {

std::string outDir;

// --- PNG: 1-bit grayscale, stored (uncompressed) deflate, no zlib needed ------------

uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
  c = ~c;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  return ~c;
}

void put32be(std::string& s, uint32_t v) {
  for (int i = 3; i >= 0; i--) s += char((v >> (8 * i)) & 0xFF);
}

void chunk(std::string& png, const char* type, const std::string& data) {
  put32be(png, uint32_t(data.size()));
  std::string body = std::string(type, 4) + data;
  png += body;
  put32be(png, crc32(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
}

// The frame buffer as the reader sees it (portrait 540x960).
void writePng(const std::string& name) {
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const uint8_t* fb = display.getFrameBuffer();
  const int panelH = HalDisplay::DISPLAY_HEIGHT;
  const int rowBytes = (w + 7) / 8;
  std::string raw;
  for (int y = 0; y < h; y++) {
    raw += '\0';  // filter: none
    for (int bx = 0; bx < rowBytes; bx++) {
      uint8_t byte = 0;
      for (int b = 0; b < 8; b++) {
        const int x = bx * 8 + b;
        bool white = true;
        if (x < w) {
          // GfxRenderer's Portrait rotation: logical (x, y) is panel (y, panelH - 1 - x).
          const int px = y, py = panelH - 1 - x;
          white = fb[py * HalDisplay::DISPLAY_WIDTH_BYTES + px / 8] >> (7 - px % 8) & 1;
        }
        byte |= uint8_t(white) << (7 - b);
      }
      raw += char(byte);
    }
  }
  // zlib stream with stored blocks.
  std::string z = "\x78\x01";
  uint32_t a = 1, b = 0;
  for (unsigned char c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t pos = 0; pos < raw.size(); pos += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    z += char(pos + n == raw.size() ? 1 : 0);
    z += char(n & 0xFF);
    z += char(n >> 8);
    z += char(~n & 0xFF);
    z += char((~n >> 8) & 0xFF);
    z += raw.substr(pos, n);
  }
  put32be(z, (b << 16) | a);

  std::string ihdr;
  put32be(ihdr, uint32_t(w));
  put32be(ihdr, uint32_t(h));
  ihdr += std::string("\x01\x00\x00\x00\x00", 5);  // 1 bit, grayscale
  std::string png = "\x89PNG\r\n\x1a\n";
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", "");
  const std::string path = outDir + "/" + name + ".png";
  std::ofstream(path, std::ios::binary) << png;
  std::cout << path << "\n";
}

// --- Time ---------------------------------------------------------------------------

constexpr uint32_t NOW = 1793107920;  // 2026-10-27 14:32 Europe/Oslo
constexpr uint32_t MIN = 60;

// Moves the clock (time() and millis() together) to a unix time.
void at(const uint32_t unixTime) {
  if (sim::nowUnix && unixTime > sim::nowUnix) sim::nowMs += (unixTime - sim::nowUnix) * 1000;
  sim::nowUnix = unixTime;
}

// --- The other members ---------------------------------------------------------------

constexpr uint8_t GROUP_ID = 7;
constexpr uint8_t ARNE = 1, KARI = 2, OLA = 3, PER = 4;
constexpr const char* NAMES[] = {"", "Arne", "Kari", "Ola", "Per"};

struct Other {
  uint8_t secret[mesh::KEY_SIZE];
  uint8_t publicKey[mesh::KEY_SIZE];
  uint8_t senderKey[mesh::KEY_SIZE];
  uint32_t counter = 100;
};
Other others[5];
uint8_t smallKeys[16][mesh::KEY_SIZE];

mesh::Node& node() { return mesh::Node::get(); }

// Lets the node hear what is queued, then does its work.
void deliver() {
  for (int i = 0; i < 5; i++) node().loop();
}

void hear(const uint8_t* raw, size_t len) { sim::rxQueue.emplace_back(raw, raw + len); }

// A message from another member, as it is sent: encrypted and maybe in pieces.
void message(const uint8_t from, const uint8_t conversation, const uint32_t when, const std::string& text,
             const uint8_t hops = mesh::DEFAULT_HOPS) {
  at(when);
  const uint8_t* key = conversation == mesh::CONV_GROUP       ? others[from].senderKey
                       : mesh::isSmallConversation(conversation) ? smallKeys[conversation - mesh::CONV_SMALL]
                                                                 : nullptr;
  uint8_t pair[mesh::KEY_SIZE];
  if (!key) {
    // A DM to me: the key we share.
    mesh::pairKey(others[from].secret, others[from].publicKey, mesh::identity().publicKey, pair);
    key = pair;
  }
  mesh::Header h;
  h.type = mesh::Type::Msg;
  h.group = GROUP_ID;
  h.hops = hops;
  h.sender = from;
  h.recipient = key == pair ? ARNE : mesh::recipientOf(conversation);  // a DM is to me
  h.counter = ++others[from].counter;
  std::string plain(mesh::MSG_PREFIX, '\0');
  mesh::put32(reinterpret_cast<uint8_t*>(&plain[0]), when);
  // The chain: the hash of their previous message here, as their unit has it.
  node().store().lastHash(conversation, from, reinterpret_cast<uint8_t*>(&plain[4]));
  plain += text;
  static mesh::Sealed sealed;
  sealed = mesh::Sealed{};
  if (!mesh::seal(h, key, reinterpret_cast<const uint8_t*>(plain.data()), plain.size(), sealed)) {
    std::cerr << "could not seal: " << text << "\n";
    std::exit(1);
  }
  for (int i = 0; i < sealed.count; i++) hear(sealed.packets[i], sealed.lengths[i]);
  deliver();
}

// A position (or SOS) from another member.
void position(const uint8_t from, const uint32_t when, const double lat, const double lon, const uint8_t battery,
              const uint8_t hops = mesh::DEFAULT_HOPS, const mesh::Type type = mesh::Type::Pos) {
  at(when);
  mesh::Position p;
  p.time = when;
  p.latE7 = int32_t(lat * 1e7);
  p.lonE7 = int32_t(lon * 1e7);
  p.altitude = 420;
  p.battery = battery;
  uint8_t plain[mesh::POS_SIZE];
  mesh::writePosition(p, plain);
  mesh::Header h;
  h.type = type;
  h.group = GROUP_ID;
  h.hops = hops;
  h.sender = from;
  h.recipient = mesh::GROUP;
  h.counter = ++others[from].counter;
  mesh::Sealed sealed;
  mesh::seal(h, others[from].senderKey, plain, sizeof(plain), sealed);
  hear(sealed.packets[0], sealed.lengths[0]);
  deliver();
}

// A sync request heard straight from a member's radio (tells their battery).
void syncRequest(const uint8_t from, const uint32_t when, const uint8_t battery) {
  at(when);
  uint8_t raw[mesh::HEADER_SIZE + mesh::SYNC_REQ_SIZE];
  mesh::Header h;
  h.type = mesh::Type::SyncReq;
  h.group = GROUP_ID;
  h.hops = 0;
  h.sender = from;
  h.recipient = mesh::GROUP;
  h.counter = ++others[from].counter;
  mesh::writeHeader(h, raw);
  mesh::SyncReq req;
  req.battery = battery;
  mesh::writeSyncReq(req, raw + mesh::HEADER_SIZE);
  hear(raw, sizeof(raw));
  deliver();
}

// A member (or the server, 0) answers one of my packets: delivered, stored or read.
void ack(const uint8_t from, const uint32_t when, const uint32_t myCounter, const mesh::AckState state) {
  at(when);
  uint8_t raw[mesh::HEADER_SIZE + mesh::ACK_SIZE];
  mesh::Header h;
  h.type = mesh::Type::Ack;
  h.group = GROUP_ID;
  h.hops = mesh::DEFAULT_HOPS;
  h.sender = from;
  h.recipient = ARNE;
  h.counter = ++others[from].counter;
  mesh::writeHeader(h, raw);
  mesh::writeAck(mesh::Ack{ARNE, myCounter, state}, raw + mesh::HEADER_SIZE);
  hear(raw, sizeof(raw));
  deliver();
}

// I write a message; returns its counter.
uint32_t write(const uint8_t conversation, const uint32_t when, const std::string& text) {
  at(when);
  if (!node().sendText(conversation, text)) {
    std::cerr << "could not send: " << text << "\n";
    std::exit(1);
  }
  deliver();
  return node().store().load(conversation, 1).back().counter;
}

// /.mesh/group.json as `waddle mesh setup` writes it: four members, the small groups I am in.
void writeGroup() {
  const auto& me = mesh::identity();
  std::string json = "{\"group\":" + std::to_string(GROUP_ID) + ",\"me\":1,\"members\":[";
  for (uint8_t n = 1; n <= 4; n++) {
    const uint8_t* pub = n == ARNE ? me.publicKey : others[n].publicKey;
    const uint8_t* sender = n == ARNE ? me.senderKey : others[n].senderKey;
    json += std::string(n > 1 ? "," : "") + "{\"number\":" + std::to_string(n) + ",\"name\":\"" + NAMES[n] +
            "\",\"publicKey\":\"" + mesh::toHex(pub, 32) + "\",\"senderKey\":\"" + mesh::toHex(sender, 32) + "\"}";
  }
  json += "],\"groups\":[";
  bool first = true;
  for (uint8_t bits = 0; bits < 16; bits++) {
    if (!(bits & 1) || mesh::memberCount(bits) < 3 || bits == 0x0F) continue;
    json += std::string(first ? "" : ",") + "{\"members\":[";
    bool firstMember = true;
    for (uint8_t n = 1; n <= 4; n++) {
      if (!(bits & mesh::memberBit(n))) continue;
      json += std::string(firstMember ? "" : ",") + std::to_string(n);
      firstMember = false;
    }
    json += "],\"key\":\"" + mesh::toHex(smallKeys[bits], 32) + "\"}";
    first = false;
  }
  json += "],\"server\":{\"url\":\"https://pi.example:8443\",\"token\":\"t1\",\"cert\":\"-----BEGIN CERTIFICATE-----\"}}";
  std::filesystem::create_directories(sim::rootDir + "/.mesh");
  std::ofstream(sim::rootDir + "/.mesh/group.json") << json;
}

void setUpGroup() {
  for (uint8_t n = 2; n <= 4; n++) {
    for (int i = 0; i < 32; i++) {
      others[n].secret[i] = uint8_t(n * 31 + i * 7 + 1);
      others[n].senderKey[i] = uint8_t(n * 17 + i * 13 + 5);
    }
    mesh::publicKey(others[n].secret, others[n].publicKey);
  }
  for (int g = 0; g < 16; g++) {
    for (int i = 0; i < 32; i++) smallKeys[g][i] = uint8_t(g * 11 + i * 3 + 9);
  }
  writeGroup();
}

// --- The day so far ------------------------------------------------------------------

constexpr double HOME_LAT = 60.0000, HOME_LON = 10.6800;  // Nordmarka
const uint8_t ARNE_KARI_OLA = uint8_t(mesh::CONV_SMALL + 0b0111);

void story() {
  // On a trip: GPS on, position shared every 10 minutes, checks every hour.
  MESH_SETTINGS.onTrip = 1;
  sim::fix.valid = true;
  sim::fix.latE7 = int32_t(HOME_LAT * 1e7);
  sim::fix.lonE7 = int32_t(HOME_LON * 1e7);
  sim::fix.altitude = 412;
  sim::fix.time = NOW - 2 * MIN;

  at(NOW - 26 * 3600);
  node().begin();

  // Yesterday evening, to everyone (relayed by Ola's unit).
  message(KARI, mesh::CONV_GROUP, NOW - 18 * 3600 - 17 * MIN, "Værmeldingen ser bra ut for i morgen!", 2);
  node().markRead(mesh::CONV_GROUP);

  // This morning: the three of us, and Kari and me.
  message(KARI, ARNE_KARI_OLA, NOW - 3 * 3600, "Skal vi dele bil til Ullevålseter?");
  const uint32_t car = write(ARNE_KARI_OLA, NOW - 3 * 3600 + 4 * MIN, "Gjerne, jeg kan kjøre. Henter dere kl 09.");
  ack(KARI, NOW - 3 * 3600 + 10 * MIN, car, mesh::AckState::Read);
  message(KARI, KARI, NOW - 2 * 3600, "Har du en ekstra hodelykt?");
  node().markRead(KARI);
  const uint32_t lamp = write(KARI, NOW - 2 * 3600 + 3 * MIN, "Ja, jeg tar med to.");
  ack(KARI, NOW - 2 * 3600 + 5 * MIN, lamp, mesh::AckState::Read);

  // Everyone, today.
  message(KARI, mesh::CONV_GROUP, NOW - 80 * MIN, "Er alle klare for turen? Husk varme klær, det blir kaldt i kveld.");
  node().markRead(mesh::CONV_GROUP);
  const uint32_t ready = write(mesh::CONV_GROUP, NOW - 78 * MIN, "Ja! Jeg tar med kart og kompass.");
  ack(KARI, NOW - 77 * MIN, ready, mesh::AckState::Read);
  ack(OLA, NOW - 75 * MIN, ready, mesh::AckState::Read);
  ack(PER, NOW - 74 * MIN, ready, mesh::AckState::Delivered);

  message(OLA, ARNE_KARI_OLA, NOW - 40 * MIN, "Jeg er med!", 2);
  syncRequest(PER, NOW - 35 * MIN, 31);
  position(OLA, NOW - 22 * MIN, HOME_LAT - 0.0300, HOME_LON - 0.0150, 64, 1);
  message(OLA, mesh::CONV_GROUP, NOW - 12 * MIN, "Vi møtes ved parkeringen kl 10.", 2);
  message(PER, mesh::CONV_GROUP, NOW - 7 * MIN, "Jeg kommer litt senere, rundt halv elleve.");
  // The hourly check: Kari's position comes in while it listens.
  at(NOW - 5 * MIN - 10);
  node().startCheck();
  position(KARI, NOW - 5 * MIN, HOME_LAT + 0.0078, HOME_LON + 0.0140, 82);
  message(KARI, KARI, NOW - 4 * MIN, "Takk! Ses ved parkeringen.");

  // Ola has not answered yet; the server has it.
  const uint32_t weather = write(OLA, NOW - 3 * MIN, "Har du sjekket værmeldingen for i kveld?");
  ack(mesh::SERVER, NOW - 3 * MIN + 10, weather, mesh::AckState::Stored);
  at(NOW);
  deliver();
}

// --- Screens -------------------------------------------------------------------------

void show(Activity& screen, const std::string& name, const std::function<void(Activity&)>& then = {}) {
  screen.onEnter();
  screen.render(RenderLock());
  if (then) {
    then(screen);
    screen.render(RenderLock());
  }
  writePng(name);
}

}  // namespace

int main(int argc, char** argv) {
  outDir = argc > 1 ? argv[1] : "build/screens";
  std::filesystem::create_directories(outDir);
  // The unit's SD card and flash, fresh every run.
  sim::rootDir = outDir + "/sd";
  std::filesystem::remove_all(sim::rootDir);
  std::filesystem::create_directories(sim::rootDir);
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);  // Europe/Oslo, as Settings sets it
  tzset();

  I18n::getInstance().setLanguage(Language::NB);
  display.begin();
  renderer.begin();
  fontDecompressor.init();
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  setUpGroup();
  story();

  {
    T5HomeActivity home(renderer, mappedInputManager);
    show(home, "t5-hjem");
  }
  {
    // The default sleep screen (SleepActivity), with the line new messages add to it.
    renderer.clearScreen();
    waddle::drawSplash(renderer, tr(STR_SLEEPING));
    meshui::drawUnreadLine(renderer, node().store().totalUnread());
    writePng("t5-sover");
  }
  {
    MessagesActivity messages(renderer, mappedInputManager);
    show(messages, "t5-meldinger");
  }
  {
    // "Ny gruppe" with Kari ticked (a tap on her row).
    NewGroupActivity newGroup(renderer, mappedInputManager);
    show(newGroup, "t5-ny-gruppe", [](Activity& screen) {
      sim::tap(100, 200);  // Kari's row
      screen.loop();
    });
  }
  {
    ChatActivity chat(renderer, mappedInputManager, mesh::CONV_GROUP);
    show(chat, "t5-chat-alle");
  }
  {
    ChatActivity chat(renderer, mappedInputManager, KARI);
    show(chat, "t5-chat-kari");
  }
  {
    MapActivity map(renderer, mappedInputManager);
    show(map, "t5-kart");
  }
  {
    // Per sends an SOS from 2 km west.
    position(PER, NOW - 1 * MIN, HOME_LAT + 0.0020, HOME_LON - 0.0360, 29, mesh::DEFAULT_HOPS, mesh::Type::Sos);
    at(NOW);
    SosAlertActivity alert(renderer, mappedInputManager, node().takeNewSos());
    show(alert, "t5-sos-varsel");
  }
  {
    // My own SOS: sent, received by Kari and the server, seen by Kari.
    SosActivity sos(renderer, mappedInputManager);
    show(sos, "t5-sos", [](Activity&) {
      // Run the unit until the SOS has gone out over the radio.
      uint32_t counter = 0;
      for (int i = 0; i < 600 && !counter; i++) {
        sim::nowMs += 1000;
        node().loop();
        for (const auto& raw : sim::sent) {
          mesh::Header h;
          if (mesh::readHeader(raw.data(), raw.size(), h) && h.type == mesh::Type::Sos && h.sender == ARNE) counter = h.counter;
        }
      }
      ack(mesh::SERVER, NOW, counter, mesh::AckState::Delivered);
      ack(KARI, NOW, counter, mesh::AckState::Read);
    });
  }
  return 0;
}
