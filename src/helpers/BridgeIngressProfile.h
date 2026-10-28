#pragma once

#include <stdint.h>

// WITH_BRIDGE is also synthesized by CommonCLI.h. This feature must not
// depend on that header having been included first in a translation unit.
#ifndef MESH_ENABLE_BRIDGE_INGRESS
#if defined(WITH_BRIDGE) || defined(WITH_ESPNOW_BRIDGE) || \
    defined(WITH_RS232_BRIDGE) || defined(WITH_MQTT_BRIDGE) || defined(WITH_UDP_BRIDGE)
#define MESH_ENABLE_BRIDGE_INGRESS 1
#else
#define MESH_ENABLE_BRIDGE_INGRESS 0
#endif
#endif

namespace mesh { namespace bridge {

enum class IngressProfile : uint8_t { Radio = 0, Radio2 = 1, Blocked = 2 };

#if MESH_ENABLE_BRIDGE_INGRESS
IngressProfile ingressProfile();
#else
inline IngressProfile ingressProfile() { return IngressProfile::Radio; }
#endif

}} // namespace mesh::bridge
