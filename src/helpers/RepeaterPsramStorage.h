#pragma once

#ifndef MESH_REPEATER_PSRAM_TABLES
  #if defined(STATION_G3_ESP32) && defined(ESP32_PLATFORM) && defined(BOARD_HAS_PSRAM) \
      && defined(MESH_CLIENT_REPEATER_ONLY) && MESH_CLIENT_REPEATER_ONLY
    #define MESH_REPEATER_PSRAM_TABLES 1
  #else
    #define MESH_REPEATER_PSRAM_TABLES 0
  #endif
#endif

#if MESH_REPEATER_PSRAM_TABLES != 0 && MESH_REPEATER_PSRAM_TABLES != 1
#error "MESH_REPEATER_PSRAM_TABLES must be 0 or 1"
#endif

#if MESH_REPEATER_PSRAM_TABLES
#if !defined(ESP32_PLATFORM) || !defined(BOARD_HAS_PSRAM)
#error "Repeater PSRAM tables require an ESP32 target with PSRAM support"
#endif

#include <sdkconfig.h>
#if !defined(CONFIG_IDF_TARGET_ESP32S3) || !CONFIG_IDF_TARGET_ESP32S3 \
    || !defined(CONFIG_SPIRAM_BOOT_INIT) || !CONFIG_SPIRAM_BOOT_INIT \
    || !defined(CONFIG_SPIRAM_USE_MALLOC) || !CONFIG_SPIRAM_USE_MALLOC
#error "Repeater PSRAM tables require ESP32-S3 with boot-initialized PSRAM heaps"
#endif

#include <esp_heap_caps.h>
#include <helpers/MQTTConnectionAdmission.h>
#include <stdio.h>
#include <stdlib.h>
#include <type_traits>

namespace mesh {

// Own only CPU data here. ISR state, locks, task stacks and DMA buffers retain
// their existing internal storage. G3's pinned SDK initializes PSRAM-backed
// heaps before global constructors, so tables can keep their startup lifetime.
template<typename T> class RepeaterPsramStorage {
  static_assert(std::is_trivial<T>::value,
                "Repeater PSRAM tables require zero-initialized trivial data");
  T* value_ = nullptr;
public:
  RepeaterPsramStorage() = default;
  ~RepeaterPsramStorage() { if (value_) heap_caps_free(value_); }
  RepeaterPsramStorage(const RepeaterPsramStorage&) = delete;
  RepeaterPsramStorage& operator=(const RepeaterPsramStorage&) = delete;

  bool begin(const char* label = "repeater table") {
    if (value_) return true;
    value_ = static_cast<T*>(heap_caps_calloc(
        1, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!value_) {
      const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
      if (MQTTConnectionAdmission::canFallback(sizeof(T),
              heap_caps_get_free_size(caps), heap_caps_get_largest_free_block(caps))) {
        value_ = static_cast<T*>(heap_caps_calloc(
            1, sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      }
    }
    if (!value_) {
      fprintf(stderr, "Cannot allocate %s (%u bytes); full capacity is required\n",
              label, static_cast<unsigned>(sizeof(T)));
    }
    return value_ != nullptr;
  }

  T* data() { return value_; }
  const T* data() const { return value_; }
  T& require(const char* label) {
    // Used only while binding the owner's references during construction.
    // Allocation never moves after pointers escape to routing or persistence.
    if (!begin(label)) abort();
    return *value_;
  }
};

} // namespace mesh
#endif
