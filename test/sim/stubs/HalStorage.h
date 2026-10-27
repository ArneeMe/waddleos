#pragma once

// The SD card as a directory on the computer (sim::rootDir).

#include <fcntl.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "SimArduino.h"

class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(FILE* f) : f_(f) {}
  HalFile(HalFile&& o) noexcept : f_(o.f_) { o.f_ = nullptr; }
  HalFile& operator=(HalFile&& o) noexcept {
    close();
    f_ = o.f_;
    o.f_ = nullptr;
    return *this;
  }
  ~HalFile() { close(); }
  explicit operator bool() const { return f_ != nullptr; }
  int read(void* buf, size_t n) { return f_ ? int(std::fread(buf, 1, n, f_)) : -1; }
  int read() {
    if (!f_) return -1;
    const int c = std::fgetc(f_);
    return c == EOF ? -1 : c;
  }
  size_t write(const void* buf, size_t n) { return f_ ? std::fwrite(buf, 1, n, f_) : 0; }
  bool close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
    return true;
  }

 private:
  FILE* f_ = nullptr;
};

class HalStorage {
 public:
  static std::string full(const char* path) { return sim::rootDir + path; }
  bool exists(const char* path) { return std::filesystem::exists(full(path)); }
  bool mkdir(const char* path, bool = true) { return std::filesystem::create_directories(full(path)) || exists(path); }
  bool remove(const char* path) { return std::filesystem::remove(full(path)); }
  String readFile(const char* path) {
    std::ifstream f(full(path), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return String(ss.str());
  }
  bool writeFile(const char* path, const String& content) {
    std::ofstream f(full(path), std::ios::binary | std::ios::trunc);
    f.write(content.c_str(), std::streamsize(content.length()));
    return bool(f);
  }
  HalFile open(const char* path, int oflag = O_RDONLY) {
    const char* mode = "rb";
    if (oflag & O_APPEND) {
      mode = "ab";
    } else if (oflag & O_TRUNC) {
      mode = "wb";
    } else if (oflag & (O_WRONLY | O_RDWR)) {
      mode = "r+b";
    }
    return HalFile(std::fopen(full(path).c_str(), mode));
  }
};

inline HalStorage Storage;
