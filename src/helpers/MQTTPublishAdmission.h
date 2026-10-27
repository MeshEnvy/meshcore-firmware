#pragma once

#include "MQTTConnectionAdmission.h"
#include <stddef.h>
#include <stdint.h>

namespace MQTTPublishAdmission {

// QoS0 writes use the SDK's fixed buffer; QoS1 can resend an entire outbox
// record. Keep the existing DMA reserve and account for larger queued records.
static const size_t kLegacyLargestBytes = 8000;
static const size_t kFramingAllowanceBytes = 128;
static const uint32_t kSampleIntervalMs = 5000;

struct Heap {
  bool psram = false;
  size_t internal_largest = 0;
  size_t dma_free = 0;
  size_t dma_largest = 0;
};

inline bool canPublish(const Heap& heap, size_t payload_size = 0,
                       size_t topic_size = 0, uint8_t qos = 0) {
  if (payload_size > SIZE_MAX - kFramingAllowanceBytes ||
      topic_size > SIZE_MAX - kFramingAllowanceBytes - payload_size) return false;
  size_t record = qos ? payload_size + topic_size + kFramingAllowanceBytes : 0;
  if (record > MQTTConnectionAdmission::kTlsRecordAllocationBytes)
    record = MQTTConnectionAdmission::kTlsRecordAllocationBytes;
  if (!heap.psram) {
    const size_t required = record > kLegacyLargestBytes ? record : kLegacyLargestBytes;
    return heap.internal_largest >= required;
  }
  const size_t largest = record > MQTTConnectionAdmission::kDmaLargestBytes
      ? record : MQTTConnectionAdmission::kDmaLargestBytes;
  const size_t free = MQTTConnectionAdmission::kDmaReserveBytes
      + largest - MQTTConnectionAdmission::kDmaLargestBytes;
  return heap.dma_free >= free && heap.dma_largest >= largest;
}

struct State {
  Heap heap;
  uint32_t sampled_at = 0;
  bool sampled = false;
  bool needsSample(uint32_t now) const {
    return !sampled || now - sampled_at >= kSampleIntervalMs;
  }
  void update(uint32_t now, const Heap& snapshot) {
    heap = snapshot;
    sampled_at = now;
    sampled = true;
  }
  void invalidate() { sampled = false; }
};

}  // namespace MQTTPublishAdmission
