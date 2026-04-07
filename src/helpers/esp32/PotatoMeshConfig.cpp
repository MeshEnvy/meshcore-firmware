#include "PotatoMeshConfig.h"

#if defined(ESP32) && defined(POTATO_MESH_INGEST)

#include <Preferences.h>

namespace {

constexpr char kNs[] = "potatomesh";
constexpr char kKeyVer[] = "pm_v";

} // namespace

PotatoMeshConfig& PotatoMeshConfig::instance() {
  static PotatoMeshConfig inst;
  return inst;
}

void PotatoMeshConfig::migrateFromBuildFlagsIfNeeded() {
  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    return;
  }
  if (prefs.getUChar(kKeyVer, 0) != 0) {
    prefs.end();
    return;
  }
#if defined(POTATO_MESH_INGEST_URL)
  if (prefs.getString("url", "").length() == 0) {
    prefs.putString("url", POTATO_MESH_INGEST_URL);
  }
#endif
#if defined(POTATO_MESH_API_TOKEN)
  if (prefs.getString("token", "").length() == 0) {
    prefs.putString("token", POTATO_MESH_API_TOKEN);
  }
#endif
#if defined(POTATO_MESH_WIFI_SSID)
  if (prefs.getString("ssid", "").length() == 0) {
    prefs.putString("ssid", POTATO_MESH_WIFI_SSID);
  }
#endif
#if defined(POTATO_MESH_WIFI_PWD)
  if (!prefs.isKey("pwd")) {
    prefs.putString("pwd", POTATO_MESH_WIFI_PWD);
  }
#endif
  prefs.putUChar(kKeyVer, 1);
  prefs.end();
}

void PotatoMeshConfig::load() {
  migrateFromBuildFlagsIfNeeded();

  Preferences prefs;
  if (!prefs.begin(kNs, true)) {
    _loaded = true;
    return;
  }

  prefs.getString("ssid", _ssid, sizeof(_ssid));
  prefs.getString("pwd", _pwd, sizeof(_pwd));
  prefs.getString("url", _url, sizeof(_url));
  prefs.getString("token", _token, sizeof(_token));
  _debug = prefs.getBool("dbg", false);
  prefs.end();
  _loaded = true;
}

bool PotatoMeshConfig::isIngestReady() const {
  return _ssid[0] != '\0' && _url[0] != '\0' && _token[0] != '\0';
}

static void put_string_pref(const char* key, const char* val) {
  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    return;
  }
  prefs.putString(key, val ? val : "");
  prefs.end();
}

void PotatoMeshConfig::setWifi(const char* s, const char* p) {
  const char* ss = s ? s : "";
  const char* pp = p ? p : "";
  strncpy(_ssid, ss, sizeof(_ssid) - 1);
  _ssid[sizeof(_ssid) - 1] = '\0';
  strncpy(_pwd, pp, sizeof(_pwd) - 1);
  _pwd[sizeof(_pwd) - 1] = '\0';
  put_string_pref("ssid", _ssid);
  put_string_pref("pwd", _pwd);
}

void PotatoMeshConfig::setApiToken(const char* t) {
  const char* tt = t ? t : "";
  strncpy(_token, tt, sizeof(_token) - 1);
  _token[sizeof(_token) - 1] = '\0';
  put_string_pref("token", _token);
}

void PotatoMeshConfig::setIngestOrigin(const char* u) {
  const char* uu = u ? u : "";
  strncpy(_url, uu, sizeof(_url) - 1);
  _url[sizeof(_url) - 1] = '\0';
  put_string_pref("url", _url);
}

void PotatoMeshConfig::setDebug(bool on) {
  _debug = on;
  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    return;
  }
  prefs.putBool("dbg", _debug);
  prefs.end();
}

void PotatoMeshConfig::toggleDebug() {
  setDebug(!debugEnabled());
}

#else

PotatoMeshConfig& PotatoMeshConfig::instance() {
  static PotatoMeshConfig inst;
  return inst;
}

void PotatoMeshConfig::load() {}

bool PotatoMeshConfig::isIngestReady() const { return false; }

void PotatoMeshConfig::setWifi(const char*, const char*) {}

void PotatoMeshConfig::setApiToken(const char*) {}

void PotatoMeshConfig::setIngestOrigin(const char*) {}

void PotatoMeshConfig::setDebug(bool) {}

void PotatoMeshConfig::toggleDebug() {}

#endif
