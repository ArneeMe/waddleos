#pragma once

// NVS as one file per key under the unit's directory.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "SimArduino.h"

class Preferences {
 public:
  bool begin(const char* ns, bool) {
    dir_ = sim::rootDir + "/nvs/" + ns;
    std::filesystem::create_directories(dir_);
    return true;
  }
  void end() {}
  size_t getBytes(const char* key, void* buf, size_t len) {
    std::ifstream f(dir_ + "/" + key, std::ios::binary);
    if (!f) return 0;
    f.read(static_cast<char*>(buf), std::streamsize(len));
    return size_t(f.gcount());
  }
  size_t putBytes(const char* key, const void* buf, size_t len) {
    std::ofstream f(dir_ + "/" + key, std::ios::binary);
    f.write(static_cast<const char*>(buf), std::streamsize(len));
    return len;
  }
  uint32_t getUInt(const char* key, uint32_t def) {
    uint32_t v = def;
    return getBytes(key, &v, sizeof(v)) == sizeof(v) ? v : def;
  }
  size_t putUInt(const char* key, uint32_t v) { return putBytes(key, &v, sizeof(v)); }

 private:
  std::string dir_;
};
