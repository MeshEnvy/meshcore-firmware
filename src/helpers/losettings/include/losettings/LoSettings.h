#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <lodb/LoDB.h>

namespace losettings {

/**
 * Typed key/value store backed by LoDB (one `kv` table per namespace).
 * On disk: `<mount>/__lodb__/<ns>/kv/<hash(key)>.pr`.
 * Writes are atomic (inherited from LoFS rename semantics).
 */
class LoSettings {
public:
  /** `mount` must match a LoFS mount prefix. */
  explicit LoSettings(const char* ns, const char* mount = "/__ext__");

  bool has(const char* key);
  bool remove(const char* key);
  bool clear();

  bool getBool(const char* key, bool def = false);
  int32_t getInt(const char* key, int32_t def = 0);
  uint32_t getUInt(const char* key, uint32_t def = 0);
  float getFloat(const char* key, float def = 0.0f);
  size_t getString(const char* key, char* out, size_t cap, const char* def = "");
  size_t getBytes(const char* key, uint8_t* out, size_t cap);

  bool setBool(const char* key, bool v);
  bool setInt(const char* key, int32_t v);
  bool setUInt(const char* key, uint32_t v);
  bool setFloat(const char* key, float v);
  bool setString(const char* key, const char* v);
  bool setBytes(const char* key, const uint8_t* v, size_t n);

  /** Copies up to @p max names into @p keys. Returns actual count written. */
  int listKeys(char keys[][32], int max);

private:
  bool valid_key(const char* key) const;
  void ensure_registered();

  LoDb _db;
  bool _registered = false;
};

}  // namespace losettings
