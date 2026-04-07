#pragma once

#include <Arduino.h>

class Preferences;

/**
 * NVS-backed WiFi STA + potato-mesh HTTP ingest settings (ESP32).
 * Used when POTATO_MESH_INGEST is enabled.
 */
class PotatoMeshConfig {
public:
  static PotatoMeshConfig& instance();

  /** Load from NVS; on first run seeds from compile-time POTATO_MESH_* macros when defined. */
  void load();

  bool isIngestReady() const;
  bool debugEnabled() const { return _loaded ? _debug : false; }

  static constexpr uint8_t KNOWN_WIFI_MAX = 8;

  const char* ssid() const { return _ssid; }
  const char* password() const { return _pwd; }
  const char* ingestOrigin() const { return _url; }
  const char* apiToken() const { return _token; }

  /** MRU-ordered saved SSID/password pairs (index 0 = most recently used). */
  uint8_t knownWifiCount() const { return _known_cnt; }
  bool getKnownWifi(uint8_t idx, char* out_ssid, size_t ssid_cap, char* out_pwd, size_t pwd_cap) const;
  bool isKnownWifiSsid(const char* ssid) const;
  bool getKnownWifiPassword(const char* ssid, char* out_pwd, size_t pwd_cap) const;

  void setWifi(const char* s, const char* p);
  void setApiToken(const char* t);
  void setIngestOrigin(const char* u);
  void setDebug(bool on);
  void toggleDebug();

private:
  PotatoMeshConfig() : _loaded(false), _debug(false), _known_cnt(0) {
    _ssid[0] = _pwd[0] = _url[0] = _token[0] = '\0';
    memset(_known_ssid, 0, sizeof(_known_ssid));
    memset(_known_pwd, 0, sizeof(_known_pwd));
  }

  void migrateFromBuildFlagsIfNeeded();
  void migrateKnownProfilesIfNeeded();
  void loadKnownWifi(Preferences& prefs);
  void rememberWifi(const char* ssid, const char* pwd);
  void persistKnownWifi();

  bool _loaded;
  bool _debug;
  char _ssid[33];
  char _pwd[65];
  char _url[257];
  char _token[129];
  uint8_t _known_cnt;
  char _known_ssid[KNOWN_WIFI_MAX][33];
  char _known_pwd[KNOWN_WIFI_MAX][65];
};
