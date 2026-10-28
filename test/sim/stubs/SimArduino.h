#pragma once

// The bits of Arduino the mesh code uses, for running it on a computer.

#include <cstdint>
#include <cstring>
#include <string>

namespace sim {
extern uint32_t nowMs;       // the simulated millis(), moved by the test driver
extern std::string rootDir;  // where this unit's "SD card" and "flash" live
}  // namespace sim

inline uint32_t millis() { return sim::nowMs; }
inline void delay(uint32_t ms) { sim::nowMs += ms; }

class String {
 public:
  String() = default;
  String(const char* s) : s_(s ? s : "") {}
  String(const char* s, size_t n) : s_(s, n) {}
  String(const std::string& s) : s_(s) {}
  const char* c_str() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }

 private:
  std::string s_;
};

struct SimEsp {
  uint32_t getFreeHeap() const { return 200000; }
};
inline SimEsp ESP;
