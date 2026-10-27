#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Connection admission uses DMA-capable internal heap rather than total heap:
// PSRAM-backed TLS still needs internal AES bounce buffers and WiFi allocations.
// These are headroom bounds, not a guarantee that a later allocation succeeds.
namespace MQTTConnectionAdmission {

static const size_t kDmaReserveBytes = 16384;
static const size_t kDmaLargestBytes = 4096;
// Keep the packet-publishing guard and connection admission on the same floor.
// A new optional broker must not consume the contiguous block that existing
// brokers need to continue forwarding packets.
// SDK QoS0 writes are fragmented into the pinned 896-byte wire buffer.
// PSRAM-backed JSON and TLS need DMA scratch, not another 16 KiB JSON block.
static const size_t kPsramPublishLargestBytes = 4096;
static_assert(kPsramPublishLargestBytes == kDmaLargestBytes,
              "Connection and publish DMA scratch floors must agree");
// Incoming TLS handshake/records are not bounded by our outgoing MQTT buffer.
// Preserve the existing TLS connection floor independently of publication.
static const size_t kPsramTlsLargestBytes = 16000;
static const size_t kInternalPublishLargestBytes = 8000;
// A stopped SDK client needs a new internal task stack/TCB. First starts also
// allocate receive/transmit/reassembly buffers and SDK transport/config objects
// after admission. Budget that ordinary allocation separately from AES headroom.
// Pin the actual SDK stack/priority in the optimizer rather than relying on an
// SDK default or confusing this per-client task with the MQTTBridge worker.
static const size_t kClientTaskStackBytes = 6144;
static const int kClientTaskPriority = 5;
static const int kClientBufferBytes = 896;
static const size_t kColdStartAllowanceBytes = 16384;
static const size_t kColdStartLargestBytes = 8192;
// SDK stop retains buffers/configuration but releases its task. Restart only
// needs the pinned 6 KiB stack plus TCB/allocator headroom, not another SDK init.
static const size_t kRestartAllowanceBytes = 8192;
static const size_t kRestartLargestBytes = 8192;
static const size_t kTlsInternalFreeBytes = 61440;
static const size_t kTlsRecordAllocationBytes = 17408;

enum class ClientMemoryState : uint8_t {
  Uninitialized,
  Stopped,
  Started,
};

static inline size_t publishLargestBytes(bool psram) {
  return psram ? kPsramPublishLargestBytes : kInternalPublishLargestBytes;
}

static inline bool hasPublishHeadroom(bool psram, size_t largest) {
  return largest >= publishLargestBytes(psram);
}

static inline bool canStart(bool psram, bool tls, ClientMemoryState state,
                            size_t dma_free, size_t dma_largest,
                            bool protect_publish = false) {
  size_t allocation_allowance;
  size_t sdk_largest;
  switch (state) {
    case ClientMemoryState::Uninitialized:
      allocation_allowance = kColdStartAllowanceBytes;
      sdk_largest = kColdStartLargestBytes;
      break;
    case ClientMemoryState::Stopped:
      allocation_allowance = kRestartAllowanceBytes;
      sdk_largest = kRestartLargestBytes;
      break;
    case ClientMemoryState::Started:
      allocation_allowance = 0;
      sdk_largest = kDmaLargestBytes;
      break;
    default:
      return false;
  }
  const size_t publication_floor = publishLargestBytes(psram);
  const size_t tls_record = tls ? (psram ? kPsramTlsLargestBytes : kTlsRecordAllocationBytes) : 0;
  // Bootstrap/recover one broker without requiring spare capacity for another
  // live connection: retained SDK buffers can fragment the heap after an outage.
  // With a live peer, conservatively allow startup/TLS allocations to consume
  // the largest block while leaving existing forwarding's publication floor.
  // Other tasks and TLS allocations can still change/fragment this snapshot;
  // these are admission bounds, not an allocation guarantee.
  size_t largest_required = publication_floor > sdk_largest ? publication_floor : sdk_largest;
  if (tls_record > largest_required) largest_required = tls_record;
  if (protect_publish) {
    largest_required = psram
        ? (publication_floor > tls_record ? publication_floor : tls_record) + allocation_allowance
        : publication_floor + allocation_allowance + tls_record;
  }
  const size_t free_required = kDmaReserveBytes + allocation_allowance;
  if (dma_free < free_required || dma_largest < largest_required) {
    return false;
  }
  return !tls || psram ||
         (dma_free >= kTlsInternalFreeBytes &&
          dma_largest >= kTlsRecordAllocationBytes);
}

// Charge a PSRAM allocation's internal fallback against the DMA reserve even
// when the internal allocator might find a block with fewer capabilities.
static inline bool canFallback(size_t requested, size_t dma_free,
                               size_t dma_largest) {
  return requested > 0 && requested <= dma_largest &&
         dma_free >= kDmaReserveBytes &&
         requested <= dma_free - kDmaReserveBytes;
}

static inline bool checkedCallocSize(size_t count, size_t size, size_t& total) {
  total = 0;
  if (count == 0 || size == 0 || count > SIZE_MAX / size) return false;
  total = count * size;
  return true;
}

// An explicit scheme controls the transport even on an unusual port. Bare
// custom hosts match setupSlot's port-based URI construction.
static inline bool requiresTls(const char* uri, uint16_t port) {
  if (uri != nullptr) {
    if (strncmp(uri, "mqtts://", 8) == 0 || strncmp(uri, "wss://", 6) == 0) {
      return true;
    }
    if (strncmp(uri, "mqtt://", 7) == 0 || strncmp(uri, "ws://", 5) == 0) {
      return false;
    }
  }
  return port == 443 || port == 8883;
}

} // namespace MQTTConnectionAdmission
