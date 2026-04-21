#pragma once

#ifdef ESP32

#include <Arduino.h>

class LotatoNodeStore;

/** Apply lotato STA policy (public DNS override if LOTATO_STA_FORCE_PUBLIC_DNS=1). */
void lotato_register_sta_dns_override();
/** No-op (STA event logging is now in lofi::Lofi); kept for call-site compat. */
void lotato_register_wifi_event_logging();
/** No-op (failover lives in lofi::Lofi); kept for call-site compat. */
void lotato_register_sta_known_wifi_failover();
/** Proxy to `lofi::Lofi::staFailoverSuppress` (scan vs connect gate). */
void lotato_sta_failover_suppress(bool suppress);
/** Drop pending ingest batch after LoSettings-backed ingest config changes. */
void lotato_ingest_restart_after_config();

class LotatoIngestor {
public:
  /**
   * Call every mesh loop with the node store and self public key so due nodes are batched into
   * one HTTP POST. Passing nullptr skips batch scheduling (only wakes worker if a batch is pending).
   */
  void service(LotatoNodeStore* node_store = nullptr, const uint8_t* self_pub_key = nullptr);
  /** Nodes in the current pending batch (0 if none). */
  uint8_t pendingQueueDepth() const;
  /** Drop pending batch and reset retry timer after URL/token/WiFi settings change. */
  void restartAfterConfigChange();
  void setPaused(bool paused);
  bool isPaused() const;
  /** Last HTTP response code (0 = no attempt yet). */
  int lastHttpCode() const;
};

#endif  // ESP32
