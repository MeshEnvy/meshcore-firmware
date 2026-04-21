#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

#include <losettings/LoSettings.h>

namespace lomessage {
class Buffer;
}

namespace locommand {
class Engine;
}

namespace losettings {

enum class ConfigValueKind : uint8_t { Bool, Int32, UInt32, String };

/** One persisted key under a LoSettings namespace (e.g. ns "lotato", key "ingest.url"). */
struct ConfigEntry {
  const char* key;
  ConfigValueKind kind;
  bool def_bool;
  int32_t def_i32;
  uint32_t def_u32;
  const char* def_str;
  const char* description;
  bool is_private;
  bool has_u32_range;
  uint32_t u32_min;
  uint32_t u32_max;
  void (*on_change)(void* ctx);
  void* on_change_ctx;
};

/** Static table of entries for one LoSettings namespace. */
class ConfigRegistry {
public:
  constexpr ConfigRegistry(const char* ns, const ConfigEntry* entries, int n)
      : _ns(ns), _entries(entries), _n(n) {}

  const char* ns() const { return _ns; }
  const ConfigEntry* entries() const { return _entries; }
  int count() const { return _n; }

  const ConfigEntry* findKey(const char* key) const;

private:
  const char* _ns;
  const ConfigEntry* _entries;
  int _n;
};

/** Aggregates registries and implements config ls/get/set/unset. */
class ConfigHub {
public:
  static ConfigHub& instance();

  void registerModule(const ConfigRegistry& reg);

  /** Split "lotato.ingest.url" -> ns + key (key may contain dots). */
  static bool splitFullKey(const char* full, char* ns_out, size_t ns_cap, char* key_out, size_t key_cap);

  void formatList(lomessage::Buffer& out) const;
  void formatGet(const char* full_key, lomessage::Buffer& out) const;
  bool setFromString(const char* full_key, const char* value, char* err, size_t err_cap);
  bool unsetKey(const char* full_key, char* err, size_t err_cap);

  /** Register config ls/get/set/unset on @p eng (root must be "config"). */
  static void bindConfigCli(locommand::Engine& eng);

private:
  static constexpr int kMaxReg = 8;
  ConfigHub();

  const ConfigRegistry* _regs[kMaxReg];
  int _n;

  const ConfigEntry* lookup(const char* full_key, char* ns_out, size_t ns_cap, char* key_only, size_t key_cap) const;
};

}  // namespace losettings
