#pragma once

#include <ArduinoJson.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM) && defined(BOARD_HAS_PSRAM)
#include <esp_heap_caps.h>
#include <helpers/MQTTConnectionAdmission.h>
#endif

namespace mesh { namespace webconfig {

// JSON pools and strings are CPU workspaces, never SDK/DMA buffers. Avoid
// ordinary malloc's small-allocation preference for scarce internal RAM.
class JsonAllocator : public ArduinoJson::Allocator {
public:
  void* allocate(size_t size) override {
    if (!size) return nullptr;
#if defined(ESP_PLATFORM) && defined(BOARD_HAS_PSRAM)
    void* memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory) return memory;
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    if (!MQTTConnectionAdmission::canFallback(size, heap_caps_get_free_size(caps),
                                              heap_caps_get_largest_free_block(caps))) return nullptr;
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return malloc(size);
#endif
  }

  void deallocate(void* memory) override {
#if defined(ESP_PLATFORM) && defined(BOARD_HAS_PSRAM)
    heap_caps_free(memory);
#else
    free(memory);
#endif
  }

  void* reallocate(void* memory, size_t size) override {
    if (!memory) return allocate(size);
    if (!size) { deallocate(memory); return nullptr; }
#if defined(ESP_PLATFORM) && defined(BOARD_HAS_PSRAM)
    const size_t previous = heap_caps_get_allocated_size(memory);
    // ArduinoJson shrinks pools and string buffers after parsing. Keeping the
    // original allocation is valid and cannot turn a shrink into a failure or
    // move PSRAM data into internal RAM. It is freed with the short-lived doc.
    if (size <= previous) return memory;
    void* replacement = allocate(size);
    if (!replacement) return nullptr; // realloc failure leaves the old data valid
    memcpy(replacement, memory, previous);
    deallocate(memory);
    return replacement;
#else
    return realloc(memory, size);
#endif
  }

  static JsonAllocator* instance() {
    static JsonAllocator allocator;
    return &allocator;
  }
};

#if defined(ESP_PLATFORM) && defined(BOARD_HAS_PSRAM)
// Match ArduinoJson 7's compatibility wrappers, including their nominal
// capacity() values. Pool growth and every existing route limit stay intact.
class DynamicJsonDocument : public ArduinoJson::JsonDocument {
  size_t capacity_;
public:
  explicit DynamicJsonDocument(size_t capacity)
      : ArduinoJson::JsonDocument(JsonAllocator::instance()), capacity_(capacity) {}
  size_t capacity() const { return capacity_; }
};
template<size_t Capacity> class StaticJsonDocument : public ArduinoJson::JsonDocument {
public:
  StaticJsonDocument() : ArduinoJson::JsonDocument(JsonAllocator::instance()) {}
  size_t capacity() const { return Capacity; }
};
#else
using DynamicJsonDocument = ArduinoJson::DynamicJsonDocument;
template<size_t Capacity> using StaticJsonDocument = ArduinoJson::StaticJsonDocument<Capacity>;
#endif

} } // namespace mesh::webconfig
