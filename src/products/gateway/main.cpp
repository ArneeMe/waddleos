// WaddleOS mesh gateway: a Heltec WiFi LoRa 32 V4 on the Pi's USB port.
//
// It listens on the mesh frequency and passes every WaddleOS packet it hears
// to the Pi, and sends what the Pi asks it to (protocol/PROTOCOL.md,
// "Gateway serial link"). It keeps no state and relays nothing by itself.
//
//   Gateway -> Pi:  RX <rssi> <snr> <hex>     HELLO waddle-gateway <version>
//   Pi -> gateway:  TX <hex>                  answered with OK or ERR <reason>

#include <Arduino.h>
#include <MeshPacket.h>
#include <MeshRadioRules.h>
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
constexpr const char* VERSION = "1";

SX1262 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RESET, PIN_LORA_BUSY);
mesh::LoraParams params;
mesh::AirtimeBudget budget;
bool kct8103l = false;
volatile bool received = false;

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

void handleReceived() {
  received = false;
  uint8_t data[255];
  const size_t len = radio.getPacketLength();
  const int state = radio.readData(data, len);
  if (state == RADIOLIB_ERR_NONE) {
    mesh::Header h;
    // Only WaddleOS packets go to the Pi; other LoRa traffic on the frequency is ignored.
    if (mesh::readHeader(data, len, h)) {
      Serial.printf("RX %d %.1f ", int(radio.getRSSI()), radio.getSNR());
      printHex(data, len);
      Serial.write('\n');
      digitalWrite(PIN_LED, HIGH);
    }
  }
  startReceive();
  digitalWrite(PIN_LED, LOW);
}

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
  const uint32_t airtime = mesh::airtimeMs(params, len);
  if (!budget.allows(airtime, millis())) {
    Serial.println("ERR airtime");
    return;
  }
  budget.spend(airtime, millis());
  frontEndTransmit(true);
  const int state = radio.transmit(data, len);
  startReceive();
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("OK");
  } else {
    Serial.printf("ERR radio %d\n", state);
  }
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
    if (strncmp(line, "TX ", 3) == 0) {
      transmit(line + 3);
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
  delay(1);
}
