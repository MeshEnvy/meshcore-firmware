#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <helpers/ContactInfo.h>

/**
 * Register once at startup: optional STA DNS policy after GOT_IP.
 * Default: keep DHCP (router) DNS — works on most LANs.
 * Build with -DPOTATO_MESH_STA_FORCE_PUBLIC_DNS=1 to force 1.1.1.1 / 8.8.8.8 (old behavior).
 */
void potato_mesh_register_sta_dns_override();
/** Register once at startup: log STA connect/disconnect/IP events. */
void potato_mesh_register_wifi_event_logging();

class PotatoMeshIngestor {
public:
  /** Enqueue contact for HTTP POST. Call from onAdvertRecv after dedupe. */
  void postContactDiscovered(const uint8_t self_pub_key[PUB_KEY_SIZE], const ContactInfo& contact);
  /** Call every loop: wakes the FreeRTOS worker when WiFi is up and the queue is non-empty. */
  void service();
  /** Pending HTTP ingest payloads (0 .. queue depth). */
  uint8_t pendingQueueDepth() const;
  /** Drop queued POSTs and reset retry timer after URL/token/WiFi settings change. */
  void restartAfterConfigChange();
  void setPaused(bool paused);
  bool isPaused() const;
  /** Last HTTP response code from the worker task (0 = no attempt yet). */
  int lastHttpCode() const;
};

#endif // ESP32
