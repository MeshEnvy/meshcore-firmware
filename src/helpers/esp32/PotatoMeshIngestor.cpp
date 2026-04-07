#include "PotatoMeshIngestor.h"

#if defined(ESP32) && defined(POTATO_MESH_INGEST)

#include <helpers/AdvertDataHelpers.h>
#include <helpers/esp32/PotatoMeshConfig.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>

/* Origin only (scheme + host[:port]); path is appended via POTATO_MESH_INGEST_API_PATH. */
#ifndef POTATO_MESH_INGEST_URL
#define POTATO_MESH_INGEST_URL "http://127.0.0.1:41447"
#endif
#ifndef POTATO_MESH_INGEST_API_PATH
#define POTATO_MESH_INGEST_API_PATH "/api/nodes"
#endif

#ifndef POTATO_MESH_API_TOKEN
#define POTATO_MESH_API_TOKEN "dev"
#endif

#ifndef POTATO_MESH_HTTP_RETRY_DELAY_MS
#define POTATO_MESH_HTTP_RETRY_DELAY_MS 400
#endif
/** Overall HTTP read/transfer timeout (ms). POST runs on a dedicated task, not the companion loop. */
#ifndef POTATO_MESH_HTTP_TIMEOUT_MS
#define POTATO_MESH_HTTP_TIMEOUT_MS 12000
#endif
#ifndef POTATO_MESH_HTTP_CONNECT_TIMEOUT_MS
#define POTATO_MESH_HTTP_CONNECT_TIMEOUT_MS 8000
#endif
#ifndef POTATO_MESH_INGEST_WORKER_STACK
#define POTATO_MESH_INGEST_WORKER_STACK 12288
#endif
#ifndef POTATO_MESH_INGEST_QUEUE_DEPTH
#define POTATO_MESH_INGEST_QUEUE_DEPTH 8
#endif

namespace {

constexpr size_t kBodyCap = 1800;
constexpr size_t kQueueDepth = POTATO_MESH_INGEST_QUEUE_DEPTH;

struct IngestQueue {
  char body[kQueueDepth][kBodyCap];
  uint16_t len[kQueueDepth];
  char node_id[kQueueDepth][12];
  uint8_t head;
  uint8_t tail;
  uint8_t count;
  uint32_t next_retry_ms;
};

IngestQueue g_q{};
bool g_paused = false;
SemaphoreHandle_t g_q_mtx = nullptr;
TaskHandle_t g_worker = nullptr;
portMUX_TYPE g_worker_init = portMUX_INITIALIZER_UNLOCKED;

void notify_worker();
void ensure_worker();

bool is_https_origin(const char* base) { return strncmp(base, "https://", 8) == 0; }

/** Long-lived HTTP + TCP/TLS: avoids HTTPClient destructor calling stop() every POST, enables keep-alive. */
static WiFiClientSecure g_ingest_tls;
static WiFiClient g_ingest_plain;
static HTTPClient g_ingest_http;
static bool g_ingest_tls_insecure = false;

struct IngestHttpSession {
  char full_url[256];
  char origin[257];
  char token[129];
  char ssid[33];
  uint32_t ip4;
  bool active;
};

static IngestHttpSession g_ingest_sess{};

static void reset_ingest_http_session() {
  g_ingest_http.setReuse(false);
  g_ingest_http.end();
  g_ingest_tls.stop();
  g_ingest_plain.stop();
  g_ingest_sess.active = false;
  g_ingest_sess.full_url[0] = '\0';
}

static bool ingest_http_session_matches(const char* full_url) {
  if (!g_ingest_sess.active) {
    return false;
  }
  PotatoMeshConfig& cfg = PotatoMeshConfig::instance();
  if (strcmp(g_ingest_sess.full_url, full_url) != 0 || strcmp(g_ingest_sess.origin, cfg.ingestOrigin()) != 0 ||
      strcmp(g_ingest_sess.token, cfg.apiToken()) != 0 || strcmp(g_ingest_sess.ssid, cfg.ssid()) != 0) {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  return (uint32_t)WiFi.localIP() == g_ingest_sess.ip4;
}

static void capture_ingest_http_session(const char* full_url) {
  PotatoMeshConfig& cfg = PotatoMeshConfig::instance();
  strncpy(g_ingest_sess.full_url, full_url, sizeof(g_ingest_sess.full_url) - 1);
  g_ingest_sess.full_url[sizeof(g_ingest_sess.full_url) - 1] = '\0';
  strncpy(g_ingest_sess.origin, cfg.ingestOrigin(), sizeof(g_ingest_sess.origin) - 1);
  g_ingest_sess.origin[sizeof(g_ingest_sess.origin) - 1] = '\0';
  strncpy(g_ingest_sess.token, cfg.apiToken(), sizeof(g_ingest_sess.token) - 1);
  g_ingest_sess.token[sizeof(g_ingest_sess.token) - 1] = '\0';
  strncpy(g_ingest_sess.ssid, cfg.ssid(), sizeof(g_ingest_sess.ssid) - 1);
  g_ingest_sess.ssid[sizeof(g_ingest_sess.ssid) - 1] = '\0';
  g_ingest_sess.ip4 = (uint32_t)WiFi.localIP();
  g_ingest_sess.active = true;
}

/** Join ingest origin (NVS or build default) + POTATO_MESH_INGEST_API_PATH. */
bool build_ingest_post_url(char* full, size_t cap) {
  const char* base = PotatoMeshConfig::instance().ingestOrigin();
  if (!base || base[0] == '\0') {
    return false;
  }
  const char* path = POTATO_MESH_INGEST_API_PATH;
  if (!path || path[0] != '/') {
    return false;
  }
  size_t blen = strlen(base);
  while (blen > 0 && base[blen - 1] == '/') {
    blen--;
  }
  int n = snprintf(full, cap, "%.*s%s", (int)blen, base, path);
  return n > 0 && (size_t)n < cap;
}

bool try_post_once(const char node_id[12], const char* body, uint16_t n) {
  char full_url[256];
  if (!build_ingest_post_url(full_url, sizeof(full_url))) {
    POTATO_MESH_DBG_LN("post %s: ingest URL/path build failed", node_id);
    return false;
  }

  if (!ingest_http_session_matches(full_url)) {
    reset_ingest_http_session();
  }

  g_ingest_http.setReuse(true);
  g_ingest_http.setConnectTimeout((int32_t)POTATO_MESH_HTTP_CONNECT_TIMEOUT_MS);
  g_ingest_http.setTimeout((uint32_t)POTATO_MESH_HTTP_TIMEOUT_MS);

  const char* origin = PotatoMeshConfig::instance().ingestOrigin();
  bool ok_begin = false;
  if (g_ingest_sess.active) {
    ok_begin = true;
  } else if (is_https_origin(origin)) {
    if (!g_ingest_tls_insecure) {
      g_ingest_tls.setInsecure();
      g_ingest_tls_insecure = true;
    }
    ok_begin = g_ingest_http.begin(g_ingest_tls, full_url);
  } else {
    ok_begin = g_ingest_http.begin(g_ingest_plain, full_url);
  }

  if (!ok_begin) {
    POTATO_MESH_DBG_LN("post %s: http.begin() failed URL=%s", node_id, full_url);
    reset_ingest_http_session();
    return false;
  }

  if (!g_ingest_sess.active) {
    capture_ingest_http_session(full_url);
  }

  if (strstr(origin, "ngrok") != nullptr) {
    g_ingest_http.addHeader("ngrok-skip-browser-warning", "true");
  }
  g_ingest_http.addHeader("Content-Type", "application/json");
  char auth_hdr[200];
  snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", PotatoMeshConfig::instance().apiToken());
  g_ingest_http.addHeader("Authorization", auth_hdr);

  POTATO_MESH_DBG_LN("post %s: heap=%u max_blk=%u POST %u bytes to %s", node_id, (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getMaxAllocHeap(), (unsigned)n, full_url);

  int code = g_ingest_http.POST((uint8_t*)body, (size_t)n);

  if (code >= 200 && code < 300) {
    POTATO_MESH_DBG_LN("post %s: HTTP %d ok", node_id, code);
    /* Must drain the body for keep-alive; do not call end() while session stays valid. */
    (void)g_ingest_http.getString();
    return true;
  }

  /* One read: works for HTTP error responses; sometimes non-empty even when code < 0 (proxy/ngrok body). */
  String resp = g_ingest_http.getString();
  constexpr unsigned kRespLogMax = 512;
  if (resp.length() > 0) {
    unsigned plen = resp.length() > kRespLogMax ? kRespLogMax : resp.length();
    POTATO_MESH_DBG_LN("post %s: response body (%u bytes, showing %u): %.*s", node_id, (unsigned)resp.length(), plen,
                        (int)plen, resp.c_str());
  }
  if (code < 0) {
    POTATO_MESH_DBG_LN("post %s: transport error %d (%s)", node_id, code, g_ingest_http.errorToString(code).c_str());
  } else {
    POTATO_MESH_DBG_LN("post %s: HTTP %d (will retry)", node_id, code);
  }
  /* Drop connection after errors or auth failure so the next attempt re-syncs TLS / credentials. */
  if (code < 0 || code == 401) {
    reset_ingest_http_session();
  }
  return false;
}

void enqueue_pending(const char node_id[12], const char* body, uint16_t n) {
  if (n >= kBodyCap) {
    POTATO_MESH_DBG_LN("post %s: payload too large for queue (%u)", node_id, (unsigned)n);
    return;
  }
  ensure_worker();
  if (!g_q_mtx) {
    return;
  }
  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  if (g_q.count >= kQueueDepth) {
    g_q.head = (uint8_t)((g_q.head + 1) % kQueueDepth);
    g_q.count--;
    POTATO_MESH_DBG_LN("ingest queue full, dropped oldest pending");
  }
  uint8_t slot = g_q.tail;
  memcpy(g_q.body[slot], body, n);
  g_q.body[slot][n] = '\0';
  g_q.len[slot] = n;
  snprintf(g_q.node_id[slot], sizeof(g_q.node_id[slot]), "%s", node_id);
  g_q.tail = (uint8_t)((g_q.tail + 1) % kQueueDepth);
  g_q.count++;
  POTATO_MESH_DBG_LN("post %s: queued (%u in queue)", node_id, (unsigned)g_q.count);
  xSemaphoreGive(g_q_mtx);
  notify_worker();
}

bool enqueue_pending_try(const char node_id[12], const char* body, uint16_t n) {
  if (n >= kBodyCap) {
    return true;
  }
  ensure_worker();
  if (!g_q_mtx) {
    return false;
  }
  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  if (g_q.count >= kQueueDepth) {
    xSemaphoreGive(g_q_mtx);
    return false;
  }
  uint8_t slot = g_q.tail;
  memcpy(g_q.body[slot], body, n);
  g_q.body[slot][n] = '\0';
  g_q.len[slot] = n;
  snprintf(g_q.node_id[slot], sizeof(g_q.node_id[slot]), "%s", node_id);
  g_q.tail = (uint8_t)((g_q.tail + 1) % kQueueDepth);
  g_q.count++;
  xSemaphoreGive(g_q_mtx);
  notify_worker();
  return true;
}

uint8_t potato_ingest_queue_depth() {
  if (!g_q_mtx) {
    return g_q.count;
  }
  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  uint8_t c = g_q.count;
  xSemaphoreGive(g_q_mtx);
  return c;
}

void potato_ingest_clear_queue() {
  reset_ingest_http_session();
  if (!g_q_mtx) {
    g_q.head = g_q.tail = g_q.count = 0;
    g_q.next_retry_ms = 0;
    POTATO_MESH_DBG_LN("ingest queue cleared (config change)");
    return;
  }
  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  g_q.head = g_q.tail = g_q.count = 0;
  g_q.next_retry_ms = 0;
  xSemaphoreGive(g_q_mtx);
  POTATO_MESH_DBG_LN("ingest queue cleared (config change)");
  notify_worker();
}

bool ingest_try_step() {
  char local_body[kBodyCap];
  char local_nid[12];
  uint16_t plen = 0;

  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  if (g_paused) {
    xSemaphoreGive(g_q_mtx);
    return false;
  }
  if (!PotatoMeshConfig::instance().isIngestReady()) {
    xSemaphoreGive(g_q_mtx);
    return false;
  }
  if (g_q.count == 0) {
    xSemaphoreGive(g_q_mtx);
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    xSemaphoreGive(g_q_mtx);
    vTaskDelay(pdMS_TO_TICKS(200));
    return true;
  }
  uint32_t now = millis();
  if ((int32_t)(now - g_q.next_retry_ms) < 0) {
    uint32_t wait = g_q.next_retry_ms - now;
    xSemaphoreGive(g_q_mtx);
    if (wait > 0) {
      vTaskDelay(pdMS_TO_TICKS(wait));
    }
    return true;
  }

  plen = g_q.len[g_q.head];
  memcpy(local_body, g_q.body[g_q.head], plen);
  local_body[plen] = '\0';
  memcpy(local_nid, g_q.node_id[g_q.head], sizeof(local_nid));

  xSemaphoreGive(g_q_mtx);

  bool ok = try_post_once(local_nid, local_body, plen);

  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  if (g_q.count > 0 && g_q.len[g_q.head] == plen && memcmp(g_q.node_id[g_q.head], local_nid, 12) == 0) {
    if (ok) {
      g_q.head = (uint8_t)((g_q.head + 1) % kQueueDepth);
      g_q.count--;
      g_q.next_retry_ms = 0;
    } else {
      g_q.next_retry_ms = millis() + (uint32_t)POTATO_MESH_HTTP_RETRY_DELAY_MS;
    }
  }
  bool more = g_q.count > 0 && !g_paused;
  xSemaphoreGive(g_q_mtx);
  return more;
}

void ingest_worker_entry(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (ingest_try_step()) {
    }
  }
}

void notify_worker() {
  if (g_worker) {
    xTaskNotifyGive(g_worker);
  }
}

void ensure_worker() {
  if (g_worker) {
    return;
  }
  if (!g_q_mtx) {
    g_q_mtx = xSemaphoreCreateMutex();
  }
  if (!g_q_mtx) {
    return;
  }

  TaskHandle_t created = nullptr;
  if (xTaskCreate(ingest_worker_entry, "potato-ingest", POTATO_MESH_INGEST_WORKER_STACK, nullptr, 1, &created) !=
      pdPASS) {
    POTATO_MESH_DBG_LN("ingest worker xTaskCreate failed");
    return;
  }

  portENTER_CRITICAL(&g_worker_init);
  if (!g_worker) {
    g_worker = created;
    portEXIT_CRITICAL(&g_worker_init);
  } else {
    portEXIT_CRITICAL(&g_worker_init);
    vTaskDelete(created);
  }
}

} // namespace

static void bin_to_hex_lower(const uint8_t* b, size_t n, char* out) {
  static const char* hexd = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hexd[b[i] >> 4];
    out[i * 2 + 1] = hexd[b[i] & 0x0f];
  }
  out[n * 2] = '\0';
}

static const char* adv_type_to_role(uint8_t adv_type) {
  switch (adv_type) {
    case ADV_TYPE_CHAT:
      return "COMPANION";
    case ADV_TYPE_REPEATER:
      return "REPEATER";
    case ADV_TYPE_ROOM:
      return "ROOM_SERVER";
    case ADV_TYPE_SENSOR:
      return "SENSOR";
    default:
      return NULL;
  }
}

static void format_node_id(char out[12], const uint8_t pub_key[PUB_KEY_SIZE]) {
  out[0] = '!';
  bin_to_hex_lower(pub_key, 4, out + 1);
}

static void format_pub_key_hex(char out[65], const uint8_t pub_key[PUB_KEY_SIZE]) {
  bin_to_hex_lower(pub_key, PUB_KEY_SIZE, out);
}

static bool append_json_escaped_name(char* dest, size_t dest_size, const char* name) {
  if (dest_size < 3)
    return false;
  char* p = dest;
  char* end = dest + dest_size - 1;
  *p++ = '"';
  while (name && *name && p < end - 1) {
    unsigned char c = (unsigned char)*name++;
    if (c == '"' || c == '\\') {
      if (p >= end - 2)
        break;
      *p++ = '\\';
      *p++ = (char)c;
    } else if (c >= 32 && c < 127) {
      *p++ = (char)c;
    } else {
      *p++ = '?';
    }
  }
  if (p >= end)
    return false;
  *p++ = '"';
  *p = '\0';
  return true;
}

static bool build_contact_ingest_json(const uint8_t self_pub_key[PUB_KEY_SIZE], const ContactInfo& contact,
                                      char* body, size_t body_cap, uint16_t* out_len, char node_id[12]) {
  format_node_id(node_id, contact.id.pub_key);
  char ingestor_id[12];
  char pub_hex[65];
  char name_json[40];
  format_node_id(ingestor_id, self_pub_key);
  format_pub_key_hex(pub_hex, contact.id.pub_key);
  if (!append_json_escaped_name(name_json, sizeof(name_json), contact.name)) {
    strncpy(name_json, "\"?\"", sizeof(name_json));
    name_json[sizeof(name_json) - 1] = '\0';
  }

  char short_hex[5];
  bin_to_hex_lower(contact.id.pub_key, 2, short_hex);

  uint32_t num = ((uint32_t)contact.id.pub_key[0] << 24) | ((uint32_t)contact.id.pub_key[1] << 16) |
                 ((uint32_t)contact.id.pub_key[2] << 8) | (uint32_t)contact.id.pub_key[3];

  uint32_t last_heard = contact.last_advert_timestamp;
  if (last_heard == 0) {
    last_heard = (uint32_t)(millis() / 1000);
  }

  const char* role = adv_type_to_role(contact.type);
  int n;

  if (contact.gps_lat != 0 || contact.gps_lon != 0) {
    double lat = (double)contact.gps_lat / 1000000.0;
    double lon = (double)contact.gps_lon / 1000000.0;
    if (role) {
      n = snprintf(body, body_cap,
                    "{"
                    "\"%s\":{"
                    "\"num\":%lu,"
                    "\"lastHeard\":%lu,"
                    "\"protocol\":\"meshcore\","
                    "\"user\":{"
                    "\"longName\":%s,"
                    "\"shortName\":\"%s\","
                    "\"publicKey\":\"%s\","
                    "\"role\":\"%s\""
                    "},"
                    "\"position\":{\"latitude\":%.6f,\"longitude\":%.6f,\"time\":%lu}"
                    "},"
                    "\"ingestor\":\"%s\""
                    "}",
                    node_id, (unsigned long)num, (unsigned long)last_heard, name_json, short_hex, pub_hex, role, lat, lon,
                    (unsigned long)last_heard, ingestor_id);
    } else {
      n = snprintf(body, body_cap,
                    "{"
                    "\"%s\":{"
                    "\"num\":%lu,"
                    "\"lastHeard\":%lu,"
                    "\"protocol\":\"meshcore\","
                    "\"user\":{"
                    "\"longName\":%s,"
                    "\"shortName\":\"%s\","
                    "\"publicKey\":\"%s\""
                    "},"
                    "\"position\":{\"latitude\":%.6f,\"longitude\":%.6f,\"time\":%lu}"
                    "},"
                    "\"ingestor\":\"%s\""
                    "}",
                    node_id, (unsigned long)num, (unsigned long)last_heard, name_json, short_hex, pub_hex, lat, lon,
                    (unsigned long)last_heard, ingestor_id);
    }
  } else {
    if (role) {
      n = snprintf(body, body_cap,
                    "{"
                    "\"%s\":{"
                    "\"num\":%lu,"
                    "\"lastHeard\":%lu,"
                    "\"protocol\":\"meshcore\","
                    "\"user\":{"
                    "\"longName\":%s,"
                    "\"shortName\":\"%s\","
                    "\"publicKey\":\"%s\","
                    "\"role\":\"%s\""
                    "}"
                    "},"
                    "\"ingestor\":\"%s\""
                    "}",
                    node_id, (unsigned long)num, (unsigned long)last_heard, name_json, short_hex, pub_hex, role,
                    ingestor_id);
    } else {
      n = snprintf(body, body_cap,
                    "{"
                    "\"%s\":{"
                    "\"num\":%lu,"
                    "\"lastHeard\":%lu,"
                    "\"protocol\":\"meshcore\","
                    "\"user\":{"
                    "\"longName\":%s,"
                    "\"shortName\":\"%s\","
                    "\"publicKey\":\"%s\""
                    "}"
                    "},"
                    "\"ingestor\":\"%s\""
                    "}",
                    node_id, (unsigned long)num, (unsigned long)last_heard, name_json, short_hex, pub_hex, ingestor_id);
    }
  }

  if (n <= 0 || (size_t)n >= body_cap) {
    return false;
  }
  *out_len = (uint16_t)n;
  return true;
}

void PotatoMeshIngestor::postContactDiscovered(const uint8_t self_pub_key[PUB_KEY_SIZE],
                                               const ContactInfo& contact) {
  if (g_paused) {
    return;
  }
  if (!PotatoMeshConfig::instance().isIngestReady()) {
    return;
  }
  char node_id[12];
  char body[1800];
  uint16_t n;
  if (!build_contact_ingest_json(self_pub_key, contact, body, sizeof(body), &n, node_id)) {
    POTATO_MESH_DBG_LN("post abort %s: JSON build failed or overflow", node_id);
    return;
  }

  POTATO_MESH_DBG_LN("post begin %s: WiFi=%d RSSI=%d IP=%s", node_id, (int)WiFi.status(), WiFi.RSSI(),
                      WiFi.localIP().toString().c_str());
  const char* role = adv_type_to_role(contact.type);
  POTATO_MESH_DBG_LN("post %s: type=%u role=%s last_heard=%lu gps=(%ld,%ld)", node_id, (unsigned)contact.type,
                      role ? role : "?", (unsigned long)contact.last_advert_timestamp,
                      (long)contact.gps_lat, (long)contact.gps_lon);

  enqueue_pending(node_id, body, n);
}

bool PotatoMeshIngestor::tryEnqueueContact(const uint8_t self_pub_key[PUB_KEY_SIZE], const ContactInfo& contact) {
  if (g_paused) {
    return false;
  }
  if (!PotatoMeshConfig::instance().isIngestReady()) {
    return false; // let companion bootstrap retry after /wifi, /auth, /endpoint until ready
  }
  char node_id[12];
  char body[1800];
  uint16_t n;
  if (!build_contact_ingest_json(self_pub_key, contact, body, sizeof(body), &n, node_id)) {
    return true;
  }
  if (!enqueue_pending_try(node_id, body, n)) {
    return false;
  }
  POTATO_MESH_DBG_LN("bootstrap queued %s (%u in queue)", node_id, (unsigned)g_q.count);
  return true;
}

uint8_t PotatoMeshIngestor::pendingQueueDepth() const {
  return potato_ingest_queue_depth();
}

void PotatoMeshIngestor::restartAfterConfigChange() {
  potato_ingest_clear_queue();
}

void PotatoMeshIngestor::service() {
  if (g_paused) {
    return;
  }
  if (!PotatoMeshConfig::instance().isIngestReady()) {
    return;
  }
  ensure_worker();
  if (!g_q_mtx || !g_worker) {
    return;
  }
  bool pending = false;
  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  pending = g_q.count > 0;
  xSemaphoreGive(g_q_mtx);
  if (pending && WiFi.status() == WL_CONNECTED) {
    notify_worker();
  }
}

void PotatoMeshIngestor::setPaused(bool paused) { g_paused = paused; }

bool PotatoMeshIngestor::isPaused() const { return g_paused; }

#else

void PotatoMeshIngestor::postContactDiscovered(const uint8_t*, const ContactInfo&) {}

bool PotatoMeshIngestor::tryEnqueueContact(const uint8_t*, const ContactInfo&) { return true; }

uint8_t PotatoMeshIngestor::pendingQueueDepth() const { return 0; }

void PotatoMeshIngestor::service() {}

void PotatoMeshIngestor::restartAfterConfigChange() {}

void PotatoMeshIngestor::setPaused(bool) {}

bool PotatoMeshIngestor::isPaused() const { return false; }

#endif
