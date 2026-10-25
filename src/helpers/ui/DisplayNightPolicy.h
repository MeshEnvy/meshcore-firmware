#pragma once

#include <stdint.h>

namespace mesh { namespace ui { namespace night_policy {

// A seven-entry quarter-wave table avoids floating-point trig or an astronomy
// library. Linear interpolation is sufficient for a display's dusk schedule.
inline int32_t sine(int degrees10) {
  static const uint16_t values[] = {0, 259, 500, 707, 866, 966, 1000};
  degrees10 %= 3600;
  if (degrees10 < 0) degrees10 += 3600;
  const bool negative = degrees10 > 1800;
  if (negative) degrees10 -= 1800;
  if (degrees10 > 900) degrees10 = 1800 - degrees10;
  const unsigned index = degrees10 / 150;
  int32_t value = values[index];
  if (index < 6) value += ((values[index + 1] - value) * (degrees10 % 150) + 75) / 150;
  return negative ? -value : value;
}

inline bool validLocation(int32_t latitude_e6, int32_t longitude_e6) {
  return latitude_e6 >= -90000000 && latitude_e6 <= 90000000
      && longitude_e6 >= -180000000 && longitude_e6 <= 180000000;
}

inline bool active(uint32_t utc, int32_t latitude_e6, int32_t longitude_e6,
                   int16_t delay_minutes) {
  if (utc < 1577836800UL || !validLocation(latitude_e6, longitude_e6)) return false;
  // Longitude supplies local solar time, so neither a time zone nor DST is
  // needed. Omitting the equation of time costs at most roughly 16 minutes.
  int solar_minute = int(utc % 86400UL / 60) + longitude_e6 / 250000;
  uint32_t day = utc / 86400UL - 10957UL;  // days since 2000-01-01
  if (solar_minute < 0) { solar_minute += 1440; --day; }
  else if (solar_minute >= 1440) { solar_minute -= 1440; ++day; }
  unsigned year = 2000;
  for (;;) {
    const unsigned length = 365 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    if (day < length) break;
    day -= length;
    ++year;
  }
  const int declination = (234 * sine((int(day) - 80) * 3600 / 365)) / 1000;
  const int latitude = latitude_e6 / 100000;
  const int32_t numerator = -15000 - sine(latitude) * sine(declination);
  const int32_t denominator = sine(latitude + 900) * sine(declination + 900);
  // Include the conventional approximately -0.83 degree sunrise altitude.
  // Polar day/night has no crossing and must not divide by a vanishing cosine.
  if (numerator <= -denominator) return false;
  if (numerator >= denominator) return true;
  int low = 0, high = 1800;
  while (low < high) {
    const int middle = (low + high) / 2;
    if (sine(middle + 900) * denominator / 1000 > numerator) low = middle + 1;
    else high = middle;
  }
  const int half_day = (low * 4 + 5) / 10;
  const int dawn = 720 - half_day;
  const int dusk = 720 + half_day;
  // Work in the solar day beginning at sunrise. This handles a positive
  // sunset delay crossing midnight without turning night mode on too early.
  int since_dawn = solar_minute - dawn;
  if (since_dawn < 0) since_dawn += 1440;
  const int start = dusk - dawn + delay_minutes;
  if (start <= 0) return true;
  return start < 1440 && since_dawn >= start;
}

} } }
