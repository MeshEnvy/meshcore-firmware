#pragma once

#include <stddef.h>
#include <stdint.h>
#include "HostKissConfig.h"

#if MESH_ENABLE_HOST_KISS
namespace mesh {
namespace hostkiss {
// Called by the Dispatcher only after radio2 protocol/generation validation.
// This is the sole RX consumer, before MeshCore logs, parsing and bridges.
void receive(const uint8_t* bytes, size_t length, uint8_t profile,
             uint32_t generation, float snr, float rssi);
void sendComplete(bool success);
}
}
#endif
