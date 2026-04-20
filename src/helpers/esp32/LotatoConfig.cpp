#include "LotatoConfig.h"

#ifdef ESP32

#include <Preferences.h>
#include <cstring>

#include <helpers/esp32/LotatoDebug.h>
#include <helpers/lofi/Lofi.h>
#include <lofi.pb.h>
#include <lodb/LoDB.h>
#include <losettings/LoSettings.h>

namespace {

constexpr char kNs[] = "lotato";

}  // namespace

LotatoConfig& LotatoConfig::instance() {
  static LotatoConfig inst;
  return inst;
}

void LotatoConfig::seedBuildFlagsIntoLoSettingsIfNeeded() {
  losettings::LoSettings st("lotato");
#if defined(LOTATO_INGEST_URL)
  if (!st.has("ingest.url")) st.setString("ingest.url", LOTATO_INGEST_URL);
#endif
#if defined(LOTATO_API_TOKEN)
  if (!st.has("ingest.token")) st.setString("ingest.token", LOTATO_API_TOKEN);
#endif
#if defined(LOTATO_WIFI_SSID)
  {
    losettings::LoSettings wf("lofi");
    if (!wf.has("active.ssid")) wf.setString("active.ssid", LOTATO_WIFI_SSID);
#if defined(LOTATO_WIFI_PWD)
    if (!wf.has("active.psk")) wf.setString("active.psk", LOTATO_WIFI_PWD);
#endif
  }
#endif
}

void LotatoConfig::migrateLegacyNvsToLoIfNeeded() {
  losettings::LoSettings st("lotato");
  if (st.has("migrated.v1")) return;

  Preferences prefs;
  if (!prefs.begin(kNs, true)) {
    (void)st.setBool("migrated.v1", true);
    return;
  }

  String url = prefs.getString("url", "");
  String token = prefs.getString("token", "");
  bool dbg = prefs.getBool("dbg", false);
  String ssid = prefs.getString("ssid", "");
  String pwd = prefs.getString("pwd", "");

  if (url.length()) st.setString("ingest.url", url.c_str());
  if (token.length()) st.setString("ingest.token", token.c_str());
  st.setBool("debug", dbg);

  {
    losettings::LoSettings wf("lofi");
    if (ssid.length()) {
      wf.setString("active.ssid", ssid.c_str());
      wf.setString("active.psk", pwd.c_str());
    }
  }

  uint8_t kn = prefs.getUChar("kn", 0);
  if (kn > 8) kn = 8;
  LoDb db("lofi");
  (void)db.registerTable("known_wifi", &KnownWifi_msg, sizeof(KnownWifi));
  for (uint8_t i = 0; i < kn; i++) {
    char ks[8], kp[8];
    snprintf(ks, sizeof(ks), "ks%u", (unsigned)i);
    snprintf(kp, sizeof(kp), "kp%u", (unsigned)i);
    String s = prefs.getString(ks, "");
    if (s.length() == 0) continue;
    String p = prefs.getString(kp, "");
    KnownWifi row = KnownWifi_init_zero;
    strncpy(row.ssid, s.c_str(), sizeof(row.ssid) - 1);
    strncpy(row.psk, p.c_str(), sizeof(row.psk) - 1);
    row.last_connected = (uint32_t)(millis() / 1000);
    row.connect_count = 1;
    lodb_uuid_t id = lodb_new_uuid(row.ssid, 0);
    (void)db.insert("known_wifi", id, &row);
  }
  if (kn == 0 && ssid.length()) {
    KnownWifi row = KnownWifi_init_zero;
    strncpy(row.ssid, ssid.c_str(), sizeof(row.ssid) - 1);
    strncpy(row.psk, pwd.c_str(), sizeof(row.psk) - 1);
    row.last_connected = (uint32_t)(millis() / 1000);
    row.connect_count = 1;
    lodb_uuid_t id = lodb_new_uuid(row.ssid, 0);
    (void)db.insert("known_wifi", id, &row);
  }

  prefs.end();
  {
    Preferences pw;
    if (pw.begin(kNs, false)) {
      pw.clear();
      pw.end();
    }
  }
  (void)st.setBool("migrated.v1", true);
  LOTATO_DBG_LN("lotato cfg: migrated legacy NVS -> LoSettings/LoDB");
}

void LotatoConfig::refreshFromLoSettings() {
  losettings::LoSettings lt("lotato");
  losettings::LoSettings wf("lofi");
  lt.getString("ingest.url", _url, sizeof(_url), "");
  lt.getString("ingest.token", _token, sizeof(_token), "");
  _debug = lt.getBool("debug", false);
  wf.getString("active.ssid", _ssid, sizeof(_ssid), "");
  wf.getString("active.psk", _pwd, sizeof(_pwd), "");
}

void LotatoConfig::load() {
  seedBuildFlagsIntoLoSettingsIfNeeded();
  migrateLegacyNvsToLoIfNeeded();
  refreshFromLoSettings();
  _loaded = true;
}

bool LotatoConfig::isIngestReady() const { return _url[0] != '\0' && _token[0] != '\0'; }

uint8_t LotatoConfig::knownWifiCount() { return lofi::Lofi::instance().knownWifiCount(); }

bool LotatoConfig::getKnownWifi(uint8_t idx, char* out_ssid, size_t ssid_cap, char* out_pwd, size_t pwd_cap) {
  return lofi::Lofi::instance().getKnownWifi(idx, out_ssid, ssid_cap, out_pwd, pwd_cap);
}

bool LotatoConfig::isKnownWifiSsid(const char* ssid) {
  if (!ssid || !ssid[0]) return false;
  char tmp[65];
  return getKnownWifiPassword(ssid, tmp, sizeof(tmp));
}

bool LotatoConfig::getKnownWifiPassword(const char* ssid, char* out_pwd, size_t pwd_cap) {
  return lofi::Lofi::instance().getKnownWifiPassword(ssid, out_pwd, pwd_cap);
}

void LotatoConfig::setWifi(const char* s, const char* p) {
  lofi::Lofi::instance().saveWifiConnect(s ? s : "", p ? p : "");
  refreshFromLoSettings();
}

bool LotatoConfig::forgetKnownWifi(const char* ssid) {
  bool ok = lofi::Lofi::instance().forgetKnownWifi(ssid);
  if (!ok) return false;
  refreshFromLoSettings();
  LOTATO_DBG_LN("lotato cfg: forgot wifi ssid=\"%.32s\"", ssid);
  return true;
}

void LotatoConfig::setApiToken(const char* t) {
  losettings::LoSettings st("lotato");
  st.setString("ingest.token", t ? t : "");
  refreshFromLoSettings();
}

void LotatoConfig::setIngestOrigin(const char* u) {
  const char* uu = u ? u : "";
  while (*uu == ' ' || *uu == '\t' || *uu == '\r' || *uu == '\n') uu++;
  size_t len = strlen(uu);
  while (len > 0 && (uu[len - 1] == ' ' || uu[len - 1] == '\t' || uu[len - 1] == '\r' || uu[len - 1] == '\n'))
    len--;
  char buf[257];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, uu, len);
  buf[len] = '\0';
  losettings::LoSettings st("lotato");
  st.setString("ingest.url", buf);
  refreshFromLoSettings();
  LOTATO_DBG_LN("lotato cfg: ingest origin set (len=%u)", (unsigned)strlen(_url));
}

void LotatoConfig::setDebug(bool on) {
  _debug = on;
  losettings::LoSettings st("lotato");
  st.setBool("debug", on);
}

void LotatoConfig::toggleDebug() { setDebug(!debugEnabled()); }

#endif  // ESP32
