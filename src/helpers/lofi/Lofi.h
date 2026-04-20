#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <helpers/lomessage/Buffer.h>
#include <lodb/LoDB.h>

#include "lofi.pb.h"

namespace lofi {

struct HttpResult {
  int status;
  int err;
};

/** Optional logger hook. Default no-op; apps that want logs override with a strong symbol. */
extern "C" __attribute__((weak)) void lofi_log(const char* msg);

/** Called by Lofi when an async operation (e.g. WiFi scan) starts/ends. Default no-op. */
extern "C" __attribute__((weak)) void lofi_async_busy(bool busy);

class Lofi {
public:
  static Lofi& instance();

  /** LoDB tables + STA event handlers. Idempotent. */
  void begin();

  /** Force public DNS (1.1.1.1/8.8.8.8) after STA GOT_IP. Default off. */
  void setForcePublicDns(bool on) { _force_public_dns = on; }

  // --- WiFi scanning ---
  void requestWifiScan();
  void serviceWifiScan();
  int scanSnapshotCount();
  bool scanSnapshotEntry(int idx, char ssid_out[33], int32_t* rssi_out);
  /** Multi-line "WiFi scan (N nets):\n1. <ssid> [bars]\n..." body builder. */
  void formatScanBody(lomessage::Buffer& buf);
  /** Optional callback invoked when an async scan completes (text reply, no ownership). */
  void setScanCompleteCallback(void (*fn)(void* ctx, const char* text), void* ctx);

  // --- Known / active WiFi (LoDB `known_wifi` + LoSettings `lofi.active.*`) ---
  /** Save active credentials and MRU-upsert into `known_wifi` (caps oldest beyond 8). */
  void saveWifiConnect(const char* ssid, const char* psk);
  uint8_t knownWifiCount();
  bool getKnownWifi(uint8_t idx, char* out_ssid, size_t ssid_cap, char* out_pwd, size_t pwd_cap);
  bool getKnownWifiPassword(const char* ssid, char* out_pwd, size_t pwd_cap);
  bool forgetKnownWifi(const char* ssid);
  /** WiFi.begin from LoSettings `lofi.active.ssid/psk`. */
  void resumeStaSavedCredentials();
  /** Get saved active SSID/PSK into callee buffers (empty strings if unset). */
  void getActiveCredentials(char* ssid_out, size_t ssid_cap, char* psk_out, size_t psk_cap);

  /** Suppress STA disconnect failover (e.g. during scan). */
  void staFailoverSuppress(bool suppress) { _failover_suppress = suppress; }

  // --- HTTP ---
  /**
   * Generic POST. `full_url` selects HTTPS (esp_http_client + CA bundle) or HTTP (HTTPClient session).
   * `bearer` optional; `extra_header_name` / `_val` optional single custom header (e.g. ngrok).
   */
  HttpResult httpPost(const char* full_url, const char* bearer, const char* body, uint16_t blen,
                      const char* extra_header_name = nullptr, const char* extra_header_val = nullptr);

  /** Drop plain-HTTP keep-alive session (call on URL/token/WiFi change). */
  void resetHttpTransport();

private:
  Lofi();
  void ensureTables();
  void registerWifiHandlers();
  void rememberKnownUnlocked(const char* ssid, const char* psk);

  LoDb _db;
  bool _tables_registered = false;
  bool _wifi_handlers_registered = false;
  bool _force_public_dns = false;
  bool _failover_suppress = false;
  void (*_scan_cb)(void*, const char*) = nullptr;
  void* _scan_cb_ctx = nullptr;
};

}  // namespace lofi

#endif
