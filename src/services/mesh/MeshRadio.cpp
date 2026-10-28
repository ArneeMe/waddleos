#include "MeshRadio.h"

#include <BoardT5S3.h>
#include <Logging.h>
#include <RadioLib.h>
#include <SPI.h>

namespace mesh::radio {

namespace {

// TCXO voltage and RF switch as in Meshtastic's t5s3_epaper variant.
constexpr float TCXO_VOLTAGE = 2.4f;

SX1262* chip = nullptr;
bool up = false;
bool sleeping = false;
volatile bool packetWaiting = false;

void IRAM_ATTR onPacket() { packetWaiting = true; }

}  // namespace

bool begin(const LoraParams& params) {
  if (up) return true;
  // LoRa and GPS share one power switch on the IO expander.
  BoardT5S3::writePca9535Pin(PCA9535_IO00_LORA_GPS_EN, true);
  BoardT5S3::setPca9535PinMode(PCA9535_IO00_LORA_GPS_EN, OUTPUT);
  delay(10);
  if (!chip) chip = new SX1262(new Module(T5S3_LORA_CS, T5S3_LORA_IRQ, T5S3_LORA_RST, T5S3_LORA_BUSY, SPI));
  const int state = chip->begin(params.frequencyMHz, params.bandwidthKHz, params.spreadingFactor, params.codingRate,
                                params.syncWord, params.powerDbm, params.preamble, TCXO_VOLTAGE);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_ERR("RADIO", "SX1262 start failed: %d", state);
    return false;
  }
  chip->setDio2AsRfSwitch(true);
  chip->setCRC(true);
  chip->setPacketReceivedAction(onPacket);
  chip->startReceive();
  up = true;
  LOG_INF("RADIO", "Listening on %.3f MHz, SF%u, BW%.0f", params.frequencyMHz, params.spreadingFactor,
          params.bandwidthKHz);
  return true;
}

bool isUp() { return up; }

bool receive(Received& out) {
  // The pin is checked too: a packet that woke the unit from light sleep may
  // have arrived while the interrupt was not armed.
  if (!up || sleeping || !(packetWaiting || digitalRead(T5S3_LORA_IRQ) == HIGH)) return false;
  packetWaiting = false;
  out.len = chip->getPacketLength();
  if (out.len > sizeof(out.data)) out.len = sizeof(out.data);
  const int state = chip->readData(out.data, out.len);
  out.rssi = int(chip->getRSSI());
  out.snr = chip->getSNR();
  chip->startReceive();
  return state == RADIOLIB_ERR_NONE;
}

bool send(const uint8_t* data, const size_t len) {
  if (!up) return false;
  resume();
  const int state = chip->transmit(data, len);
  packetWaiting = false;
  chip->startReceive();
  if (state != RADIOLIB_ERR_NONE) LOG_ERR("RADIO", "Send failed: %d", state);
  return state == RADIOLIB_ERR_NONE;
}

int irqPin() { return T5S3_LORA_IRQ; }

void rearm() {
  if (up) chip->setPacketReceivedAction(onPacket);
}

void pause() {
  if (!up || sleeping) return;
  chip->sleep(true);
  sleeping = true;
}

void resume() {
  if (!up || !sleeping) return;
  chip->standby();
  chip->startReceive();
  sleeping = false;
}

bool paused() { return sleeping; }

void end() {
  if (!up) return;
  chip->sleep();
  sleeping = false;
  BoardT5S3::writePca9535Pin(PCA9535_IO00_LORA_GPS_EN, false);
  up = false;
}

}  // namespace mesh::radio
