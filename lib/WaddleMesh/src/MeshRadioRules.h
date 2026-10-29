#pragma once

// The rules every radio follows: airtime per packet, the hourly airtime budget
// (EU868), packets already seen (for relaying and dropping repeats), and
// counters that are never used twice.

#include <cstddef>
#include <cstdint>

#include "MeshPacket.h"

namespace mesh {

// Radio settings shared by units and the gateway (protocol/PROTOCOL.md, "Airtime").
struct LoraParams {
  float frequencyMHz = 869.525f;
  float bandwidthKHz = 250.0f;
  uint8_t spreadingFactor = 10;
  uint8_t codingRate = 5;  // 4/5
  uint16_t preamble = 16;
  int8_t powerDbm = 14;
  uint8_t syncWord = 0x12;  // private LoRa network
};

// Time on air of one packet in milliseconds (Semtech SX126x datasheet formula).
uint32_t airtimeMs(const LoraParams& p, size_t payloadBytes);

// Airtime spent in the last hour, kept under a budget (6 % of an hour by default,
// inside the 10 % the 869.4-869.65 MHz sub-band allows).
class AirtimeBudget {
 public:
  explicit AirtimeBudget(uint32_t budgetMsPerHour = 216000) : budget_(budgetMsPerHour) {}
  // True when sending a packet of this airtime now stays within the budget.
  bool allows(uint32_t airtime, uint32_t nowMs) const;
  void spend(uint32_t airtime, uint32_t nowMs);
  uint32_t usedLastHour(uint32_t nowMs) const;
  // When to ask again after allows() said no (at the next minute).
  static uint32_t msUntilRetry(uint32_t nowMs) { return MINUTE - nowMs % MINUTE; }

 private:
  // Airtime per minute of the last hour (not per packet: any number of small
  // packets fits as long as their time does).
  static constexpr uint32_t MINUTE = 60000;
  static constexpr int MINUTES = 60;
  uint32_t start_[MINUTES] = {};  // millis() at the start of that minute
  uint32_t ms_[MINUTES] = {};
  uint32_t budget_;
};

// Remembers recent packets by type, sender, counter and piece, so a unit relays
// each packet once and drops copies it hears again.
class SeenPackets {
 public:
  // Returns true the first time, false for a packet already seen. With the
  // body, a forged packet that copies a real one's header does not hide it.
  bool remember(const Header& h, const uint8_t* body = nullptr, size_t bodyLen = 0);
  bool contains(const Header& h, const uint8_t* body = nullptr, size_t bodyLen = 0) const;

 private:
  static constexpr int ENTRIES = 128;
  uint64_t keys_[ENTRIES] = {};
  int next_ = 0;
  static uint64_t key(const Header& h, const uint8_t* body, size_t bodyLen);
};

// Hands out counters that are never repeated, even after a crash: it reserves a
// range and asks to have the end of the range saved before any of it is used.
class CounterReserve {
 public:
  static constexpr uint32_t STEP = 64;
  // savedLimit: the last value written to flash (0 on a new unit).
  explicit CounterReserve(uint32_t savedLimit) : next_(savedLimit + 1), limit_(savedLimit) {}
  // The next counter. When saveLimit is set, write it to flash before sending.
  uint32_t take(uint32_t* saveLimit);

 private:
  uint32_t next_;
  uint32_t limit_;
};

}  // namespace mesh
