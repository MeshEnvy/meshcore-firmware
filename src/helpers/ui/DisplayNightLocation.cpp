#include "DisplayNightLocation.h"

#if defined(MESHCORE_HAS_COLOR_DISPLAY)
#include <Mesh.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/SensorManager.h>
#include "DisplayNightMode.h"

namespace mesh {
namespace ui {
namespace {
DisplayNightLocationCache locations;
uint32_t last_refresh_ms = 0;
bool refreshed = false;
}

void observeDisplayNightAdvert(const mesh::Packet* packet,
                               const mesh::Identity& identity,
                               const mesh::Identity& self,
                               const uint8_t* app_data, size_t app_data_len,
                               uint32_t now_ms) {
  if (packet == nullptr || packet->radio_local || packet->forwarded
      || app_data_len > MAX_ADVERT_DATA_SIZE
      || identity.matches(self)
      || packet->getPathHashCount() != 0
      || (packet->hasTransportCodes() && packet->transport_codes[0] == 0
          && packet->transport_codes[1] == 0)) return;
  // Each role invokes this from its verified-advert callback, before contact
  // admission. Shared or relayed contact data cannot refresh local RF age.
  AdvertDataParser advert(app_data, app_data_len);
  if (!advert.isValid() || !advert.hasLatLon()) return;
  uint32_t prefix;
  memcpy(&prefix, identity.pub_key, sizeof(prefix));
  locations.observe(prefix, advert.getIntLat(), advert.getIntLon(), now_ms);
}

void refreshDisplayNightLocation(const SensorManager& sensors,
                                uint32_t utc, uint32_t now_ms) {
  if (refreshed && static_cast<uint32_t>(now_ms - last_refresh_ms) < 15000UL) return;
  refreshed = true;
  last_refresh_ms = now_ms;
  int32_t latitude_e6, longitude_e6;
  const bool has_location = readDisplayNightLocation(
      locations, sensors, now_ms, latitude_e6, longitude_e6);
  updateDisplayNightMode(utc, has_location, latitude_e6, longitude_e6);
}

} // namespace ui
} // namespace mesh
#endif
