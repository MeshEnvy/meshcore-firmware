#pragma once

#include <Arduino.h>

/**
 * Lotato ingest + WiFi STA settings (ESP32).
 * Scalars live in LoSettings; known WiFi list in LoDB via `lofi::Lofi`.
 */
class LotatoConfig {
public:
  static LotatoConfig& instance();

  /** Seed compile-time defaults into LoSettings if missing; then refresh caches from LoSettings. */
  void load();

  /** Re-read LoSettings into RAM caches (after Lofi updates active WiFi, etc.). */
  void refreshFromLoSettings();

  bool isIngestReady() const;
  bool debugEnabled() const { return _loaded ? _debug : false; }

  const char* ssid() const { return _ssid; }
  const char* password() const { return _pwd; }
  const char* ingestOrigin() const { return _url; }
  const char* apiToken() const { return _token; }

  uint8_t knownWifiCount();
  bool getKnownWifi(uint8_t idx, char* out_ssid, size_t ssid_cap, char* out_pwd, size_t pwd_cap);
  bool isKnownWifiSsid(const char* ssid);
  bool getKnownWifiPassword(const char* ssid, char* out_pwd, size_t pwd_cap);

  void setWifi(const char* s, const char* p);
  bool forgetKnownWifi(const char* ssid);
  void setApiToken(const char* t);
  void setIngestOrigin(const char* u);
  void setDebug(bool on);
  void toggleDebug();

private:
  LotatoConfig() : _loaded(false), _debug(false) {
    _ssid[0] = _pwd[0] = _url[0] = _token[0] = '\0';
  }

  void seedBuildFlagsIntoLoSettingsIfNeeded();

  bool _loaded;
  bool _debug;
  char _ssid[33];
  char _pwd[65];
  char _url[257];
  char _token[129];
};
