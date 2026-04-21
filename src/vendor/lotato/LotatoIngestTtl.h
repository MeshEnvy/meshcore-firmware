#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <cstdint>
#include <lodb/LoDB.h>

/** LoDB-backed last successful ingest POST time per node (keyed by stable LoDB uuid from node id). */
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

  LoDb _db{"lotato", "/__ext__"};
  bool _registered = false;
};

LotatoIngestTtlStore& lotato_ingest_ttl_store();

#endif
