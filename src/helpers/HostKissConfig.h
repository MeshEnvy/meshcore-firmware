#pragma once

// A host-assisted raw radio2 modem in Full firmware. Keep this independent of
// chip/Arduino headers: all translation units must choose identical layouts.
// Full infrastructure builds opt in explicitly; reduced builds stay unchanged.
#ifndef MESH_ENABLE_HOST_KISS
  #if (defined(ESP32_PLATFORM) || defined(NRF52_PLATFORM)) \
      && defined(COMPANION_RADIO_FULL) && COMPANION_RADIO_FULL \
      && !(defined(MESH_PRIMARY_ESPNOW) && MESH_PRIMARY_ESPNOW)
    #define MESH_ENABLE_HOST_KISS 1
  #else
    #define MESH_ENABLE_HOST_KISS 0
  #endif
#endif

// ESP-NOW-only images have no independent LoRa modem for this transport.
#if defined(MESH_PRIMARY_ESPNOW) && MESH_PRIMARY_ESPNOW
  #undef MESH_ENABLE_HOST_KISS
  #define MESH_ENABLE_HOST_KISS 0
#endif
