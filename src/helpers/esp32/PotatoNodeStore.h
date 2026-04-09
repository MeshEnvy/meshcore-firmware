#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <FS.h>

/** Maximum number of nodes persisted to SPIFFS. Override via build flag. */
#ifndef POTATO_NODE_STORE_MAX
#define POTATO_NODE_STORE_MAX 4000
#endif

/** How often each node is re-posted to potato-mesh to keep lastHeard fresh (ms). */
#ifndef POTATO_NODE_REPOST_MS
#define POTATO_NODE_REPOST_MS (15UL * 60UL * 1000UL)
#endif

/** Minimum gap between successive POSTs of the same node from a fresh advert (ms). */
#ifndef POTATO_NODE_COOLOFF_MS
#define POTATO_NODE_COOLOFF_MS 60000UL
#endif

/** How many slots the sweep advances per loop() call. */
#ifndef POTATO_NODE_SWEEP_PER_LOOP
#define POTATO_NODE_SWEEP_PER_LOOP 5
#endif

/**
 * On-disk record per node. Fixed size: 84 bytes.
 * magic == POTATO_NODE_MAGIC when the slot is occupied.
 */
struct PotatoNodeRecord {
  uint8_t pub_key[32]; // full 32-byte public key
  char    name[32];    // advertised name (null-terminated)
  uint8_t type;        // ADV_TYPE_* from AdvertDataHelpers.h
  uint8_t _pad[3];
  uint32_t last_advert; // Unix timestamp from advert packet
  int32_t  gps_lat;     // latitude  × 1e6 (0 if none)
  int32_t  gps_lon;     // longitude × 1e6 (0 if none)
  uint32_t magic;       // 0x504F5441 ('POTA') if valid
};
static_assert(sizeof(PotatoNodeRecord) == 84, "PotatoNodeRecord layout changed");

#define POTATO_NODE_MAGIC 0x504F5441u

/**
 * Persistent flat-file node store for potato-mesh ingest.
 *
 * Stores up to POTATO_NODE_STORE_MAX nodes in a fixed-size binary file on
 * SPIFFS (/potato-nodes.bin). Each occupied slot is 84 bytes.
 *
 * An in-memory index tracks {pub_key prefix, last_advert, last_posted_ms}
 * for fast dedup and LRU eviction without reading the file on every advert.
 *
 * A periodic incremental sweep re-posts all known nodes to potato-mesh
 * (POTATO_NODE_REPOST_MS interval), advancing POTATO_NODE_SWEEP_PER_LOOP
 * slots per loop() call so it never blocks the mesh radio loop.
 */
class PotatoNodeStore {
public:
  static constexpr int    MAX         = POTATO_NODE_STORE_MAX;
  static constexpr size_t RECORD_SIZE = sizeof(PotatoNodeRecord);
  static constexpr const char* PATH   = "/potato-nodes.bin";

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

  /**
   * True if enough time has passed to POST this slot (cooloff or repost window).
   * Use after put() to decide whether to call postContactDiscovered immediately.
   */
  bool shouldPost(int slot, uint32_t now_ms) const;

  /** Record that slot was just posted at now_ms. */
  void markPosted(int slot, uint32_t now_ms);

  /** Read the full on-disk record for slot. Returns false on I/O error. */
  bool readRecord(int slot, PotatoNodeRecord& out) const;

  /** Total number of occupied slots. */
  int count() const { return _count; }

  /** Reset all post timers so every node is re-posted on the next sweep pass. */
  void resetPostTimers() {
    for (int i = 0; i < MAX; i++) _index[i].last_posted_ms = 0;
  }

  /** Log !id list for occupied slots when debug is on (truncated if very many nodes). */
  void logFlushTargetsDebug() const;

  /**
   * Call once per loop(). Advances the sweep cursor by POTATO_NODE_SWEEP_PER_LOOP
   * slots, reads and returns the next slot due for re-posting (or -1 if none
   * ready this pass). Caller should read that slot's record and post it.
   *
   * Returns the slot index that needs re-posting, or -1.
   * When non-negative, caller must call markPosted(slot, millis()) after posting.
   */
  int sweepNext(uint32_t now_ms);

private:
  struct Entry {
    uint8_t  key[4];          // first 4 bytes of pub_key for fast match
    uint32_t last_advert;     // cached last_advert for LRU eviction
    uint32_t last_posted_ms;  // millis() when last posted (0 = never this session)
  };

  fs::FS* _fs        = nullptr;
  int     _count     = 0;
  int     _sweep_cur = 0; // next slot to inspect during sweep
  Entry   _index[MAX]{};

  int  findSlot(const uint8_t* pub_key) const;
  int  findEmptySlot() const;
  int  evictLRU();
  bool writeRecord(int slot, const PotatoNodeRecord& rec);
  void loadIndex();
};

#endif // ESP32
