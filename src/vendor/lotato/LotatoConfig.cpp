#include "LotatoConfig.h"

#ifdef ESP32

#include <cstring>

#include <lolog/LoLog.h>
#include <LotatoIngestor.h>
#include <lofi/Lofi.h>
#include <losettings/ConfigHub.h>
#include <losettings/LoSettings.h>

namespace {

void lotato_on_cfg_changed(void*) {
  LotatoConfig::instance().refreshFromLoSettings();
  lotato_ingest_restart_after_config();
}

constexpr uint32_t kVisMax = 30u * 86400u;
constexpr uint32_t kGcMax  = 60u * 86400u;

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

void LotatoConfig::refreshFromLoSettings() {
  losettings::LoSettings lt("lotato");
  losettings::LoSettings wf("lofi");
  lt.getString("ingest.url", _url, sizeof(_url), "");
  lt.getString("ingest.token", _token, sizeof(_token), "");
  _ingest_paused      = lt.getBool("ingest.paused", false);
  _ingest_visibility_secs = lt.getUInt("ingest.visibility_secs", 259200u);
  _ingest_refresh_secs    = lt.getUInt("ingest.refresh_secs", 900u);
  _ingest_gc_stale_secs   = lt.getUInt("ingest.gc_stale_secs", 259200u);
  wf.getString("active.ssid", _ssid, sizeof(_ssid), "");
  wf.getString("active.psk", _pwd, sizeof(_pwd), "");
}

void LotatoConfig::registerConfigSchema() {
  static bool registered = false;
  if (registered) return;
  registered = true;

  static const losettings::ConfigEntry kLotato[] = {
      {"ingest.url",
       losettings::ConfigValueKind::String,
       false,
       0,
       0,
       "",
       "Potato Mesh ingest origin URL (max 256 chars)",
       false,
       false,
       0,
       0,
       lotato_on_cfg_changed,
       nullptr},
      {"ingest.token",
       losettings::ConfigValueKind::String,
       false,
       0,
       0,
       "",
       "Potato Mesh API bearer token (max 128 chars)",
       true,
       false,
       0,
       0,
       lotato_on_cfg_changed,
       nullptr},
      {"ingest.paused",
       losettings::ConfigValueKind::Bool,
       false,
       0,
       0,
       nullptr,
       "When true, stops POSTing node batches to ingest",
       false,
       false,
       0,
       0,
       lotato_on_cfg_changed,
       nullptr},
      {"ingest.visibility_secs",
       losettings::ConfigValueKind::UInt32,
       false,
       0,
       259200u,
       nullptr,
       "Mesh-heard visibility window for ingest (seconds)",
       false,
       true,
       300u,
       kVisMax,
       lotato_on_cfg_changed,
       nullptr},
      {"ingest.refresh_secs",
       losettings::ConfigValueKind::UInt32,
       false,
       0,
       900u,
       nullptr,
       "Minimum seconds between successful ingest POSTs per visible node",
       false,
       true,
       60u,
       86400u,
       lotato_on_cfg_changed,
       nullptr},
      {"ingest.gc_stale_secs",
       losettings::ConfigValueKind::UInt32,
       false,
       0,
       259200u,
       nullptr,
       "Remove store slots not mesh-heard for this long (>= visibility_secs)",
       false,
       true,
       300u,
       kGcMax,
       lotato_on_cfg_changed,
       nullptr},
  };
  static const losettings::ConfigRegistry kReg("lotato", kLotato, (int)(sizeof(kLotato) / sizeof(kLotato[0])));
  losettings::ConfigHub::instance().registerModule(kReg);
}

void LotatoConfig::load() {
  seedBuildFlagsIntoLoSettingsIfNeeded();
  registerConfigSchema();
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
  ::lolog::LoLog::debug("lotato", "lotato cfg: forgot wifi ssid=\"%.32s\"", ssid);
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
  ::lolog::LoLog::debug("lotato", "lotato cfg: ingest origin set (len=%u)", (unsigned)strlen(_url));
}

extern "C" void lofi_on_lo_settings_changed(void) {
  LotatoConfig::instance().refreshFromLoSettings();
}

#endif  // ESP32
