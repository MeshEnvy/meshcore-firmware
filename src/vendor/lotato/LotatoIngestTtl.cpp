#include "LotatoIngestTtl.h"

#ifdef ESP32

#include "lotato_ingest_ttl.pb.h"

#include <cstring>

namespace {

constexpr uint64_t kUuidSaltIngestTtl = 0x4c6f7461746f5474ull; // 'LotatoTt' — namespace LoDB uuid derivation

void format_node_id_prefix(const uint8_t pub_key[32], char out[12]) {
  static const char* hexd = "0123456789abcdef";
  out[0] = '!';
  for (int i = 0; i < 4; i++) {
    out[1 + i * 2]     = hexd[(pub_key[i] >> 4) & 0x0f];
    out[1 + i * 2 + 1] = hexd[pub_key[i] & 0x0f];
  }
  out[9] = '\0';
}

}  // namespace

LotatoIngestTtlStore& lotato_ingest_ttl_store() {
  static LotatoIngestTtlStore s;
  return s;
}

void LotatoIngestTtlStore::begin() {
  if (_registered) return;
  _db.registerTable("ingest_ttl", &LotatoIngestTtl_msg, sizeof(LotatoIngestTtl));
  _registered = true;
}

lodb_uuid_t LotatoIngestTtlStore::rowUuid(const uint8_t pub_key[32]) {
  char id[12];
  format_node_id_prefix(pub_key, id);
  return lodb_new_uuid(id, kUuidSaltIngestTtl);
}

void LotatoIngestTtlStore::fillRow(const uint8_t pub_key[32], uint32_t unix_ts, void* row_out) {
  auto* r = static_cast<LotatoIngestTtl*>(row_out);
  *r = LotatoIngestTtl_init_zero;
  r->pub_key_prefix.size = 4;
  memcpy(r->pub_key_prefix.bytes, pub_key, 4);
  r->last_posted_unix = unix_ts;
  r->reserved = 0;
}

uint32_t LotatoIngestTtlStore::lastPostedUnix(const uint8_t pub_key[32]) {
  if (!_registered || !pub_key) return 0;
  LotatoIngestTtl row = LotatoIngestTtl_init_zero;
  lodb_uuid_t u = rowUuid(pub_key);
  if (_db.get("ingest_ttl", u, &row) != LODB_OK) return 0;
  if (row.pub_key_prefix.size != 4 || memcmp(row.pub_key_prefix.bytes, pub_key, 4) != 0) return 0;
  return row.last_posted_unix;
}

void LotatoIngestTtlStore::setLastPostedUnix(const uint8_t pub_key[32], uint32_t unix_ts) {
  if (!_registered || !pub_key) return;
  LotatoIngestTtl row = LotatoIngestTtl_init_zero;
  fillRow(pub_key, unix_ts, &row);
  lodb_uuid_t u = rowUuid(pub_key);
  if (_db.update("ingest_ttl", u, &row) == LODB_OK) return;
  (void)_db.insert("ingest_ttl", u, &row);
}

void LotatoIngestTtlStore::clear(const uint8_t pub_key[32]) {
  if (!_registered || !pub_key) return;
  (void)_db.deleteRecord("ingest_ttl", rowUuid(pub_key));
}

#endif
