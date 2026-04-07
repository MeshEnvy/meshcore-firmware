#pragma once

#include <Arduino.h>

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

  const char* ssid() const { return _ssid; }
  const char* password() const { return _pwd; }
  const char* ingestOrigin() const { return _url; }
  const char* apiToken() const { return _token; }

  void setWifi(const char* s, const char* p);
  void setApiToken(const char* t);
  void setIngestOrigin(const char* u);
  void setDebug(bool on);
  void toggleDebug();

private:
  PotatoMeshConfig() : _loaded(false), _debug(false) {
    _ssid[0] = _pwd[0] = _url[0] = _token[0] = '\0';
  }

  void migrateFromBuildFlagsIfNeeded();

  bool _loaded;
  bool _debug;
  char _ssid[33];
  char _pwd[65];
  char _url[257];
  char _token[129];
};
