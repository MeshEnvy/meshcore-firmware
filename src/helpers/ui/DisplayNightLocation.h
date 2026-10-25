#pragma once

#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include "DisplayBuildFlags.h"

#if defined(MESHCORE_HAS_COLOR_DISPLAY)

class SensorManager;
namespace mesh {
class Packet;
class Identity;
namespace ui {

// Solar timing needs only an approximate position. Keep one observation per
// identity prefix, at roughly 1 km resolution, rather than copying contacts.
class DisplayNightLocationCache {
  struct Entry {
    uint32_t identity;
    uint32_t heard_at;
    int16_t latitude_e2;
    int16_t longitude_e2;
    bool empty() const { return latitude_e2 == 0 && longitude_e2 == 0; }
    void clear() { latitude_e2 = longitude_e2 = 0; }
  };
  static_assert(sizeof(Entry) == 12, "Night location observations must stay compact");
  Entry entries[16] = {};

  static int16_t quantize(int32_t value) {
    return (value + (value < 0 ? -5000 : 5000)) / 10000;
  }
  static int32_t meanE6(int32_t sum, uint8_t count) {
    // Split the multiplication to keep the whole average in 32-bit math.
    return (sum / count) * 10000 + ((sum % count) * 10000) / count;
  }

public:
  static constexpr uint32_t MAX_AGE_MS = 12UL * 60UL * 60UL * 1000UL;
  static bool valid(int32_t latitude_e6, int32_t longitude_e6) {
    return latitude_e6 >= -90000000L && latitude_e6 <= 90000000L
        && longitude_e6 >= -180000000L && longitude_e6 <= 180000000L
        && (latitude_e6 != 0 || longitude_e6 != 0);
  }

  bool observe(uint32_t identity, int32_t latitude_e6, int32_t longitude_e6,
               uint32_t now_ms) {
    if (!valid(latitude_e6, longitude_e6)) return false;
    const int16_t latitude_e2 = quantize(latitude_e6);
    const int16_t longitude_e2 = quantize(longitude_e6);
    if (latitude_e2 == 0 && longitude_e2 == 0) return false;
    Entry* selected = &entries[0];
    uint32_t oldest_age = 0;
    for (auto& entry : entries) {
      if (!entry.empty() && entry.identity == identity) {
        selected = &entry;
        break;
      }
      const uint32_t age = now_ms - entry.heard_at;
      if (entry.empty() || age > oldest_age) {
        selected = &entry;
        oldest_age = entry.empty() ? UINT32_MAX : age;
      }
    }
    selected->identity = identity;
    selected->heard_at = now_ms;
    selected->latitude_e2 = latitude_e2;
    selected->longitude_e2 = longitude_e2;
    return true;
  }

  bool average(uint32_t now_ms, int32_t& latitude_e6, int32_t& longitude_e6) {
    int32_t latitude_sum = 0, longitude_sum = 0, anchor = 0;
    uint8_t count = 0;
    for (auto& entry : entries) {
      if (entry.empty()) continue;
      if (static_cast<uint32_t>(now_ms - entry.heard_at) > MAX_AGE_MS) {
        entry.clear();
        continue;
      }
      if (count == 0) anchor = entry.longitude_e2;
      int32_t longitude = entry.longitude_e2;
      if (longitude - anchor > 18000) longitude -= 36000;
      else if (longitude - anchor < -18000) longitude += 36000;
      latitude_sum += entry.latitude_e2;
      longitude_sum += longitude;
      ++count;
    }
    latitude_e6 = longitude_e6 = 0;
    if (count == 0) return false;
    latitude_e6 = meanE6(latitude_sum, count);
    longitude_e6 = meanE6(longitude_sum, count);
    if (longitude_e6 > 180000000L) longitude_e6 -= 360000000L;
    else if (longitude_e6 <= -180000000L) longitude_e6 += 360000000L;
    return true;
  }
};

template<class Sensors>
bool readDisplayNightLocation(DisplayNightLocationCache& cache,
                              const Sensors& sensors, uint32_t now_ms,
                              int32_t& latitude_e6, int32_t& longitude_e6) {
  // Retire expired neighbors even while GPS supplies the preferred position.
  const bool has_neighbors = cache.average(now_ms, latitude_e6, longitude_e6);
  double latitude = 0, longitude = 0;
  if (sensors.getCachedGpsPosition(latitude, longitude)
      && isfinite(latitude) && isfinite(longitude)
      && latitude >= -90.0 && latitude <= 90.0
      && longitude >= -180.0 && longitude <= 180.0) {
    const int32_t gps_latitude = static_cast<int32_t>(latitude * 1000000.0
        + (latitude < 0 ? -0.5 : 0.5));
    const int32_t gps_longitude = static_cast<int32_t>(longitude * 1000000.0
        + (longitude < 0 ? -0.5 : 0.5));
    if (DisplayNightLocationCache::valid(gps_latitude, gps_longitude)) {
      latitude_e6 = gps_latitude;
      longitude_e6 = gps_longitude;
      return true;
    }
  }
  return has_neighbors;
}

void observeDisplayNightAdvert(const mesh::Packet* packet,
                               const mesh::Identity& identity,
                               const mesh::Identity& self,
                               const uint8_t* app_data, size_t app_data_len,
                               uint32_t now_ms);
void refreshDisplayNightLocation(const SensorManager& sensors,
                                uint32_t utc, uint32_t now_ms);

} // namespace ui
} // namespace mesh
#endif
