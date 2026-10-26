#pragma once

#include "BeaconFeatureConfig.h"
#include "EthernetTunnelConfig.h"
#include "HostKissConfig.h"
#include "ota/OtaDirectConfig.h"

// Full Companion includes channel chat on both supported platforms. Keep this
// independent of board/chip macros, which some Arduino variants reveal only
// after shared sources include this header. Every translation unit must select
// the same layouts. The concrete driver still gates modem/profile support;
// release builds must also pass their unchanged flash and runtime RAM budgets.
#ifndef MESH_ENABLE_MESHTEXT_CHAT
  #if (defined(ESP32_PLATFORM) || defined(NRF52_PLATFORM)) \
      && defined(COMPANION_RADIO_FULL) && COMPANION_RADIO_FULL \
      && !(defined(MESH_PRIMARY_ESPNOW) && MESH_PRIMARY_ESPNOW)
    #define MESH_ENABLE_MESHTEXT_CHAT 1
  #else
    #define MESH_ENABLE_MESHTEXT_CHAT 0
  #endif
#endif

// Compile the bounded wire codec uniformly for beacon-enabled builds. Some
// variants discover their radio chip only when Arduino includes the variant
// header; using USE_SX1262 here would select inconsistent class layouts and
// omit codec definitions in translation units that do not include Arduino.
// Concrete driver capability still gates Meshtext TX independently. This
// does not enable Companion chat, RX handling, or automatic radio2 management.
#ifndef MESH_ENABLE_MESHTEXT_WIRE
  #if MESH_ENABLE_MESHTEXT_CHAT || MESH_ENABLE_TX_BEACONS
    #define MESH_ENABLE_MESHTEXT_WIRE 1
  #else
    #define MESH_ENABLE_MESHTEXT_WIRE 0
  #endif
#endif

// Foreign transports and negotiated direct KISS OTA share one bounded raw
// dispatcher slot. Direct OTA retains its temporary MeshCore profile; foreign
// transports keep their existing radio2-only ownership.
#ifndef MESH_ENABLE_FOREIGN_RADIO
  #define MESH_ENABLE_FOREIGN_RADIO (MESH_ENABLE_MESHTEXT_CHAT || MESH_ENABLE_ETHERNET_TUNNEL || MESH_ENABLE_OTA_KISS_DIRECT || MESH_ENABLE_HOST_KISS)
#endif
