#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <cstdint>
#include <lodb/LoDB.h>

/**
 * Last successful ingest POST time per node, keyed by stable LoDB uuid from node id.
 *
 * Backed by LoDB mounted on the `/__ram__` ramdisk: every entry is RAM-only and
 * wiped on reboot. This is intentional — after power cycle, the ingest worker
 * should treat every occupied node-store slot as "never posted" and mirror it
 * again. Refresh cadence / OCC throttling within a single uptime still works.
 */
class LotatoIngestTtlStore {
public:
  void begin();

  /** 0 if no row or error. */
  uint32_t lastPostedUnix(const uint8_t pub_key[32]);

  void setLastPostedUnix(const uint8_t pub_key[32], uint32_t unix_ts);
  void clear(const uint8_t pub_key[32]);

private:
  static lodb_uuid_t rowUuid(const uint8_t pub_key[32]);
  static void fillRow(const uint8_t pub_key[32], uint32_t unix_ts, void* row_out);

  LoDb _db{"lotato", "/__ram__"};
  bool _registered = false;
};

LotatoIngestTtlStore& lotato_ingest_ttl_store();

#endif
