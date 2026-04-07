#pragma once

#include <Arduino.h>
#include <helpers/esp32/PotatoMeshDebug.h>
#include <helpers/ContactInfo.h>

/**
 * POST discovered mesh nodes to potato-mesh (or compatible) HTTP API.
 *
 * Built when ESP32 and POTATO_MESH_INGEST (see root platformio.ini). Posts only
 * while WiFi station is up; credentials are POTATO_MESH_WIFI_* (STA for HTTP only,
 * separate from companion BLE/USB serial).
 *
 * POTATO_MESH_INGEST_URL: origin only, e.g. http://192.168.1.10:41447 or
 * https://xyz.ngrok-free.app — path is POTATO_MESH_INGEST_API_PATH (default /api/nodes).
 */
class PotatoMeshIngestor {
public:
  void postContactDiscovered(const uint8_t self_pub_key[PUB_KEY_SIZE], const ContactInfo& contact);
  /** Enqueue one contact if ingest is ready and queue has space. Returns false if full or ingest not configured yet (retry later). */
  bool tryEnqueueContact(const uint8_t self_pub_key[PUB_KEY_SIZE], const ContactInfo& contact);
  /** Call from main loop: pokes the background worker when WiFi is up and the queue is non-empty (POST runs on a FreeRTOS task). */
  void service();
  /** Pending HTTP ingest payloads (0 .. POTATO_MESH_INGEST_QUEUE_DEPTH). */
  uint8_t pendingQueueDepth() const;
  /** Drop queued POSTs and reset retry timer after URL/token/WiFi settings change. */
  void restartAfterConfigChange();
  /** When true, no new payloads are queued and service() does not POST (queue retained). */
  void setPaused(bool paused);
  bool isPaused() const;
};
