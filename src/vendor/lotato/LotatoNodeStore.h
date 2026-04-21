#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/** Maximum number of nodes persisted to SPIFFS. Override via build flag. */
#ifndef LOTATO_NODE_STORE_MAX
#define LOTATO_NODE_STORE_MAX 4000
#endif

/**
 * On-disk record per node. Fixed size: 84 bytes.
 * magic == LOTATO_NODE_MAGIC when the slot is occupied.
 */
struct LotatoNodeRecord {
  uint8_t pub_key[32]; // full 32-byte public key
  char    name[32];    // advertised name (null-terminated)
  uint8_t type;        // ADV_TYPE_* from AdvertDataHelpers.h
  uint8_t _pad[3];
  uint32_t last_advert; // Unix timestamp from advert packet
  int32_t  gps_lat;     // latitude  × 1e6 (0 if none)
  int32_t  gps_lon;     // longitude × 1e6 (0 if none)
  uint32_t magic;       // 0x4C4F5441 ('LOTA') if valid
};
static_assert(sizeof(LotatoNodeRecord) == 84, "LotatoNodeRecord layout changed");

#define LOTATO_NODE_MAGIC 0x4C4F5441u

/**
 * Persistent node store for Lotato ingest on SPIFFS (/lotato-nodes.bin).
 *
 * On-disk **dense** layout: 8-byte header + occupancy bitmap + one 84-byte record per
 * occupied logical slot (in slot order). Empty slots omit records, so disk use scales with
 * how many nodes are stored (plus a fixed bitmap, ~(MAX/8) bytes).
 *
 * If `/lotato-nodes.bin` does not start with the dense magic sentinel, it is removed on boot
 * and replaced with an empty store (no legacy flat format).
 *
 * An in-memory index tracks {pub_key prefix, last_advert, last_posted_ms} for fast dedup,
 * LRU eviction, and ingest scheduling. Scheduling throttles off `millis()`, not wall
 * clock, so it works even before NTP. The `ingest_ttl` LoDB table on the `/__ram__`
 * ramdisk is kept for introspection only and is wiped on reboot.
 */
class LotatoNodeStore {
public:
  static constexpr int    MAX         = LOTATO_NODE_STORE_MAX;
  static constexpr size_t RECORD_SIZE = sizeof(LotatoNodeRecord);
  /** Occupancy bitmap size in bytes (one bit per logical slot). */
  static constexpr size_t BITMAP_BYTES = (MAX + 7) / 8;
  static constexpr const char* PATH   = "/lotato-nodes.bin";

  /**
   * Open (or create) the node file. Must be called after SPIFFS.begin().
   * Rebuilds the in-memory index from disk on first call.
   */
  void begin(fs::FS* fs);

  /**
   * Insert or update a node. Returns the slot index, or -1 on error.
   * Evicts the least-recently-heard node when the store is full.
   */
  int put(const uint8_t* pub_key, const char* name, uint8_t type,
          uint32_t last_advert, int32_t lat, int32_t lon);

  /** True if this occupied slot should be included in the next ingest batch (uptime-throttled). */
  bool dueForIngest(int slot, uint32_t now_ms) const;

  /** Record successful POST at @p now_ms (`millis()`); also updates RAM-TTL for introspection. */
  void markPosted(int slot, uint32_t now_ms);

  /** Read the full on-disk record for slot. Returns false on I/O error. */
  bool readRecord(int slot, LotatoNodeRecord& out) const;

  /** Total number of occupied slots. */
  int count() const { return _count; }

  /** Clear last-post time for ALL occupied slots, making them due again. */
  void flushAllTtl();

  /** Remove stale slots not mesh-heard for `lotato.ingest.gc_stale_secs` (no-op if wall clock invalid). */
  void gcSweepStale();

  /** Remove one occupied slot from SPIFFS + index + ingest TTL. */
  bool removeSlot(int slot);

  /** Count occupied slots that pass the mesh-heard visibility window. */
  int countVisibleNodes() const;

  /** Count occupied slots that are visible and due for ingest. */
  int countDueNodes() const;

  /** Log !id list for occupied slots when debug is on (truncated if very many nodes). */
  void logFlushTargetsDebug() const;

  /** Verbose per-slot dump: id, name, type, last_advert, last_posted_ms, gps. Debug log only. */
  void dumpAllNodesDebug() const;

private:
  bool visibleMeshHeard(int slot) const;

  struct Entry {
    uint8_t  key[4];         // first 4 bytes of pub_key for fast match
    uint32_t last_advert;    // cached last_advert for LRU eviction
    uint32_t last_posted_ms; // millis() of last successful post; 0 = never posted (or flushed)
  };

  fs::FS* _fs = nullptr;
  int     _count = 0;
  Entry   _index[MAX]{};
  mutable SemaphoreHandle_t _idx_mtx = nullptr;

  int  findSlot(const uint8_t* pub_key) const;
  int  findEmptySlot() const;
  int  evictLRU();
  bool writeRecord(int slot, const LotatoNodeRecord& rec);
  void loadIndex();
};

#endif // ESP32
