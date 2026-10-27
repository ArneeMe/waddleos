#pragma once
// The bits of Arduino the screens use, on a computer.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "SimArduino.h"
#include "driver/gpio.h"

#ifndef PROGMEM
#define PROGMEM
#endif
#define LOW 0
#define HIGH 1
#define INPUT 0
#define OUTPUT 1
inline int digitalRead(int) { return HIGH; }
inline void digitalWrite(int, int) {}
inline void pinMode(int, int) {}
inline unsigned long micros() { return sim::nowMs * 1000UL; }

struct SimSerial {
  template <class... A>
  void printf(A&&...) {}
};
inline SimSerial Serial;
