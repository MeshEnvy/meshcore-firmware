#include <losettings/ConfigHub.h>

#include <helpers/locommand/Engine.h>
#include <helpers/lomessage/Buffer.h>
#include <climits>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace losettings {

const ConfigEntry* ConfigRegistry::findKey(const char* key) const {
  if (!key || !_entries) return nullptr;
  for (int i = 0; i < _n; i++) {
    if (_entries[i].key && strcmp(_entries[i].key, key) == 0) return &_entries[i];
  }
  return nullptr;
}

ConfigHub& ConfigHub::instance() {
  static ConfigHub h;
  return h;
}

ConfigHub::ConfigHub() : _regs{}, _n(0) {}

void ConfigHub::registerModule(const ConfigRegistry& reg) {
  if (_n >= kMaxReg) return;
  for (int i = 0; i < _n; i++) {
    if (_regs[i] && strcmp(_regs[i]->ns(), reg.ns()) == 0) return;
  }
  _regs[_n++] = &reg;
}

bool ConfigHub::splitFullKey(const char* full, char* ns_out, size_t ns_cap, char* key_out, size_t key_cap) {
  if (!full || !ns_out || !key_out || ns_cap < 2 || key_cap < 2) return false;
  const char* p = strchr(full, '.');
  if (!p || p == full) return false;
  size_t n1 = (size_t)(p - full);
  if (n1 >= ns_cap) return false;
  memcpy(ns_out, full, n1);
  ns_out[n1] = '\0';
  strncpy(key_out, p + 1, key_cap - 1);
  key_out[key_cap - 1] = '\0';
  return key_out[0] != '\0';
}

const ConfigEntry* ConfigHub::lookup(const char* full_key, char* ns_out, size_t ns_cap, char* key_only,
                                     size_t key_cap) const {
  if (!splitFullKey(full_key, ns_out, ns_cap, key_only, key_cap)) return nullptr;
  for (int r = 0; r < _n; r++) {
    const ConfigRegistry* reg = _regs[r];
    if (!reg || strcmp(reg->ns(), ns_out) != 0) continue;
    return reg->findKey(key_only);
  }
  return nullptr;
}

static void append_effective_line(lomessage::Buffer& out, const char* full_key, const ConfigEntry* e,
                                  LoSettings& st) {
  out.appendf("%s = ", full_key);
  if (e->is_private) {
    out.append("(redacted)");
  } else {
    switch (e->kind) {
      case ConfigValueKind::Bool:
        out.appendf("%s", st.getBool(e->key, e->def_bool) ? "true" : "false");
        break;
      case ConfigValueKind::Int32:
        out.appendf("%ld", (long)st.getInt(e->key, e->def_i32));
        break;
      case ConfigValueKind::UInt32:
        out.appendf("%lu", (unsigned long)st.getUInt(e->key, e->def_u32));
        break;
      case ConfigValueKind::String: {
        char buf[192];
        st.getString(e->key, buf, sizeof(buf), e->def_str ? e->def_str : "");
        out.append(buf);
        break;
      }
    }
  }
  out.appendf("  (default ");
  if (e->is_private) {
    out.append("(redacted)");
  } else {
    switch (e->kind) {
      case ConfigValueKind::Bool:
        out.appendf("%s", e->def_bool ? "true" : "false");
        break;
      case ConfigValueKind::Int32:
        out.appendf("%ld", (long)e->def_i32);
        break;
      case ConfigValueKind::UInt32:
        out.appendf("%lu", (unsigned long)e->def_u32);
        break;
      case ConfigValueKind::String:
        out.append(e->def_str ? e->def_str : "");
        break;
    }
  }
  out.appendf(")  - %s\n", e->description ? e->description : "");
}

void ConfigHub::formatList(lomessage::Buffer& out) const {
  for (int r = 0; r < _n; r++) {
    const ConfigRegistry* reg = _regs[r];
    if (!reg) continue;
    LoSettings st(reg->ns());
    for (int i = 0; i < reg->count(); i++) {
      const ConfigEntry& e = reg->entries()[i];
      char full[80];
      snprintf(full, sizeof(full), "%s.%s", reg->ns(), e.key);
      append_effective_line(out, full, &e, st);
    }
  }
}

void ConfigHub::formatGet(const char* full_key, lomessage::Buffer& out) const {
  char ns[16], key[48];
  const ConfigEntry* e = lookup(full_key, ns, sizeof(ns), key, sizeof(key));
  if (!e) {
    out.appendf("Err - unknown key %s\n", full_key ? full_key : "");
    return;
  }
  LoSettings st(ns);
  if (e->is_private) {
    out.append("(redacted)\n");
    return;
  }
  switch (e->kind) {
    case ConfigValueKind::Bool:
      out.appendf("%s\n", st.getBool(key, e->def_bool) ? "true" : "false");
      break;
    case ConfigValueKind::Int32:
      out.appendf("%ld\n", (long)st.getInt(key, e->def_i32));
      break;
    case ConfigValueKind::UInt32:
      out.appendf("%lu\n", (unsigned long)st.getUInt(key, e->def_u32));
      break;
    case ConfigValueKind::String: {
      char buf[256];
      st.getString(key, buf, sizeof(buf), e->def_str ? e->def_str : "");
      out.append(buf);
      out.append("\n");
      break;
    }
  }
}

static bool parse_bool_token(const char* v, bool* out) {
  if (!v || !out) return false;
  if (strcasecmp(v, "true") == 0 || strcasecmp(v, "on") == 0 || strcasecmp(v, "yes") == 0 || strcmp(v, "1") == 0) {
    *out = true;
    return true;
  }
  if (strcasecmp(v, "false") == 0 || strcasecmp(v, "off") == 0 || strcasecmp(v, "no") == 0 || strcmp(v, "0") == 0) {
    *out = false;
    return true;
  }
  return false;
}

bool ConfigHub::setFromString(const char* full_key, const char* value, char* err, size_t err_cap) {
  if (err && err_cap) err[0] = '\0';
  if (!full_key || !value) {
    if (err && err_cap) snprintf(err, err_cap, "missing key or value");
    return false;
  }
  char ns[16], key[48];
  const ConfigEntry* e = lookup(full_key, ns, sizeof(ns), key, sizeof(key));
  if (!e) {
    if (err && err_cap) snprintf(err, err_cap, "unknown key");
    return false;
  }
  LoSettings st(ns);
  bool ok = false;
  switch (e->kind) {
    case ConfigValueKind::Bool: {
      bool b;
      if (!parse_bool_token(value, &b)) {
        if (err && err_cap) snprintf(err, err_cap, "expected true/false/on/off/yes/no");
        return false;
      }
      ok = st.setBool(key, b);
      break;
    }
    case ConfigValueKind::Int32: {
      char* end = nullptr;
      long v = strtol(value, &end, 10);
      if (!end || *end != '\0' || v < INT32_MIN || v > INT32_MAX) {
        if (err && err_cap) snprintf(err, err_cap, "invalid int32");
        return false;
      }
      ok = st.setInt(key, (int32_t)v);
      break;
    }
    case ConfigValueKind::UInt32: {
      char* end = nullptr;
      unsigned long v = strtoul(value, &end, 10);
      if (!end || *end != '\0' || v > UINT32_MAX) {
        if (err && err_cap) snprintf(err, err_cap, "invalid uint32");
        return false;
      }
      uint32_t u = (uint32_t)v;
      if (e->has_u32_range && (u < e->u32_min || u > e->u32_max)) {
        if (err && err_cap) snprintf(err, err_cap, "out of range [%lu,%lu]", (unsigned long)e->u32_min,
                                     (unsigned long)e->u32_max);
        return false;
      }
      if (strcmp(ns, "lotato") == 0 && strcmp(key, "ingest.gc_stale_secs") == 0) {
        uint32_t vis = st.getUInt("ingest.visibility_secs", 259200u);
        if (u < vis) {
          if (err && err_cap) snprintf(err, err_cap, "must be >= ingest.visibility_secs (%lu)",
                                         (unsigned long)vis);
          return false;
        }
      }
      ok = st.setUInt(key, u);
      break;
    }
    case ConfigValueKind::String: {
      size_t len = strlen(value);
      size_t max_len = 200;
      if (strcmp(ns, "lotato") == 0) {
        if (strcmp(key, "ingest.url") == 0) max_len = 256;
        if (strcmp(key, "ingest.token") == 0) max_len = 128;
      } else if (strcmp(ns, "lofi") == 0) {
        if (strcmp(key, "active.ssid") == 0) max_len = 32;
        if (strcmp(key, "active.psk") == 0) max_len = 64;
      }
      if (len > max_len) {
        if (err && err_cap) snprintf(err, err_cap, "string too long (max %u)", (unsigned)max_len);
        return false;
      }
      ok = st.setString(key, value);
      break;
    }
  }
  if (!ok) {
    if (err && err_cap) snprintf(err, err_cap, "save failed");
    return false;
  }
  if (e->on_change) e->on_change(e->on_change_ctx);
  return true;
}

bool ConfigHub::unsetKey(const char* full_key, char* err, size_t err_cap) {
  if (err && err_cap) err[0] = '\0';
  char ns[16], key[48];
  const ConfigEntry* e = lookup(full_key, ns, sizeof(ns), key, sizeof(key));
  if (!e) {
    if (err && err_cap) snprintf(err, err_cap, "unknown key");
    return false;
  }
  LoSettings st(ns);
  if (!st.remove(key)) {
    if (err && err_cap) snprintf(err, err_cap, "remove failed or key absent");
    return false;
  }
  if (e->on_change) e->on_change(e->on_change_ctx);
  return true;
}

namespace {

struct ConfigCliCtx {
  // reserved
};

static void h_config_ls(locommand::Context& ctx) {
  (void)ctx.app_ctx;
  ConfigHub::instance().formatList(ctx.out);
}

static void h_config_get(locommand::Context& ctx) {
  if (ctx.argc < 1) {
    ctx.printHelp();
    return;
  }
  ConfigHub::instance().formatGet(ctx.argv[0], ctx.out);
}

static void h_config_set(locommand::Context& ctx) {
  if (ctx.argc < 2) {
    ctx.printHelp();
    return;
  }
  char valbuf[320];
  valbuf[0] = '\0';
  size_t pos = 0;
  for (int i = 1; i < ctx.argc && pos + 1 < sizeof(valbuf); i++) {
    if (i > 1 && pos + 1 < sizeof(valbuf)) valbuf[pos++] = ' ';
    const char* p = ctx.argv[i];
    size_t l = strlen(p);
    if (pos + l >= sizeof(valbuf)) break;
    memcpy(valbuf + pos, p, l);
    pos += l;
    valbuf[pos] = '\0';
  }
  char err[128];
  if (!ConfigHub::instance().setFromString(ctx.argv[0], valbuf, err, sizeof(err))) {
    ctx.out.appendf("Err - %s\n", err);
    return;
  }
  ctx.out.append("OK\n");
}

static void h_config_unset(locommand::Context& ctx) {
  if (ctx.argc < 1) {
    ctx.printHelp();
    return;
  }
  char err[128];
  if (!ConfigHub::instance().unsetKey(ctx.argv[0], err, sizeof(err))) {
    ctx.out.appendf("Err - %s\n", err);
    return;
  }
  ctx.out.append("OK\n");
}

static const locommand::ArgSpec k_get_args[] = {
    {"full_key", "string", nullptr, true, "Namespaced key, e.g. lotato.ingest.url"},
};

static const locommand::ArgSpec k_set_args[] = {
    {"full_key", "string", nullptr, true, "Namespaced key"},
    {"value", "string", nullptr, true, "New value (use quotes via mesh CLI if spaces)"},
};

static const locommand::ArgSpec k_unset_args[] = {
    {"full_key", "string", nullptr, true, "Namespaced key to remove (revert to default)"},
};

}  // namespace

void ConfigHub::bindConfigCli(locommand::Engine& eng) {
  eng.add("ls", h_config_ls, nullptr, nullptr, "list all registered config keys and effective values");
  eng.addWithArgs("get", h_config_get, k_get_args, 1, nullptr, "print one config value");
  eng.addWithArgs("set", h_config_set, k_set_args, 2, nullptr, "set a config value");
  eng.addWithArgs("unset", h_config_unset, k_unset_args, 1, nullptr, "remove saved value (use default)");
}

}  // namespace losettings
