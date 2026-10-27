#pragma once
// The clock: time() is fixed by the screen program (sim::nowUnix), local time
// follows the TZ environment variable, as setTimezone() does on the device.
#include <cstdio>
#include <ctime>

#include "SimArduino.h"

struct SimClock {
  bool isAvailable() const { return true; }
  bool setUtc(time_t) { return true; }
  bool localTime(struct tm& out) const {
    const time_t now = time(nullptr);
    return localtime_r(&now, &out) != nullptr;
  }
  bool getTime(uint8_t& hour, uint8_t& minute) const {
    struct tm t {};
    if (!localTime(t)) return false;
    hour = uint8_t(t.tm_hour);
    minute = uint8_t(t.tm_min);
    return true;
  }
  bool formatTime(char* buf, size_t size, bool = false) const {
    uint8_t h = 0, m = 0;
    if (!getTime(h, m)) return false;
    snprintf(buf, size, "%02u:%02u", h, m);
    return true;
  }
};
inline SimClock halClock;
