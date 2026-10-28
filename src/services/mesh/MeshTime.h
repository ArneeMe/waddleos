#pragma once

#include <cstdint>

namespace mesh {

// Unix seconds for a UTC date and time (month 1-12), with no timezone involved.
// Howard Hinnant's days-from-civil.
inline uint32_t unixFromCivil(int year, int month, int day, int hour, int minute, int second) {
  year -= month <= 2 ? 1 : 0;
  const int era = year / 400;
  const int yoe = year - era * 400;
  const int doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const long days = long(era) * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
  return uint32_t(days * 86400L + hour * 3600L + minute * 60L + second);
}

}  // namespace mesh
