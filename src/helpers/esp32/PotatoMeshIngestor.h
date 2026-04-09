#pragma once

#ifdef ESP32

#include <Arduino.h>

class PotatoNodeStore;

/**
 * Register once at startup: optional STA DNS policy after GOT_IP.
 * Default: keep DHCP (router) DNS — works on most LANs.
 * Build with -DPOTATO_MESH_STA_FORCE_PUBLIC_DNS=1 to force 1.1.1.1 / 8.8.8.8 (old behavior).
 */
void potato_mesh_register_sta_dns_override();
/** Register once at startup: log STA connect/disconnect/IP events. */
void potato_mesh_register_wifi_event_logging();
/**
 * When two or more known WiFi profiles exist, disable ESP auto-reconnect and rotate to the next
 * saved SSID on hard join failures (e.g. reason 201 NO_AP_FOUND, auth/handshake failures).
 */
void potato_mesh_register_sta_known_wifi_failover();
/** While true, STA disconnect handler skips failover/reconnect (e.g. during `potato wifi scan`). */
void potato_mesh_sta_failover_suppress(bool suppress);

class PotatoMeshIngestor {
public:
  /**
   * Call every mesh loop with the node store and self public key so due nodes are batched into
   * one HTTP POST. Passing nullptr skips batch scheduling (only wakes worker if a batch is pending).
   */
  void service(PotatoNodeStore* node_store = nullptr, const uint8_t* self_pub_key = nullptr);
  /** Nodes in the current pending batch (0 if none). */
  uint8_t pendingQueueDepth() const;
  /** Drop pending batch and reset retry timer after URL/token/WiFi settings change. */
  void restartAfterConfigChange();
  void setPaused(bool paused);
  bool isPaused() const;
  /** Last HTTP response code from the worker task (0 = no attempt yet). */
  int lastHttpCode() const;
};

#endif // ESP32
