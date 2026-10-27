#pragma once

#include <MeshCore.h>
#include <string.h>
#include "FloodAdvertRtcPolicy.h"
#include "FloodAdvertIramPolicy.h"
#include "RepeaterPsramStorage.h"

#ifndef FLOOD_ADVERT_SOURCE_SLOTS
  #if defined(STM32_PLATFORM)
    #define FLOOD_ADVERT_SOURCE_SLOTS 8
  #elif defined(NRF52_PLATFORM)
    // Preserve the runtime heap reserve alongside the fixed 64 KiB mOTA arena.
    #define FLOOD_ADVERT_SOURCE_SLOTS 96
  #else
    #define FLOOD_ADVERT_SOURCE_SLOTS 128
  #endif
#endif

namespace mesh {
#if FLOOD_ADVERT_IRAM_WORD_STORAGE
volatile uint32_t* floodAdvertIramBank();
#endif

// Volatile, bounded receive history. Only forwarding roles own this table.
// All times are uint32_t uptime milliseconds, never advert timestamps or RTC.
class FloodAdvertLimiter {
public:
  static constexpr uint32_t WINDOW_MS = 180UL * 60UL * 1000UL;
  static constexpr uint32_t BAD_INTERVAL_MS = 12UL * 60UL * 60UL * 1000UL;
  static constexpr uint32_t RECOVERY_MS = 7UL * 24UL * 60UL * 60UL * 1000UL;
  static constexpr uint8_t HASH_SLOTS = 11; // maximum normal allowance + evidence of excess
  static_assert(HASH_SLOTS <= 15, "Advert hash count must fit its four-bit field");
  static constexpr uint8_t PREFIX_BYTES = 6; // first 12 public-key hex characters

  enum class Decision : uint8_t { Allow, Duplicate, Quota, BadList, Capacity };

  struct Entry {
    uint8_t key[PUB_KEY_SIZE];
    uint8_t hashes[HASH_SLOTS][MAX_HASH_SIZE];
    uint32_t window_start;
    uint32_t last_violation;
    uint32_t last_forwarded;
    uint32_t last_heard;
    uint16_t forwarded;
    uint8_t min_hops;
    // RAM-only state: count is 0..11 and the four flags are independent.
    // Sharing one byte also removes tail padding without shortening history.
    uint8_t count : 4;
    uint8_t flags : 4;
  };

  enum LimitReason : uint8_t { WindowQuota = 1, ReceiveHistory = 2, BadListRule = 4 };
  struct LimitedEntry {
    uint8_t key[PUB_KEY_SIZE];
    uint32_t wait_ms;
    uint32_t recovery_ms;
    uint16_t forwarded;
    uint8_t quota;
    uint8_t hops;
    uint8_t reasons;
  };

  void reset();
  void clear(const uint8_t* full_key);
  void tick(uint32_t now);
  static uint8_t allowance(uint8_t hops);
  bool needsShorterPath(const uint8_t* key, uint8_t hops, uint32_t now);
  void noteKnownCopy(const uint8_t* key, const uint8_t* hash, uint32_t now);
  // Call ONLY after signature validation. Previously seen copies may improve
  // the minimum path, but cannot add evidence or consume forwarding quota.
  void observe(const uint8_t* key, const uint8_t* hash, uint8_t hops,
               uint32_t now, bool previously_seen = false);
  Decision check(const uint8_t* key, const uint8_t* hash, uint32_t now);
  void commit(const uint8_t* key, const uint8_t* hash, uint32_t now);
  bool isBad(const uint8_t* key, uint32_t now);
  // Return the total number of limited full keys and copy the requested slice.
  // Queries only expire elapsed history; they never consume quota, refresh
  // last-heard, or add abuse evidence. Prefix collisions remain separate keys.
  size_t listLimited(uint32_t now, size_t offset, LimitedEntry* dest, size_t count);

protected:
#if FLOOD_ADVERT_IRAM_WORD_STORAGE
  FloodAdvertLimiter(volatile uint32_t* entries, size_t capacity);
  ~FloodAdvertLimiter();
#elif FLOOD_ADVERT_RTC_FAST_SLOTS || FLOOD_ADVERT_RTC_SLOW_SLOTS
  FloodAdvertLimiter(Entry* entries, size_t capacity, Entry* fast, Entry* slow)
      : _entries(entries, fast, slow), _capacity(capacity) {
    if (capacity != FLOOD_ADVERT_SOURCE_SLOTS) abort();
  }
#else
  FloodAdvertLimiter(Entry* entries, size_t capacity) : _entries(entries), _capacity(capacity) {}
#if MESH_REPEATER_PSRAM_TABLES
  // Bind the complete table from begin(), after Arduino initializes PSRAM,
  // before reset() or any limiter operation can inspect the entries.
  void bindStorage(Entry* entries) {
    if (!entries || reinterpret_cast<uintptr_t>(entries) % alignof(Entry)) abort();
    _entries = entries;
  }
#endif
#endif
  FloodAdvertLimiter(const FloodAdvertLimiter&) = delete;
  FloodAdvertLimiter& operator=(const FloodAdvertLimiter&) = delete;

private:
  enum : uint8_t { ACTIVE = 1, PREVIOUS_OVER = 2, BAD = 4, HAS_FORWARDED = 8 };
  static_assert((ACTIVE | PREVIOUS_OVER | BAD | HAS_FORWARDED) <= 15,
                "Advert flags must fit their four-bit field");
#if FLOOD_ADVERT_IRAM_WORD_STORAGE
  volatile uint32_t* _entries;
  static const void*& iramOwner();
  void requireIramOwner(size_t index) const;
  struct EntryTail {
    uint16_t forwarded;
    uint8_t min_hops;
    uint8_t count : 4;
    uint8_t flags : 4;
  };
  static_assert(sizeof(Entry) == 140 && sizeof(EntryTail) == 4
                && offsetof(Entry, forwarded) == 136 && offsetof(Entry, min_hops) == 138,
                "Qualified IRAM codec must preserve the original record ABI");
  static constexpr size_t EntryWords = sizeof(Entry) / sizeof(uint32_t);
  void loadEntry(size_t index, Entry& dest) const;
  void storeEntry(size_t index, const Entry& source);
  EntryTail readTail(size_t index) const;
  void writeTail(size_t index, EntryTail tail);
  uint32_t readStamp(size_t index, size_t offset) const;
  bool keyMatches(size_t index, const uint8_t* key, size_t length) const;
  size_t findIndex(const uint8_t* key) const;
#elif FLOOD_ADVERT_RTC_FAST_SLOTS || FLOOD_ADVERT_RTC_SLOW_SLOTS
  // CPU-only banks. No transport or DMA engine receives these addresses.
  class EntryStorage {
    static constexpr size_t FastCount = FLOOD_ADVERT_RTC_FAST_SLOTS;
    static constexpr size_t SlowCount = FLOOD_ADVERT_RTC_SLOW_SLOTS;
    static constexpr size_t MainCount = FLOOD_ADVERT_SOURCE_SLOTS - FastCount - SlowCount;
    static_assert(FLOOD_ADVERT_SOURCE_SLOTS > FastCount + SlowCount,
                  "Advert history must retain an ordinary main bank");
    static_assert(sizeof(Entry) == 140 && std::is_trivial<Entry>::value,
                  "Qualified advert records must retain every original field");
    static_assert(sizeof(Entry) * FastCount <= 7168 && sizeof(Entry) * SlowCount <= 7168,
                  "Advert banks must leave SDK RTC storage headroom");
    Entry* main_;
    Entry* fast_;
    Entry* slow_;
    static const void*& owner() {
      // This lease lives in ordinary BSS and resets on every boot/deep wake.
      static const void* value = nullptr;
      return value;
    }
    void requireOwner() const { if (owner() != this) abort(); }
  public:
    EntryStorage(Entry* main, Entry* fast, Entry* slow)
        : main_(main), fast_(fast), slow_(slow) {
      if (!main || reinterpret_cast<uintptr_t>(main) % alignof(Entry)
          || (FastCount && (!fast || reinterpret_cast<uintptr_t>(fast) % 4))
          || (SlowCount && (!slow || reinterpret_cast<uintptr_t>(slow) % 4))
          || owner() != nullptr) abort();
      owner() = this;
    }
    ~EntryStorage() { requireOwner(); owner() = nullptr; }
    EntryStorage(const EntryStorage&) = delete;
    EntryStorage& operator=(const EntryStorage&) = delete;
    Entry& operator[](size_t index) {
      requireOwner();
      if (index >= FLOOD_ADVERT_SOURCE_SLOTS) abort();
      if (index < MainCount) return main_[index];
      index -= MainCount;
      if (index < FastCount) return fast_[index];
      return slow_[index - FastCount];
    }
    void reset() {
      requireOwner();
      // Clear in place, including retained bytes after reboot/deep wake.
      memset(main_, 0, sizeof(Entry) * MainCount);
      if (FastCount) memset(fast_, 0, sizeof(Entry) * FastCount);
      if (SlowCount) memset(slow_, 0, sizeof(Entry) * SlowCount);
    }
  };
  EntryStorage _entries;
#else
  Entry* _entries;
#endif
  size_t _capacity;
  uint32_t _last_sweep = 0;
#if !FLOOD_ADVERT_IRAM_WORD_STORAGE
  Entry* find(const uint8_t* key);
#endif
  static int hashIndex(const Entry& entry, const uint8_t* hash);
  static void advance(Entry& entry, uint32_t now);
  void refresh(uint32_t now);
};

template<size_t Capacity = FLOOD_ADVERT_SOURCE_SLOTS>
class StaticFloodAdvertLimiter : public FloodAdvertLimiter {
  static_assert(Capacity > 0, "Advert limiter needs at least one source slot");
#if FLOOD_ADVERT_IRAM_WORD_STORAGE
  static_assert(Capacity == FLOOD_ADVERT_SOURCE_SLOTS,
                "IRAM advert storage retains the exact full source capacity");
public:
  StaticFloodAdvertLimiter() : FloodAdvertLimiter(floodAdvertIramBank(), Capacity) { reset(); }
#elif FLOOD_ADVERT_RTC_FAST_SLOTS || FLOOD_ADVERT_RTC_SLOW_SLOTS
  static_assert(Capacity == FLOOD_ADVERT_SOURCE_SLOTS,
                "RTC advert storage requires the exact fixed source capacity");
  Entry _storage[Capacity - FLOOD_ADVERT_RTC_FAST_SLOTS - FLOOD_ADVERT_RTC_SLOW_SLOTS];
public:
  StaticFloodAdvertLimiter(Entry* fast, Entry* slow)
      : FloodAdvertLimiter(_storage, Capacity, fast, slow) { reset(); }
#elif MESH_REPEATER_PSRAM_TABLES
  static_assert(sizeof(Entry) == 140,
                "Update the advert PSRAM fallback budget in check_firmware_ram.py");
  struct Storage { Entry entries[Capacity]; };
  RepeaterPsramStorage<Storage> _storage;
public:
  StaticFloodAdvertLimiter() : FloodAdvertLimiter(nullptr, Capacity) {}
  bool begin() {
    if (_storage.data()) return true;
    // Exhaustion must stop startup rather than disabling advert protection
    // or reducing its source/hash history. No full internal array is retained.
    if (!_storage.begin("advert source history")) return false;
    bindStorage(_storage.data()->entries);
    reset();
    return true;
  }
#else
  Entry _storage[Capacity];
public:
  StaticFloodAdvertLimiter() : FloodAdvertLimiter(_storage, Capacity) { reset(); }
#endif
#if !MESH_REPEATER_PSRAM_TABLES || FLOOD_ADVERT_IRAM_WORD_STORAGE \
    || FLOOD_ADVERT_RTC_FAST_SLOTS || FLOOD_ADVERT_RTC_SLOW_SLOTS
  bool begin() { return true; }
#endif
};

} // namespace mesh
