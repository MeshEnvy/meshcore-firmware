#include <losettings/LoSettings.h>

#include "losettings.pb.h"

#include <cstdlib>
#include <cstring>

namespace losettings {

namespace {

constexpr const char* kTable = "kv";

enum Kind : uint32_t {
  KIND_BOOL = 1,
  KIND_INT32 = 2,
  KIND_UINT32 = 3,
  KIND_FLOAT = 4,
  KIND_STRING = 5,
  KIND_BYTES = 6,
};

bool load(LoDb& db, const char* key, LoSettingsKv& out) {
  if (!key) return false;
  return db.get(kTable, lodb_new_uuid(key, 0), &out) == LODB_OK;
}

bool save(LoDb& db, const char* key, const LoSettingsKv& rec) {
  lodb_uuid_t id = lodb_new_uuid(key, 0);
  if (db.update(kTable, id, &rec) == LODB_OK) return true;
  return db.insert(kTable, id, &rec) == LODB_OK;
}

void fill_base(LoSettingsKv& rec, const char* key, uint32_t kind) {
  memset(&rec, 0, sizeof(rec));
  strncpy(rec.key, key, sizeof(rec.key) - 1);
  rec.kind = kind;
}

}  // namespace

LoSettings::LoSettings(const char* ns, const char* mount) : _db(ns, mount) {}

void LoSettings::ensure_registered() {
  if (_registered) return;
  _db.registerTable(kTable, &LoSettingsKv_msg, sizeof(LoSettingsKv));
  _registered = true;
}

bool LoSettings::valid_key(const char* key) const {
  if (!key || !key[0]) return false;
  size_t n = strlen(key);
  if (n > 32) return false;
  for (size_t i = 0; i < n; i++) {
    char c = key[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '-';
    if (!ok) return false;
  }
  return true;
}

bool LoSettings::has(const char* key) {
  if (!valid_key(key)) return false;
  ensure_registered();
  LoSettingsKv rec = LoSettingsKv_init_zero;
  return load(_db, key, rec);
}

bool LoSettings::remove(const char* key) {
  if (!valid_key(key)) return false;
  ensure_registered();
  return _db.deleteRecord(kTable, lodb_new_uuid(key, 0)) == LODB_OK;
}

bool LoSettings::clear() {
  ensure_registered();
  return _db.truncate(kTable) == LODB_OK;
}

bool LoSettings::getBool(const char* key, bool def) {
  if (!valid_key(key)) return def;
  ensure_registered();
  LoSettingsKv rec = LoSettingsKv_init_zero;
  if (!load(_db, key, rec) || rec.kind != KIND_BOOL || rec.data.size < 1) return def;
  return rec.data.bytes[0] != 0;
}

int32_t LoSettings::getInt(const char* key, int32_t def) {
  if (!valid_key(key)) return def;
  ensure_registered();
  LoSettingsKv rec = LoSettingsKv_init_zero;
  if (!load(_db, key, rec) || rec.kind != KIND_INT32 || rec.data.size < 4) return def;
  int32_t v;
  memcpy(&v, rec.data.bytes, 4);
  return v;
}

uint32_t LoSettings::getUInt(const char* key, uint32_t def) {
  if (!valid_key(key)) return def;
  ensure_registered();
  LoSettingsKv rec = LoSettingsKv_init_zero;
  if (!load(_db, key, rec) || rec.kind != KIND_UINT32 || rec.data.size < 4) return def;
  uint32_t v;
  memcpy(&v, rec.data.bytes, 4);
  return v;
}

float LoSettings::getFloat(const char* key, float def) {
  if (!valid_key(key)) return def;
  ensure_registered();
  LoSettingsKv rec = LoSettingsKv_init_zero;
  if (!load(_db, key, rec) || rec.kind != KIND_FLOAT || rec.data.size < 4) return def;
  float v;
  memcpy(&v, rec.data.bytes, 4);
  return v;
}

size_t LoSettings::getString(const char* key, char* out, size_t cap, const char* def) {
  if (!out || cap < 1) return 0;
  out[0] = '\0';
  const char* src = def ? def : "";
  size_t src_len = strlen(src);
  size_t dn = (src_len < cap - 1) ? src_len : cap - 1;
  if (!valid_key(key)) {
    memcpy(out, src, dn);
    out[dn] = '\0';
    return dn;
  }
  ensure_registered();
  LoSettingsKv rec = LoSettingsKv_init_zero;
  if (!load(_db, key, rec) || rec.kind != KIND_STRING) {
    memcpy(out, src, dn);
    out[dn] = '\0';
    return dn;
  }
  size_t n = rec.data.size;
  if (n > cap - 1) n = cap - 1;
  memcpy(out, rec.data.bytes, n);
  out[n] = '\0';
  return n;
}

size_t LoSettings::getBytes(const char* key, uint8_t* out, size_t cap) {
  if (!valid_key(key) || !out || cap == 0) return 0;
  ensure_registered();
  LoSettingsKv rec = LoSettingsKv_init_zero;
  if (!load(_db, key, rec) || rec.kind != KIND_BYTES) return 0;
  size_t n = rec.data.size;
  if (n > cap) n = cap;
  memcpy(out, rec.data.bytes, n);
  return n;
}

bool LoSettings::setBool(const char* key, bool v) {
  if (!valid_key(key)) return false;
  ensure_registered();
  LoSettingsKv rec;
  fill_base(rec, key, KIND_BOOL);
  rec.data.size = 1;
  rec.data.bytes[0] = v ? 1 : 0;
  return save(_db, key, rec);
}

bool LoSettings::setInt(const char* key, int32_t v) {
  if (!valid_key(key)) return false;
  ensure_registered();
  LoSettingsKv rec;
  fill_base(rec, key, KIND_INT32);
  rec.data.size = 4;
  memcpy(rec.data.bytes, &v, 4);
  return save(_db, key, rec);
}

bool LoSettings::setUInt(const char* key, uint32_t v) {
  if (!valid_key(key)) return false;
  ensure_registered();
  LoSettingsKv rec;
  fill_base(rec, key, KIND_UINT32);
  rec.data.size = 4;
  memcpy(rec.data.bytes, &v, 4);
  return save(_db, key, rec);
}

bool LoSettings::setFloat(const char* key, float v) {
  if (!valid_key(key)) return false;
  ensure_registered();
  LoSettingsKv rec;
  fill_base(rec, key, KIND_FLOAT);
  rec.data.size = 4;
  memcpy(rec.data.bytes, &v, 4);
  return save(_db, key, rec);
}

bool LoSettings::setString(const char* key, const char* v) {
  if (!valid_key(key)) return false;
  ensure_registered();
  const char* src = v ? v : "";
  size_t n = strlen(src);
  if (n > sizeof(LoSettingsKv::data.bytes)) n = sizeof(LoSettingsKv::data.bytes);
  LoSettingsKv rec;
  fill_base(rec, key, KIND_STRING);
  rec.data.size = (pb_size_t)n;
  if (n) memcpy(rec.data.bytes, src, n);
  return save(_db, key, rec);
}

bool LoSettings::setBytes(const char* key, const uint8_t* v, size_t n) {
  if (!valid_key(key) || (!v && n)) return false;
  ensure_registered();
  if (n > sizeof(LoSettingsKv::data.bytes)) n = sizeof(LoSettingsKv::data.bytes);
  LoSettingsKv rec;
  fill_base(rec, key, KIND_BYTES);
  rec.data.size = (pb_size_t)n;
  if (n) memcpy(rec.data.bytes, v, n);
  return save(_db, key, rec);
}

int LoSettings::listKeys(char keys[][32], int max) {
  ensure_registered();
  if (!keys || max <= 0) return 0;
  auto rows = _db.select(kTable);
  int n = 0;
  for (void* row : rows) {
    if (n >= max) break;
    LoSettingsKv* kv = (LoSettingsKv*)row;
    strncpy(keys[n], kv->key, 31);
    keys[n][31] = '\0';
    n++;
  }
  LoDb::freeRecords(rows);
  return n;
}

}  // namespace losettings
