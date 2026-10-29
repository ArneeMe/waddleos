#include "MeshRadioRules.h"

#include <cmath>

namespace mesh {

uint32_t airtimeMs(const LoraParams& p, const size_t payloadBytes) {
  const double symbolMs = std::ldexp(1.0, p.spreadingFactor) / p.bandwidthKHz;
  const int lowRateOptimize = symbolMs > 16.0 ? 1 : 0;
  const double preambleMs = (p.preamble + 4.25) * symbolMs;
  const int sf = p.spreadingFactor;
  // Explicit header, CRC on.
  const double numerator = 8.0 * double(payloadBytes) - 4.0 * sf + 28 + 16;
  const double denominator = 4.0 * (sf - 2 * lowRateOptimize);
  double symbols = std::ceil(numerator / denominator) * p.codingRate;
  if (symbols < 0) symbols = 0;
  return uint32_t(std::ceil(preambleMs + (8 + symbols) * symbolMs));
}

uint32_t AirtimeBudget::usedLastHour(const uint32_t nowMs) const {
  uint32_t used = 0;
  for (int i = 0; i < ENTRIES; i++) {
    if (ms_[i] && nowMs - at_[i] < 3600000UL) used += ms_[i];
  }
  return used;
}

bool AirtimeBudget::allows(const uint32_t airtime, const uint32_t nowMs) const {
  // The ring forgets the oldest send once full; refuse rather than undercount.
  const int slot = next_;
  if (ms_[slot] && nowMs - at_[slot] < 3600000UL) return false;
  return usedLastHour(nowMs) + airtime <= budget_;
}

void AirtimeBudget::spend(const uint32_t airtime, const uint32_t nowMs) {
  at_[next_] = nowMs;
  ms_[next_] = airtime ? airtime : 1;
  next_ = (next_ + 1) % ENTRIES;
}

uint64_t SeenPackets::key(const Header& h, const uint8_t* body, const size_t bodyLen) {
  // 11 bits of the body (FNV-1a), so a packet with a copied header and other
  // contents counts as a different one.
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < bodyLen; i++) hash = (hash ^ body[i]) * 16777619u;
  // Never 0, so an empty entry never matches.
  return (uint64_t(1) << 63) | (uint64_t(hash & 0x7FF) << 52) | (uint64_t(h.type) << 48) | (uint64_t(h.sender) << 40) |
         (uint64_t(h.pieceIndex) << 32) | h.counter;
}

bool SeenPackets::contains(const Header& h, const uint8_t* body, const size_t bodyLen) const {
  const uint64_t k = key(h, body, bodyLen);
  for (const uint64_t e : keys_) {
    if (e == k) return true;
  }
  return false;
}

bool SeenPackets::remember(const Header& h, const uint8_t* body, const size_t bodyLen) {
  if (contains(h, body, bodyLen)) return false;
  keys_[next_] = key(h, body, bodyLen);
  next_ = (next_ + 1) % ENTRIES;
  return true;
}

uint32_t CounterReserve::take(uint32_t* saveLimit) {
  *saveLimit = 0;
  if (next_ > limit_) {
    limit_ = next_ + STEP - 1;
    *saveLimit = limit_;
  }
  return next_++;
}

}  // namespace mesh
