#pragma once

// The T5 S3 Pro's SX1262 (868 MHz). It shares the SPI bus with the SD card and
// its power switch (PCA9535 IO0) with the GPS.

#include <MeshPacket.h>
#include <MeshRadioRules.h>

#include <cstddef>
#include <cstdint>

namespace mesh {

struct Received {
  uint8_t data[255];
  size_t len = 0;
  int rssi = 0;
  float snr = 0;
};

namespace radio {

// Powers the radio (and the GPS with it) and starts listening. False when the
// chip does not answer.
bool begin(const LoraParams& params);
bool isUp();
// Takes one received packet, if one has arrived.
bool receive(Received& out);
// Sends one packet and goes back to listening. Blocks for its airtime.
bool send(const uint8_t* data, size_t len);
// The radio's interrupt pin, high when a packet has arrived (a light sleep wake source).
int irqPin();
// Sets the packet interrupt again (light sleep's wake setup changes the pin).
void rearm();
// Stops listening, keeping its settings (about 1 µA), and starts again. The
// GPS on the same power switch is not touched.
void pause();
void resume();
bool paused();
// Powers the radio and the GPS down.
void end();

}  // namespace radio
}  // namespace mesh
