// The T5 mesh unit's screens, drawn by the real firmware code to PNG files.
//
//   screens <output dir>
//
// Arne's unit (member 1) runs the real mesh::Node, MessageStore and screens.
// Kari, Ola and Per are played here: their messages, positions, acks and an
// SOS reach the node as real encrypted packets over the simulated radio, so
// every screen shows what it would show on the device. The clock is fixed at
// tuesday 27 October 2026 14:32 (Europe/Oslo).

#include <MeshRoster.h>
#include <monocypher.h>
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
  // A group's newest key: the same one Arne's unit made and sent to them.
  const mesh::GroupKey* groupKey = mesh::isGroupConversation(conversation)
                                       ? node().groups().at(conversation - mesh::CONV_FIRST_GROUP).newest()
                                       : nullptr;
  const uint8_t* key = conversation == mesh::CONV_EVERYONE ? others[from].senderKey
                       : groupKey                       ? groupKey->key
                                                        : nullptr;
  uint8_t pair[mesh::KEY_SIZE];
  if (!key) {
    // A DM to me: the key we share.
    mesh::dmKey(others[from].secret, others[from].publicKey, mesh::identity().publicKey, pair);
    key = pair;
  }
  mesh::Header h;
  h.type = mesh::Type::Msg;
  h.network = GROUP_ID;
  h.hops = hops;
  h.sender = from;
  h.recipient = key == pair ? ARNE : groupKey ? mesh::TO_GROUP : mesh::EVERYONE;  // a DM is to me
  if (groupKey) h.groupId = node().groups().at(conversation - mesh::CONV_FIRST_GROUP).id;
  h.key = groupKey ? groupKey->number : key == pair ? 0 : 1;  // everyone: their sender key number 1
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
  h.network = GROUP_ID;
  h.hops = hops;
  h.sender = from;
  h.recipient = mesh::EVERYONE;
  h.key = 1;
  h.counter = ++others[from].counter;
  mesh::Sealed sealed;
  mesh::seal(h, others[from].senderKey, plain, sizeof(plain), sealed);
  hear(sealed.packets[0], sealed.lengths[0]);
  deliver();
}

// A sync request heard straight from a member's radio (tells their battery).
void syncRequest(const uint8_t from, const uint32_t when, const uint8_t battery) {
  at(when);
  uint8_t raw[mesh::HEADER_SIZE + mesh::SYNC_REQ_MAX];
  mesh::Header h;
  h.type = mesh::Type::SyncReq;
  h.network = GROUP_ID;
  h.hops = 0;
  h.sender = from;
  h.recipient = mesh::EVERYONE;
  h.counter = ++others[from].counter;
  mesh::writeHeader(h, raw);
  mesh::SyncReq req;
  req.battery = battery;
  hear(raw, mesh::HEADER_SIZE + mesh::writeSyncReq(req, raw + mesh::HEADER_SIZE));
  deliver();
}

// A member (or the server, 0) answers one of my packets: delivered, stored or read.
void ack(const uint8_t from, const uint32_t when, const uint32_t myCounter, const mesh::AckState state) {
  at(when);
  uint8_t raw[mesh::HEADER_SIZE + mesh::ACK_SIZE];
  mesh::Header h;
  h.type = mesh::Type::Ack;
  h.network = GROUP_ID;
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

// Joining as `waddle mesh join` and the admin's `approve` do it: an invitation,
// a signed roster of the four, and the sealed welcome.
std::string base64Url(const std::vector<uint8_t>& data) {
  static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  uint32_t bits = 0;
  int count = 0;
  for (const uint8_t b : data) {
    bits = (bits << 8) | b;
    count += 8;
    while (count >= 6) out += A[(bits >> (count -= 6)) & 63];
  }
  if (count) out += A[(bits << (6 - count)) & 63];
  return out;
}

void joinNetwork() {
  uint8_t adminSeed[32], adminBoxSecret[32], adminBox[32], adminSign[32], password[16], networkKey[32];
  for (int i = 0; i < 32; i++) {
    adminSeed[i] = uint8_t(i + 1);
    adminBoxSecret[i] = uint8_t(i * 3 + 2);
    networkKey[i] = uint8_t(i * 5 + 3);
  }
  for (int i = 0; i < 16; i++) password[i] = uint8_t(i * 7);
  mesh::publicKey(adminBoxSecret, adminBox);
  mesh::signPublicKey(adminSeed, adminSign);
  std::vector<uint8_t> invite = {GROUP_ID, 1, 2, 3, 4, 5, 6, 7, 8};
  invite.insert(invite.end(), password, password + 16);
  invite.insert(invite.end(), adminBox, adminBox + 32);
  invite.insert(invite.end(), 32, 0);
  const std::string url = "https://pi.example:8443";
  invite.insert(invite.end(), url.begin(), url.end());
  std::string box;
  char code[8];
  if (const char* err = mesh::startJoin("waddle://join/" + base64Url(invite), "Arne", box, code)) {
    std::cerr << "join: " << err << "\n";
    std::exit(1);
  }
  mesh::Roster roster;
  roster.network = GROUP_ID;
  roster.version = 1;
  for (uint8_t n = 1; n <= 4; n++) {
    mesh::RosterEntry e;
    e.number = n;
    e.admin = n == ARNE;
    e.name = NAMES[n];
    memcpy(e.publicKey, n == ARNE ? mesh::identity().publicKey : others[n].publicKey, 32);
    roster.members.push_back(e);
  }
  const auto signedRoster = mesh::writeRoster(roster, adminSeed);
  const std::string welcome = std::string("{\"network\":") + std::to_string(GROUP_ID) +
                              ",\"me\":1,\"name\":\"Arne\",\"networkKey\":\"" + mesh::toHex(networkKey, 32) +
                              "\",\"adminSign\":\"" + mesh::toHex(adminSign, 32) + "\",\"adminBox\":\"" +
                              mesh::toHex(adminBox, 32) + "\",\"counterFloor\":0,\"server\":{\"url\":\"" + url +
                              "\",\"token\":\"t1\",\"cert\":\"-----BEGIN CERTIFICATE-----\"},\"roster\":\"" +
                              mesh::toHex(signedRoster.data(), signedRoster.size()) + "\"}";
  // The admin's side of the join key.
  uint8_t input[32 + 16 + 64], k[32];
  crypto_x25519(input, adminBoxSecret, mesh::identity().publicKey);
  memcpy(input + 32, password, 16);
  memcpy(input + 48, mesh::identity().publicKey, 32);
  memcpy(input + 80, adminBox, 32);
  crypto_blake2b(k, 32, input, sizeof(input));
  std::vector<uint8_t> sealed(welcome.size() + mesh::TAG_SIZE);
  mesh::sealBox(k, 4, reinterpret_cast<const uint8_t*>(welcome.data()), welcome.size(), sealed.data());
  if (const char* err = mesh::finishJoin(mesh::toHex(sealed.data(), sealed.size()))) {
    std::cerr << "welcome: " << err << "\n";
    std::exit(1);
  }
}

void setUpGroup() {
  for (uint8_t n = 2; n <= 4; n++) {
    for (int i = 0; i < 32; i++) {
      others[n].secret[i] = uint8_t(n * 31 + i * 7 + 1);
      others[n].senderKey[i] = uint8_t(n * 17 + i * 13 + 5);
    }
    mesh::publicKey(others[n].secret, others[n].publicKey);
  }
  joinNetwork();
}

// The others' sender keys reach me as DMs, as after joining.
void senderKeys(const uint32_t when) {
  for (uint8_t n = 2; n <= 4; n++) {
    std::string text{'\0', 'S', 1};
    text.append(reinterpret_cast<const char*>(others[n].senderKey), mesh::KEY_SIZE);
    message(n, n, when, text);
  }
}

// --- The day so far ------------------------------------------------------------------

constexpr double HOME_LAT = 60.0000, HOME_LON = 10.6800;  // Nordmarka
uint8_t ARNE_KARI_OLA = 0;  // a group Arne makes in story()

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
  senderKeys(NOW - 26 * 3600 + MIN);

  // Yesterday evening, to everyone (relayed by Ola's unit).
  message(KARI, mesh::CONV_EVERYONE, NOW - 18 * 3600 - 17 * MIN, "Værmeldingen ser bra ut for i morgen!", 2);
  node().markRead(mesh::CONV_EVERYONE);

  // This morning: the three of us, and Kari and me.
  at(NOW - 3 * 3600 - MIN);
  ARNE_KARI_OLA = uint8_t(node().createGroup(mesh::memberBit(KARI) | mesh::memberBit(OLA), "Bilen til Ullevål"));
  deliver();
  message(KARI, ARNE_KARI_OLA, NOW - 3 * 3600, "Skal vi dele bil til Ullevålseter?");
  const uint32_t car = write(ARNE_KARI_OLA, NOW - 3 * 3600 + 4 * MIN, "Gjerne, jeg kan kjøre. Henter dere kl 09.");
  ack(KARI, NOW - 3 * 3600 + 10 * MIN, car, mesh::AckState::Read);
  message(KARI, KARI, NOW - 2 * 3600, "Har du en ekstra hodelykt?");
  node().markRead(KARI);
  const uint32_t lamp = write(KARI, NOW - 2 * 3600 + 3 * MIN, "Ja, jeg tar med to.");
  ack(KARI, NOW - 2 * 3600 + 5 * MIN, lamp, mesh::AckState::Read);

  // Everyone, today.
  message(KARI, mesh::CONV_EVERYONE, NOW - 80 * MIN, "Er alle klare for turen? Husk varme klær, det blir kaldt i kveld.");
  node().markRead(mesh::CONV_EVERYONE);
  const uint32_t ready = write(mesh::CONV_EVERYONE, NOW - 78 * MIN, "Ja! Jeg tar med kart og kompass.");
  ack(KARI, NOW - 77 * MIN, ready, mesh::AckState::Read);
  ack(OLA, NOW - 75 * MIN, ready, mesh::AckState::Read);
  ack(PER, NOW - 74 * MIN, ready, mesh::AckState::Delivered);

  message(OLA, ARNE_KARI_OLA, NOW - 40 * MIN, "Jeg er med!", 2);
  syncRequest(PER, NOW - 35 * MIN, 31);
  position(OLA, NOW - 22 * MIN, HOME_LAT - 0.0300, HOME_LON - 0.0150, 64, 1);
  message(OLA, mesh::CONV_EVERYONE, NOW - 12 * MIN, "Vi møtes ved parkeringen kl 10.", 2);
  message(PER, mesh::CONV_EVERYONE, NOW - 7 * MIN, "Jeg kommer litt senere, rundt halv elleve.");
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
    ChatActivity chat(renderer, mappedInputManager, mesh::CONV_EVERYONE);
    show(chat, "t5-chat-alle");
  }
  {
    ChatActivity chat(renderer, mappedInputManager, KARI);
    show(chat, "t5-chat-kari");
  }
  {
    ChatActivity chat(renderer, mappedInputManager, ARNE_KARI_OLA);
    show(chat, "t5-chat-gruppe");
  }
  {
    // The group's members, with "Fjern" tapped once on Ola (asks "Sikker?").
    GroupInfoActivity info(renderer, mappedInputManager, ARNE_KARI_OLA);
    show(info, "t5-gruppe", [](Activity& screen) {
      sim::tap(460, 330);  // Ola's "Fjern"
      screen.loop();
    });
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
