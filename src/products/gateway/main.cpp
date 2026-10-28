// WaddleOS mesh gateway: a Heltec WiFi LoRa 32 V4 on the Pi's USB port.
//
// While the Pi is there it listens on the mesh frequency, passes every
// WaddleOS packet it hears to the Pi and sends what the Pi asks it to
// (protocol/PROTOCOL.md, "Gateway serial link"). The Pi relays and answers.
//
// When the Pi has said nothing for 30 s (a power cut; the gateway on a power
// bank) it is standalone: it keeps the packets it hears in flash, relays and
// answers sync requests by itself (lib/WaddleMesh GatewayStore), and hands the
// kept packets to the Pi when it is back.
//
//   Gateway -> Pi:  RX <rssi> <snr> <hex>     HELLO waddle-gateway <version> <front end>
//                   PONG <kept> <version> <front end>
//                   STORED <hex> ... DUMPED <n>
//   Pi -> gateway:  TX <hex>                  answered with OK or ERR <reason>
//                   PING <group id>  (every 10 s)   DUMP   CLEAR (answered with OK)

#include <Arduino.h>
#include <GatewayStore.h>
#include <LittleFS.h>
#include <MeshPacket.h>
#include <MeshRadioRules.h>
#include <Preferences.h>
#include <RadioLib.h>

namespace {

// Heltec V4 pins (as Meshtastic's variants/esp32s3/heltec_v4/variant.h).
constexpr int PIN_LORA_CS = 8;
constexpr int PIN_LORA_SCK = 9;
constexpr int PIN_LORA_MOSI = 10;
constexpr int PIN_LORA_MISO = 11;
constexpr int PIN_LORA_RESET = 12;
constexpr int PIN_LORA_BUSY = 13;
constexpr int PIN_LORA_DIO1 = 14;
constexpr int PIN_LED = 35;
// The V4 has a front-end chip (PA and LNA) after the radio: a GC1109 on V4.2,
// a KCT8103L on V4.3. Both sit on the same power pin and enable pin.
constexpr int PIN_FEM_POWER = 7;
constexpr int PIN_FEM_ENABLE = 2;   // GC1109 CSD / KCT8103L CSD
constexpr int PIN_GC1109_PA = 46;   // GC1109 CPS: high = full PA when sending
constexpr int PIN_KCT8103L_CTX = 5;  // KCT8103L CTX: high when sending, low = receive LNA

// The front end adds about 11 dB, so 10 dBm from the radio is about 21 dBm at
// the antenna (the 869.4-869.65 MHz sub-band allows 27 dBm).
constexpr int8_t RADIO_POWER_DBM = 10;
constexpr const char* VERSION = "2";

// Standalone after this long without a line from the Pi (it pings every 10 s).
constexpr uint32_t PI_TIMEOUT_MS = 30000;
// Between the gateway's own packets when standalone, so it does not hog the air.
constexpr uint32_t OWN_GAP_MS = 500;
// The kept packets: one fixed-size record per GatewayStore slot (seq u32, length u8, packet).
constexpr const char* STORE_FILE = "/kept.bin";
constexpr size_t RECORD_SIZE = 5 + mesh::MAX_PACKET;

SX1262 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RESET, PIN_LORA_BUSY);
mesh::LoraParams params;
mesh::AirtimeBudget budget;
bool kct8103l = false;
volatile bool received = false;

Preferences prefs;
mesh::GatewayStore* store = nullptr;  // made in setup() from what NVS holds
mesh::GatewayStore::Result result;
uint32_t lastFromPi = 0;

// The gateway's own packets when standalone (relays, ACKs, answers), sent one by one.
constexpr int OUTBOX_SIZE = 24;
mesh::RawPacket outbox[OUTBOX_SIZE];
int outboxHead = 0;
int outboxUsed = 0;
uint32_t nextOwnSend = 0;

bool standalone() { return millis() - lastFromPi > PI_TIMEOUT_MS; }

void IRAM_ATTR onReceive() { received = true; }

void frontEndInit() {
  pinMode(PIN_FEM_POWER, OUTPUT);
  digitalWrite(PIN_FEM_POWER, HIGH);
  delay(1);
  // The KCT8103L pulls its enable pin up; the GC1109 does not.
  pinMode(PIN_FEM_ENABLE, INPUT);
  kct8103l = digitalRead(PIN_FEM_ENABLE) == HIGH;
  pinMode(PIN_FEM_ENABLE, OUTPUT);
  digitalWrite(PIN_FEM_ENABLE, HIGH);
  const int modePin = kct8103l ? PIN_KCT8103L_CTX : PIN_GC1109_PA;
  pinMode(modePin, OUTPUT);
  digitalWrite(modePin, LOW);
}

void frontEndTransmit(const bool on) { digitalWrite(kct8103l ? PIN_KCT8103L_CTX : PIN_GC1109_PA, on ? HIGH : LOW); }

void printHex(const uint8_t* data, const size_t len) {
  static const char* digits = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    Serial.write(digits[data[i] >> 4]);
    Serial.write(digits[data[i] & 15]);
  }
}

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void startReceive() {
  frontEndTransmit(false);
  radio.startReceive();
}

// Sends one packet within the airtime budget. Returns "" or why it was not sent.
const char* sendPacket(const uint8_t* data, const size_t len) {
  const uint32_t airtime = mesh::airtimeMs(params, len);
  if (!budget.allows(airtime, millis())) return "airtime";
  budget.spend(airtime, millis());
  frontEndTransmit(true);
  const int state = radio.transmit(data, len);
  startReceive();
  return state == RADIOLIB_ERR_NONE ? "" : "radio";
}

// --- Kept packets in flash ----------------------------------------------------

void writeRecord(File& f, const mesh::GatewayStore::Slot& s) {
  uint8_t record[RECORD_SIZE] = {};
  mesh::put32(record, s.seq);
  record[4] = s.packet.len;
  memcpy(record + 5, s.packet.data, s.packet.len);
  f.write(record, sizeof(record));
}

// An empty file of empty records, so each slot can be written in place.
void emptyStoreFile() {
  File f = LittleFS.open(STORE_FILE, "w");
  const mesh::GatewayStore::Slot empty;
  for (int i = 0; i < mesh::GatewayStore::SLOTS; i++) writeRecord(f, empty);
  f.close();
}

void saveSlot(const int index) {
  File f = LittleFS.open(STORE_FILE, "r+");
  if (!f) return;
  f.seek(size_t(index) * RECORD_SIZE);
  writeRecord(f, store->slot(index));
  f.close();
}

void loadSlots() {
  File f = LittleFS.open(STORE_FILE, "r");
  if (!f || f.size() != mesh::GatewayStore::SLOTS * RECORD_SIZE) {
    if (f) f.close();
    emptyStoreFile();
    return;
  }
  uint8_t record[RECORD_SIZE];
  for (int i = 0; i < mesh::GatewayStore::SLOTS; i++) {
    if (f.read(record, sizeof(record)) != sizeof(record)) break;
    mesh::GatewayStore::Slot s;
    s.seq = mesh::get32(record);
    s.packet.len = record[4] <= mesh::MAX_PACKET ? record[4] : 0;
    memcpy(s.packet.data, record + 5, s.packet.len);
    store->restore(i, s);
  }
  f.close();
}

// --- Standalone ---------------------------------------------------------------

void queueOwn(const mesh::RawPacket& p) {
  if (outboxUsed == OUTBOX_SIZE) return;  // full: the unit asks again
  outbox[(outboxHead + outboxUsed) % OUTBOX_SIZE] = p;
  outboxUsed++;
}

void sendOwn() {
  if (!outboxUsed || int32_t(millis() - nextOwnSend) < 0) return;
  const mesh::RawPacket& p = outbox[outboxHead];
  if (strcmp(sendPacket(p.data, p.len), "airtime") == 0) {
    nextOwnSend = millis() + 10000;  // over budget: try again later
    return;
  }
  outboxHead = (outboxHead + 1) % OUTBOX_SIZE;
  outboxUsed--;
  nextOwnSend = millis() + OWN_GAP_MS;
}

void handleStandalone(const uint8_t* data, const size_t len) {
  mesh::RawPacket copy;
  if (store->relay(data, len, copy)) queueOwn(copy);
  store->handle(data, len, result);
  if (result.savedSlot >= 0) saveSlot(result.savedSlot);
  // The counter is saved before any packet with it is sent.
  if (result.saveCounter) prefs.putUInt("counter", result.saveCounter);
  for (int i = 0; i < result.count; i++) queueOwn(result.packets[i]);
}

void handleReceived() {
  received = false;
  uint8_t data[255];
  const size_t len = radio.getPacketLength();
  const int state = radio.readData(data, len);
  const int rssi = int(radio.getRSSI());
  const float snr = radio.getSNR();
  startReceive();
  if (state != RADIOLIB_ERR_NONE) return;
  mesh::Header h;
  // Only WaddleOS packets count; other LoRa traffic on the frequency is ignored.
  if (!mesh::readHeader(data, len, h)) return;
  digitalWrite(PIN_LED, HIGH);
  if (standalone()) {
    handleStandalone(data, len);
  } else {
    Serial.printf("RX %d %.1f ", rssi, snr);
    printHex(data, len);
    Serial.write('\n');
  }
  digitalWrite(PIN_LED, LOW);
}

// --- Serial link --------------------------------------------------------------

void transmit(const char* hex) {
  uint8_t data[mesh::MAX_PACKET];
  const size_t hexLen = strlen(hex);
  if (hexLen % 2 || hexLen / 2 > sizeof(data) || hexLen < 2 * mesh::HEADER_SIZE) {
    Serial.println("ERR length");
    return;
  }
  for (size_t i = 0; i < hexLen / 2; i++) {
    const int hi = hexValue(hex[2 * i]);
    const int lo = hexValue(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      Serial.println("ERR hex");
      return;
    }
    data[i] = uint8_t(hi << 4 | lo);
  }
  const size_t len = hexLen / 2;
  mesh::Header h;
  if (!mesh::readHeader(data, len, h)) {
    Serial.println("ERR packet");
    return;
  }
  const char* error = sendPacket(data, len);
  if (*error) {
    Serial.printf("ERR %s\n", error);
  } else {
    Serial.println("OK");
  }
}

void pong(const char* groupText) {
  const int group = atoi(groupText);
  if (group > 0 && group < 256 && group != store->group()) {
    store->setGroup(uint8_t(group));
    prefs.putUChar("group", uint8_t(group));
  }
  Serial.printf("PONG %d %s %s\n", store->count(), VERSION, kct8103l ? "kct8103l" : "gc1109");
}

// Every kept packet, oldest first.
void dump() {
  int sent = 0;
  uint32_t after = 0;
  for (;;) {
    int next = -1;
    for (int i = 0; i < mesh::GatewayStore::SLOTS; i++) {
      const uint32_t seq = store->slot(i).seq;
      if (seq > after && (next < 0 || seq < store->slot(next).seq)) next = i;
    }
    if (next < 0) break;
    const mesh::RawPacket& p = store->slot(next).packet;
    Serial.print("STORED ");
    printHex(p.data, p.len);
    Serial.write('\n');
    after = store->slot(next).seq;
    sent++;
  }
  Serial.printf("DUMPED %d\n", sent);
}

void readSerial() {
  static char line[2 * mesh::MAX_PACKET + 8];
  static size_t used = 0;
  while (Serial.available()) {
    const char c = char(Serial.read());
    if (c == '\r') continue;
    if (c != '\n') {
      if (used < sizeof(line) - 1) line[used++] = c;
      continue;
    }
    line[used] = '\0';
    if (used > 0) lastFromPi = millis();
    if (strncmp(line, "TX ", 3) == 0) {
      transmit(line + 3);
    } else if (strncmp(line, "PING ", 5) == 0) {
      pong(line + 5);
    } else if (strcmp(line, "DUMP") == 0) {
      dump();
    } else if (strcmp(line, "CLEAR") == 0) {
      store->clear();
      emptyStoreFile();
      Serial.println("OK");
    } else if (used > 0) {
      Serial.println("ERR command");
    }
    used = 0;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  prefs.begin("gateway");
  store = new mesh::GatewayStore(prefs.getUChar("group", 0), prefs.getUInt("counter", 0));
  if (LittleFS.begin(true)) {
    loadSlots();
  } else {
    Serial.println("ERR flash");
  }
  frontEndInit();
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);
  const int state = radio.begin(params.frequencyMHz, params.bandwidthKHz, params.spreadingFactor, params.codingRate,
                                params.syncWord, RADIO_POWER_DBM, params.preamble, 1.8f);
  radio.setDio2AsRfSwitch(true);
  radio.setCRC(true);
  radio.setRxBoostedGainMode(true);
  radio.setPacketReceivedAction(onReceive);
  startReceive();
  delay(500);
  Serial.printf("HELLO waddle-gateway %s %s\n", VERSION, kct8103l ? "kct8103l" : "gc1109");
  if (state != RADIOLIB_ERR_NONE) Serial.printf("ERR radio start %d\n", state);
}

void loop() {
  if (received) handleReceived();
  readSerial();
  sendOwn();
  delay(1);
}
