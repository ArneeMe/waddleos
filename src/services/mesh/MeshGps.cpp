#include "MeshGps.h"

#include <Arduino.h>
#include <BoardT5S3Pins.h>
#include <Logging.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

namespace mesh::gps {

namespace {

HardwareSerial& port = Serial1;
bool isAwake = false;
uint32_t baud = 0;
uint32_t baudTriedAt = 0;
int baudIndex = 0;
constexpr uint32_t BAUDS[] = {9600, 38400};  // L76K, u-blox M10 defaults
const char* modelName = "ukjent";
bool sawSentence = false;

Fix fix;
uint32_t fixAtMs = 0;
char line[100];
size_t used = 0;

// GGA gives altitude and satellites; RMC gives date and position.
int16_t pendingAltitude = 0;
uint8_t pendingSatellites = 0;

void openPort(const uint32_t speed) {
  port.end();
  port.begin(speed, SERIAL_8N1, T5S3_GPS_RXD, T5S3_GPS_TXD);
  baud = speed;
  baudTriedAt = millis();
}

// The i-th comma-separated field (0 = the sentence name), or "".
const char* field(const char* s, int i, char* out, size_t outLen) {
  while (i > 0 && *s) {
    if (*s++ == ',') i--;
  }
  size_t n = 0;
  while (*s && *s != ',' && *s != '*' && n + 1 < outLen) out[n++] = *s++;
  out[n] = '\0';
  return out;
}

bool checksumOk(const char* s) {
  const char* star = strchr(s, '*');
  if (s[0] != '$' || !star || strlen(star) < 3) return false;
  uint8_t sum = 0;
  for (const char* p = s + 1; p < star; p++) sum ^= uint8_t(*p);
  return sum == uint8_t(strtoul(star + 1, nullptr, 16));
}

// "ddmm.mmmm" + hemisphere -> degrees x 10^7.
int32_t coordinate(const char* value, const char* hemisphere) {
  if (!value[0]) return 0;
  const double raw = atof(value);
  const int degrees = int(raw / 100);
  const double minutes = raw - degrees * 100;
  double result = degrees + minutes / 60.0;
  if (hemisphere[0] == 'S' || hemisphere[0] == 'W') result = -result;
  return int32_t(result * 1e7);
}

void handleSentence(const char* s) {
  if (!checksumOk(s)) return;
  if (!sawSentence) {
    sawSentence = true;
    LOG_INF("GPS", "GPS talks at %lu baud", static_cast<unsigned long>(baud));
  }
  char f[24], g[24];
  const char* type = s + 3;  // after "$GP" / "$GN" / "$GB"
  if (strncmp(type, "TXT", 3) == 0) {
    // Start-up text tells the model; log it for the first device test.
    LOG_INF("GPS", "%s", s);
    if (strstr(s, "u-blox") || strstr(s, "PROTVER") || strstr(s, "MOD=")) modelName = "u-blox M10";
    if (strstr(s, "SW=URANUS") || strstr(s, "CASIC")) modelName = "L76K";
    return;
  }
  if (strncmp(type, "GGA", 3) == 0) {
    pendingAltitude = int16_t(atof(field(s, 9, f, sizeof(f))));
    pendingSatellites = uint8_t(atoi(field(s, 7, f, sizeof(f))));
    return;
  }
  if (strncmp(type, "RMC", 3) != 0) return;
  if (field(s, 2, f, sizeof(f))[0] != 'A') return;  // no fix
  char time[12], date[12];
  field(s, 1, time, sizeof(time));
  field(s, 9, date, sizeof(date));
  if (strlen(time) < 6 || strlen(date) != 6) return;
  struct tm t {};
  t.tm_hour = (time[0] - '0') * 10 + (time[1] - '0');
  t.tm_min = (time[2] - '0') * 10 + (time[3] - '0');
  t.tm_sec = (time[4] - '0') * 10 + (time[5] - '0');
  t.tm_mday = (date[0] - '0') * 10 + (date[1] - '0');
  t.tm_mon = (date[2] - '0') * 10 + (date[3] - '0') - 1;
  t.tm_year = 100 + (date[4] - '0') * 10 + (date[5] - '0');
  // Days from civil date, so no timezone setting is involved.
  const int y = t.tm_year + 1900 - (t.tm_mon < 2 ? 1 : 0);
  const int era = y / 400;
  const int yoe = y - era * 400;
  const int mp = (t.tm_mon + 9) % 12;
  const int doy = (153 * mp + 2) / 5 + t.tm_mday - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = long(era) * 146097 + doe - 719468;
  fix.time = uint32_t(days * 86400L + t.tm_hour * 3600L + t.tm_min * 60L + t.tm_sec);
  fix.latE7 = coordinate(field(s, 3, f, sizeof(f)), field(s, 4, g, sizeof(g)));
  fix.lonE7 = coordinate(field(s, 5, f, sizeof(f)), field(s, 6, g, sizeof(g)));
  fix.altitude = pendingAltitude;
  fix.satellites = pendingSatellites;
  fix.valid = true;
  fixAtMs = millis();
}

// UBX-RXM-PMREQ for M10: backup mode until the GPS hears something on its UART.
void sendUbloxSleep() {
  uint8_t msg[] = {0xB5, 0x62, 0x02, 0x41, 16, 0,   //
                   0,    0,    0,    0,               // version, reserved
                   0,    0,    0,    0,               // duration 0 = until woken
                   0x06, 0,    0,    0,               // backup + force
                   0x08, 0,    0,    0,               // wake on UART RX
                   0,    0};                          // checksum
  uint8_t a = 0, b = 0;
  for (size_t i = 2; i < sizeof(msg) - 2; i++) {
    a += msg[i];
    b += a;
  }
  msg[sizeof(msg) - 2] = a;
  msg[sizeof(msg) - 1] = b;
  port.write(msg, sizeof(msg));
  port.flush();
}

}  // namespace

void wake() {
  if (isAwake) return;
  baudIndex = 0;
  openPort(BAUDS[baudIndex]);
  // Any byte on its UART wakes a sleeping u-blox.
  port.write(0xFF);
  isAwake = true;
}

void poll() {
  if (!isAwake) return;
  while (port.available()) {
    const char c = char(port.read());
    if (c == '$') used = 0;
    if (c == '\n' || c == '\r') {
      if (used > 6) {
        line[used] = '\0';
        handleSentence(line);
      }
      used = 0;
    } else if (used < sizeof(line) - 1) {
      line[used++] = c;
    }
  }
  // Nothing readable after 3 s: try the other speed.
  if (!sawSentence && millis() - baudTriedAt > 3000) {
    baudIndex = (baudIndex + 1) % 2;
    openPort(BAUDS[baudIndex]);
  }
}

void sleep() {
  if (sawSentence) {
    sendUbloxSleep();
  } else {
    // Speed not known yet (it was powered up with the radio): say it at both.
    for (const uint32_t speed : BAUDS) {
      openPort(speed);
      sendUbloxSleep();
    }
  }
  isAwake = false;
}

bool awake() { return isAwake; }
const Fix& lastFix() { return fix; }
uint32_t lastFixAtMs() { return fixAtMs; }
const char* model() { return modelName; }

}  // namespace mesh::gps
