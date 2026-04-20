#include "LotatoConfig.h"

#ifdef ESP32

#include <Preferences.h>
#include <helpers/esp32/LotatoDebug.h>
#include <cstring>

namespace {

constexpr char kNs[] = "lotato";
constexpr char kKeyVer[] = "lv";
constexpr char kKeyKn[] = "kn";

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

} // namespace

LotatoConfig& LotatoConfig::instance() {
  static LotatoConfig inst;
  return inst;
}

void LotatoConfig::migrateFromBuildFlagsIfNeeded() {
  Preferences prefs;
  if (!prefs.begin(kNs, false)) return;
  if (prefs.getUChar(kKeyVer, 0) != 0) { prefs.end(); return; }
#if defined(LOTATO_INGEST_URL)
  if (prefs.getString("url", "").length() == 0) prefs.putString("url", LOTATO_INGEST_URL);
#endif
#if defined(LOTATO_API_TOKEN)
  if (prefs.getString("token", "").length() == 0) prefs.putString("token", LOTATO_API_TOKEN);
#endif
#if defined(LOTATO_WIFI_SSID)
  if (prefs.getString("ssid", "").length() == 0) prefs.putString("ssid", LOTATO_WIFI_SSID);
#endif
#if defined(LOTATO_WIFI_PWD)
  if (!prefs.isKey("pwd")) prefs.putString("pwd", LOTATO_WIFI_PWD);
#endif
  prefs.putUChar(kKeyVer, 1);
  prefs.end();
}

void LotatoConfig::migrateKnownProfilesIfNeeded() {
  Preferences prefs;
  if (!prefs.begin(kNs, false)) return;
  uint8_t v = prefs.getUChar(kKeyVer, 0);
  if (v >= 2) { prefs.end(); return; }
  if (prefs.getUChar(kKeyKn, 0) == 0 && prefs.getString("ks0", "").length() == 0) {
    String s = prefs.getString("ssid", "");
    if (s.length() > 0) {
      prefs.putString("ks0", s);
      prefs.putString("kp0", prefs.getString("pwd", ""));
      prefs.putUChar(kKeyKn, 1);
      LOTATO_DBG_LN("lotato cfg: migrated known WiFi from active ssid");
    }
  }
  prefs.putUChar(kKeyVer, 2);
  prefs.end();
}

void LotatoConfig::loadKnownWifi(Preferences& prefs) {
  _known_cnt = prefs.getUChar(kKeyKn, 0);
  if (_known_cnt > KNOWN_WIFI_MAX) _known_cnt = KNOWN_WIFI_MAX;
  for (uint8_t i = 0; i < KNOWN_WIFI_MAX; i++) {
    char key[8];
    snprintf(key, sizeof(key), "ks%u", (unsigned)i);
    _known_ssid[i][0] = '\0';
    memset(_known_pwd[i], 0, sizeof(_known_pwd[i]));
    if (i < _known_cnt) {
      prefs.getString(key, _known_ssid[i], sizeof(_known_ssid[i]));
      snprintf(key, sizeof(key), "kp%u", (unsigned)i);
      prefs.getString(key, _known_pwd[i], sizeof(_known_pwd[i]));
    }
  }
}

void LotatoConfig::load() {
  migrateFromBuildFlagsIfNeeded();
  migrateKnownProfilesIfNeeded();

  Preferences prefs;
  if (!prefs.begin(kNs, true)) { _loaded = true; return; }
  prefs.getString("ssid", _ssid, sizeof(_ssid));
  prefs.getString("pwd", _pwd, sizeof(_pwd));
  prefs.getString("url", _url, sizeof(_url));
  prefs.getString("token", _token, sizeof(_token));
  _debug = prefs.getBool("dbg", false);
  loadKnownWifi(prefs);
  prefs.end();
  _loaded = true;
}

bool LotatoConfig::getKnownWifi(uint8_t idx, char* out_ssid, size_t ssid_cap, char* out_pwd, size_t pwd_cap) const {
  if (idx >= _known_cnt || !out_ssid || ssid_cap < 1) return false;
  strncpy(out_ssid, _known_ssid[idx], ssid_cap - 1);
  out_ssid[ssid_cap - 1] = '\0';
  if (out_pwd && pwd_cap > 0) {
    strncpy(out_pwd, _known_pwd[idx], pwd_cap - 1);
    out_pwd[pwd_cap - 1] = '\0';
  }
  return true;
}

bool LotatoConfig::isKnownWifiSsid(const char* ssid) const {
  if (!ssid || !ssid[0]) return false;
  for (uint8_t i = 0; i < _known_cnt; i++) {
    if (strcmp(_known_ssid[i], ssid) == 0) return true;
  }
  return false;
}

bool LotatoConfig::getKnownWifiPassword(const char* ssid, char* out_pwd, size_t pwd_cap) const {
  if (!ssid || !ssid[0] || !out_pwd || pwd_cap < 1) return false;
  for (uint8_t i = 0; i < _known_cnt; i++) {
    if (strcmp(_known_ssid[i], ssid) == 0) {
      strncpy(out_pwd, _known_pwd[i], pwd_cap - 1);
      out_pwd[pwd_cap - 1] = '\0';
      return true;
    }
  }
  return false;
}

void LotatoConfig::persistKnownWifi() {
  Preferences prefs;
  if (!prefs.begin(kNs, false)) return;
  prefs.putUChar(kKeyKn, _known_cnt);
  for (uint8_t i = 0; i < KNOWN_WIFI_MAX; i++) {
    char key[8];
    snprintf(key, sizeof(key), "ks%u", (unsigned)i);
    prefs.putString(key, i < _known_cnt ? _known_ssid[i] : "");
    snprintf(key, sizeof(key), "kp%u", (unsigned)i);
    prefs.putString(key, i < _known_cnt ? _known_pwd[i] : "");
  }
  prefs.end();
}

void LotatoConfig::rememberWifi(const char* ssid, const char* pwd) {
  if (!ssid || !ssid[0]) return;
  const char* pp = pwd ? pwd : "";

  uint8_t w = 0;
  for (uint8_t r = 0; r < _known_cnt; r++) {
    if (strcmp(_known_ssid[r], ssid) == 0) continue;
    if (w != r) {
      strcpy(_known_ssid[w], _known_ssid[r]);
      strcpy(_known_pwd[w], _known_pwd[r]);
    }
    w++;
  }
  _known_cnt = w;

  if (_known_cnt < KNOWN_WIFI_MAX) _known_cnt++;
  for (int i = (int)_known_cnt - 1; i > 0; i--) {
    strcpy(_known_ssid[i], _known_ssid[i - 1]);
    strcpy(_known_pwd[i], _known_pwd[i - 1]);
  }
  strncpy(_known_ssid[0], ssid, sizeof(_known_ssid[0]) - 1);
  _known_ssid[0][sizeof(_known_ssid[0]) - 1] = '\0';
  strncpy(_known_pwd[0], pp, sizeof(_known_pwd[0]) - 1);
  _known_pwd[0][sizeof(_known_pwd[0]) - 1] = '\0';
  persistKnownWifi();
  LOTATO_DBG_LN("lotato cfg: wifi saved count=%u", (unsigned)_known_cnt);
}

bool LotatoConfig::isIngestReady() const {
  return _url[0] != '\0' && _token[0] != '\0';
}

static void put_string_pref(const char* key, const char* val) {
  Preferences prefs;
  if (!prefs.begin("lotato", false)) return;
  prefs.putString(key, val ? val : "");
  prefs.end();
}

bool LotatoConfig::forgetKnownWifi(const char* ssid) {
  if (!ssid || !ssid[0]) return false;
  bool found = false;
  uint8_t w = 0;
  for (uint8_t r = 0; r < _known_cnt; r++) {
    if (strcmp(_known_ssid[r], ssid) == 0) {
      found = true;
      memset(_known_pwd[r], 0, sizeof(_known_pwd[r]));
      continue;
    }
    if (w != r) {
      strcpy(_known_ssid[w], _known_ssid[r]);
      strcpy(_known_pwd[w], _known_pwd[r]);
    }
    w++;
  }
  if (!found) return false;
  _known_cnt = w;

  bool clear_active = (strcmp(_ssid, ssid) == 0);
  if (clear_active) {
    memset(_pwd, 0, sizeof(_pwd));
    _ssid[0] = '\0';
    put_string_pref("ssid", "");
    put_string_pref("pwd", "");
  }
  persistKnownWifi();
  LOTATO_DBG_LN("lotato cfg: forgot wifi ssid=\"%.32s\" active_cleared=%d", ssid, clear_active ? 1 : 0);
  return true;
}

void LotatoConfig::setWifi(const char* s, const char* p) {
  const char* ss = s ? s : "";
  const char* pp = p ? p : "";
  strncpy(_ssid, ss, sizeof(_ssid) - 1); _ssid[sizeof(_ssid) - 1] = '\0';
  strncpy(_pwd, pp, sizeof(_pwd) - 1);   _pwd[sizeof(_pwd) - 1] = '\0';
  put_string_pref("ssid", _ssid);
  put_string_pref("pwd", _pwd);
  rememberWifi(_ssid, _pwd);
}

void LotatoConfig::setApiToken(const char* t) {
  const char* tt = t ? t : "";
  strncpy(_token, tt, sizeof(_token) - 1); _token[sizeof(_token) - 1] = '\0';
  put_string_pref("token", _token);
}

void LotatoConfig::setIngestOrigin(const char* u) {
  const char* uu = u ? u : "";
  while (*uu && is_ws(*uu)) uu++;
  size_t len = strlen(uu);
  while (len > 0 && is_ws(uu[len - 1])) len--;
  if (len >= sizeof(_url)) len = sizeof(_url) - 1;
  memcpy(_url, uu, len);
  _url[len] = '\0';
  put_string_pref("url", _url);
  LOTATO_DBG_LN("lotato cfg: ingest origin set (len=%u)", (unsigned)strlen(_url));
}

void LotatoConfig::setDebug(bool on) {
  _debug = on;
  Preferences prefs;
  if (!prefs.begin("lotato", false)) return;
  prefs.putBool("dbg", _debug);
  prefs.end();
}

void LotatoConfig::toggleDebug() { setDebug(!debugEnabled()); }

#endif // ESP32
