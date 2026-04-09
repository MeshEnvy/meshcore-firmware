#include "PotatoMeshIngestor.h"

#ifdef ESP32

#include <helpers/AdvertDataHelpers.h>
#include <helpers/esp32/PotatoMeshConfig.h>
#include <helpers/esp32/PotatoMeshDebug.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef POTATO_MESH_INGEST_API_PATH
#define POTATO_MESH_INGEST_API_PATH "/api/nodes"
#endif
#ifndef POTATO_MESH_HTTP_RETRY_DELAY_MS
#define POTATO_MESH_HTTP_RETRY_DELAY_MS 400
#endif
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
/** Merge up to this many queued single-node JSON bodies into one POST (server accepts multi-key /api/nodes). */
#ifndef POTATO_MESH_INGEST_BATCH_MAX
#define POTATO_MESH_INGEST_BATCH_MAX 8
#endif
#ifndef POTATO_MESH_WIFI_DOWN_LOG_INTERVAL_MS
#define POTATO_MESH_WIFI_DOWN_LOG_INTERVAL_MS 8000
#endif
/** Set to 1 to force 1.1.1.1/8.8.8.8 on GOT_IP (can break LANs that block third-party DNS). */
#ifndef POTATO_MESH_STA_FORCE_PUBLIC_DNS
#define POTATO_MESH_STA_FORCE_PUBLIC_DNS 0
#endif

extern "C" esp_err_t arduino_esp_crt_bundle_attach(void* conf);

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
  uint32_t fail_backoff_ms;
};

IngestQueue g_q{};
static uint32_t g_last_wifi_down_log_ms = 0;
bool g_paused = false;
static int g_last_http_code = 0;
SemaphoreHandle_t g_q_mtx = nullptr;
TaskHandle_t g_worker = nullptr;
portMUX_TYPE g_worker_init = portMUX_INITIALIZER_UNLOCKED;

void notify_worker();
void ensure_worker();

bool is_https_origin(const char* base) { return strncmp(base, "https://", 8) == 0; }

/** Copy hostname from https://host[:port]/… or http://… into host_out. */
static bool ingest_copy_url_hostname(const char* url, char* host_out, size_t cap) {
  if (!url || !host_out || cap < 2) return false;
  const char* p = nullptr;
  if (strncmp(url, "https://", 8) == 0) p = url + 8;
  else if (strncmp(url, "http://", 7) == 0) p = url + 7;
  if (!p) return false;
  size_t i = 0;
  while (p[i] && p[i] != '/' && p[i] != ':' && p[i] != '?' && i + 1 < cap) {
    host_out[i] = p[i];
    i++;
  }
  host_out[i] = '\0';
  return i > 0;
}

static void log_ingest_dns_for_host(const char* full_url) {
  if (!potato_mesh_dbg_active()) return;
  char host[96];
  if (!ingest_copy_url_hostname(full_url, host, sizeof(host))) return;
  IPAddress ip;
  if (WiFi.hostByName(host, ip)) {
    POTATO_MESH_DBG_LN("potato ingest: DNS ok host=%.64s -> %s", host, ip.toString().c_str());
  } else {
    POTATO_MESH_DBG_LN(
        "potato ingest: DNS failed host=%.64s (use router DNS; check tunnel/ngrok up)",
        host);
  }
}

class DevNullStream : public Stream {
public:
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t*, size_t len) override { return len; }
};

static void discard_http_body(HTTPClient& http) {
  DevNullStream sink;
  (void)http.writeToStream(&sink);
}

static void log_or_discard_response_body(HTTPClient& http, const char* node_id) {
  int sz = http.getSize();
  if (sz > 0 && sz <= 512) {
    char buf[513];
    WiFiClient& s = http.getStream();
    int n = s.readBytes((uint8_t*)buf, (size_t)sz);
    if (n < 0) n = 0;
    buf[std::min(n, 512)] = '\0';
    POTATO_MESH_DBG_LN("post %s: response body (%d bytes): %s", node_id, sz, buf);
    return;
  }
  discard_http_body(http);
}

static WiFiClient g_ingest_plain;
static HTTPClient g_ingest_http;

struct IngestHttpSession {
  char full_url[256];
  char origin[257];
  char token[129];
  uint32_t ip4;
  bool active;
};
static IngestHttpSession g_ingest_sess{};

static void reset_ingest_http_session() {
  g_ingest_http.setReuse(false);
  g_ingest_http.end();
  g_ingest_plain.stop();
  g_ingest_sess.active = false;
  g_ingest_sess.full_url[0] = '\0';
}

/**
 * HTTPS ingest via esp_http_client + esp_tls + mbedTLS CA bundle (arduino_esp_crt_bundle_attach).
 * Replaces WiFiClientSecure/HTTPClient for TLS; that stack can see EOF mid-handshake to some HTTPS edges.
 */
static bool try_post_https_esp_http(const char* full_url, const char* post_label, const char* body,
                                    uint16_t n) {
  esp_http_client_config_t cfg{};
  cfg.url = full_url;
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = (int)POTATO_MESH_HTTP_TIMEOUT_MS;
  cfg.crt_bundle_attach = arduino_esp_crt_bundle_attach;
  cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    POTATO_MESH_DBG_LN("post %s: esp_http_client_init failed", post_label);
    g_last_http_code = 0;
    return false;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  char auth_hdr[200];
  snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", PotatoMeshConfig::instance().apiToken());
  esp_http_client_set_header(client, "Authorization", auth_hdr);
  const char* origin = PotatoMeshConfig::instance().ingestOrigin();
  if (origin && strstr(origin, "ngrok") != nullptr) {
    esp_http_client_set_header(client, "ngrok-skip-browser-warning", "true");
  }

  if (esp_http_client_set_post_field(client, body, (int)n) != ESP_OK) {
    POTATO_MESH_DBG_LN("post %s: esp_http set_post_field failed", post_label);
    esp_http_client_cleanup(client);
    g_last_http_code = 0;
    return false;
  }

  POTATO_MESH_DBG_LN("post %s: heap=%u POST %u bytes to %s (esp_http)", post_label,
                      (unsigned)ESP.getFreeHeap(), (unsigned)n, full_url);

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  g_last_http_code = status;
  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    POTATO_MESH_DBG_LN("post %s: esp_http err=%s", post_label, esp_err_to_name(err));
    return false;
  }

  if (status >= 200 && status < 300) {
    POTATO_MESH_DBG_LN("post %s: HTTP %d ok", post_label, status);
    return true;
  }

  POTATO_MESH_DBG_LN("post %s: HTTP %d (will retry)", post_label, status);
  return false;
}

static bool ingest_http_session_matches(const char* full_url) {
  if (!g_ingest_sess.active) return false;
  PotatoMeshConfig& cfg = PotatoMeshConfig::instance();
  if (strcmp(g_ingest_sess.full_url, full_url) != 0 ||
      strcmp(g_ingest_sess.origin, cfg.ingestOrigin()) != 0 ||
      strcmp(g_ingest_sess.token, cfg.apiToken()) != 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
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
  g_ingest_sess.ip4 = (uint32_t)WiFi.localIP();
  g_ingest_sess.active = true;
}

static bool build_ingest_post_url(char* full, size_t cap) {
  const char* base = PotatoMeshConfig::instance().ingestOrigin();
  if (!base || base[0] == '\0') return false;
  const char* path = POTATO_MESH_INGEST_API_PATH;
  if (!path || path[0] != '/') return false;
  size_t blen = strlen(base);
  while (blen > 0 && base[blen - 1] == '/') blen--;
  int n = snprintf(full, cap, "%.*s%s", (int)blen, base, path);
  return n > 0 && (size_t)n < cap;
}

bool try_post_once(const char* post_label, const char* body, uint16_t n) {
  char full_url[256];
  if (!build_ingest_post_url(full_url, sizeof(full_url))) {
    POTATO_MESH_DBG_LN("post %s: ingest URL build failed", post_label);
    return false;
  }

  const char* origin = PotatoMeshConfig::instance().ingestOrigin();
  if (is_https_origin(origin)) {
    log_ingest_dns_for_host(full_url);
    if (potato_mesh_dbg_active()) {
      POTATO_MESH_DBG_LN("potato ingest: HTTPS esp_http (IDF tls + CA bundle)");
    }
    return try_post_https_esp_http(full_url, post_label, body, n);
  }

  if (!ingest_http_session_matches(full_url)) {
    reset_ingest_http_session();
  }

  g_ingest_http.setReuse(true);
  g_ingest_http.setConnectTimeout((int32_t)POTATO_MESH_HTTP_CONNECT_TIMEOUT_MS);
  g_ingest_http.setTimeout((uint32_t)POTATO_MESH_HTTP_TIMEOUT_MS);

  bool ok_begin = false;
  if (g_ingest_sess.active) {
    ok_begin = true;
  } else {
    log_ingest_dns_for_host(full_url);
    ok_begin = g_ingest_http.begin(g_ingest_plain, full_url);
  }

  if (!ok_begin) {
    POTATO_MESH_DBG_LN("post %s: http.begin() failed URL=%s", post_label, full_url);
    reset_ingest_http_session();
    return false;
  }

  if (!g_ingest_sess.active) {
    capture_ingest_http_session(full_url);
  }

  if (origin && strstr(origin, "ngrok") != nullptr) {
    g_ingest_http.addHeader("ngrok-skip-browser-warning", "true");
  }
  g_ingest_http.addHeader("Content-Type", "application/json");
  char auth_hdr[200];
  snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", PotatoMeshConfig::instance().apiToken());
  g_ingest_http.addHeader("Authorization", auth_hdr);

  POTATO_MESH_DBG_LN("post %s: heap=%u POST %u bytes to %s", post_label,
                      (unsigned)ESP.getFreeHeap(), (unsigned)n, full_url);

  int code = g_ingest_http.POST((uint8_t*)body, (size_t)n);
  g_last_http_code = code;

  if (code >= 200 && code < 300) {
    POTATO_MESH_DBG_LN("post %s: HTTP %d ok", post_label, code);
    discard_http_body(g_ingest_http);
    return true;
  }

  log_or_discard_response_body(g_ingest_http, post_label);
  if (code < 0) {
    POTATO_MESH_DBG_LN("post %s: transport error %d", post_label, code);
  } else {
    POTATO_MESH_DBG_LN("post %s: HTTP %d (will retry)", post_label, code);
  }
  if (code < 0 || code == 401) {
    reset_ingest_http_session();
  }
  return false;
}

void enqueue_pending(const char node_id[12], const char* body, uint16_t n) {
  if (n >= kBodyCap) {
    POTATO_MESH_DBG_LN("post %s: payload too large (%u)", node_id, (unsigned)n);
    return;
  }
  ensure_worker();
  if (!g_q_mtx) return;
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

uint8_t potato_ingest_queue_depth() {
  if (!g_q_mtx) return g_q.count;
  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  uint8_t c = g_q.count;
  xSemaphoreGive(g_q_mtx);
  return c;
}

void potato_ingest_clear_queue() {
  reset_ingest_http_session();
  if (!g_q_mtx) {
    g_q.head = g_q.tail = g_q.count = 0;
    g_q.next_retry_ms = g_q.fail_backoff_ms = 0;
    POTATO_MESH_DBG_LN("ingest queue cleared");
    return;
  }
  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  g_q.head = g_q.tail = g_q.count = 0;
  g_q.next_retry_ms = g_q.fail_backoff_ms = 0;
  xSemaphoreGive(g_q_mtx);
  POTATO_MESH_DBG_LN("ingest queue cleared");
  notify_worker();
}

static const char* find_ingestor_field(const char* json) { return strstr(json, ",\"ingestor\""); }

/** Combine two firmware-built /api/nodes bodies `{ "<id>": {...}, "ingestor": "<self>" }` into one object. */
static bool merge_ingest_bodies(const char* base, const char* next, uint16_t next_len, char* out, size_t out_cap,
                                uint16_t* out_len) {
  const char* ig_base = find_ingestor_field(base);
  const char* ig_next = find_ingestor_field(next);
  if (!ig_base || !ig_next) return false;
  if (strcmp(ig_base, ig_next) != 0) return false;
  if (next_len < 2 || next[0] != '{') return false;
  size_t prefix = (size_t)(ig_base - base);
  const char* mid_start = next + 1;
  size_t mid_len = (size_t)(ig_next - mid_start);
  size_t tail_len = strlen(ig_base);
  size_t total = prefix + 1 + mid_len + tail_len;
  if (total + 1 > out_cap) return false;
  memcpy(out, base, prefix);
  out[prefix] = ',';
  memcpy(out + prefix + 1, mid_start, mid_len);
  memcpy(out + prefix + 1 + mid_len, ig_base, tail_len + 1);
  *out_len = (uint16_t)total;
  return true;
}

static void format_ingest_post_label(char* out, size_t cap, const char (*ids)[12], uint8_t n) {
  if (n == 0 || cap < 2) {
    if (cap) out[0] = '\0';
    return;
  }
  if (n == 1) {
    snprintf(out, cap, "%s", ids[0]);
    return;
  }
  size_t pos = 0;
  int w = snprintf(out, cap, "batch");
  if (w < 0 || (size_t)w >= cap) return;
  pos = (size_t)w;
  for (uint8_t i = 0; i < n && pos + 1 < cap; i++) {
    w = snprintf(out + pos, cap - pos, "%c%s", i ? ',' : ':', ids[i]);
    if (w < 0 || (size_t)w >= cap - pos) break;
    pos += (size_t)w;
  }
}

bool ingest_try_step() {
  char merged[kBodyCap];
  char merge_tmp[kBodyCap];
  char post_label[96];
  char local_nid[12];
  char batch_ids[POTATO_MESH_INGEST_BATCH_MAX][12];
  uint16_t merged_len = 0;
  uint16_t first_plen = 0;
  uint8_t n_batch = 0;

  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  if (g_paused || !PotatoMeshConfig::instance().isIngestReady() || g_q.count == 0) {
    xSemaphoreGive(g_q_mtx);
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (potato_mesh_dbg_active()) {
      uint32_t now = millis();
      if (g_last_wifi_down_log_ms == 0 ||
          (int32_t)(now - g_last_wifi_down_log_ms) >= (int32_t)POTATO_MESH_WIFI_DOWN_LOG_INTERVAL_MS) {
        g_last_wifi_down_log_ms = now;
        POTATO_MESH_DBG_LN("ingest: waiting on WiFi (status=%d)", (int)WiFi.status());
      }
    }
    xSemaphoreGive(g_q_mtx);
    vTaskDelay(pdMS_TO_TICKS(200));
    return true;
  }
  uint32_t now = millis();
  if ((int32_t)(now - g_q.next_retry_ms) < 0) {
    uint32_t wait = g_q.next_retry_ms - now;
    xSemaphoreGive(g_q_mtx);
    if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait));
    return true;
  }

  first_plen = g_q.len[g_q.head];
  merged_len = first_plen;
  memcpy(merged, g_q.body[g_q.head], merged_len);
  merged[merged_len] = '\0';
  memcpy(local_nid, g_q.node_id[g_q.head], sizeof(local_nid));
  memcpy(batch_ids[0], g_q.node_id[g_q.head], 12);
  n_batch = 1;

  while (n_batch < POTATO_MESH_INGEST_BATCH_MAX && n_batch < g_q.count) {
    uint8_t idx = (uint8_t)((g_q.head + n_batch) % kQueueDepth);
    uint16_t next_len = g_q.len[idx];
    uint16_t new_len = 0;
    if (!merge_ingest_bodies(merged, g_q.body[idx], next_len, merge_tmp, sizeof(merge_tmp), &new_len)) break;
    memcpy(merged, merge_tmp, new_len + 1);
    merged_len = new_len;
    memcpy(batch_ids[n_batch], g_q.node_id[idx], 12);
    n_batch++;
  }

  format_ingest_post_label(post_label, sizeof(post_label), batch_ids, n_batch);
  if (potato_mesh_dbg_active() && n_batch > 1) {
    POTATO_MESH_DBG_LN("potato ingest: coalesced %u node POSTs into one (%u bytes)", (unsigned)n_batch,
                        (unsigned)merged_len);
  }
  xSemaphoreGive(g_q_mtx);

  bool ok = try_post_once(post_label, merged, merged_len);

  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  if (g_q.count >= n_batch && g_q.len[g_q.head] == first_plen &&
      memcmp(g_q.node_id[g_q.head], local_nid, 12) == 0) {
    if (ok) {
      for (uint8_t pops = n_batch; pops > 0; pops--) {
        g_q.head = (uint8_t)((g_q.head + 1) % kQueueDepth);
        g_q.count--;
      }
      g_q.next_retry_ms = 0;
      g_q.fail_backoff_ms = (uint32_t)POTATO_MESH_HTTP_RETRY_DELAY_MS;
    } else {
      uint32_t b = g_q.fail_backoff_ms;
      if (b < (uint32_t)POTATO_MESH_HTTP_RETRY_DELAY_MS) {
        b = (uint32_t)POTATO_MESH_HTTP_RETRY_DELAY_MS;
      } else {
        b = std::min(b * 2, (uint32_t)10000);
      }
      g_q.fail_backoff_ms = b;
      g_q.next_retry_ms = millis() + b;
    }
  }
  bool more = g_q.count > 0 && !g_paused;
  xSemaphoreGive(g_q_mtx);
  return more;
}

void ingest_worker_entry(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (ingest_try_step()) {}
  }
}

void notify_worker() {
  if (g_worker) xTaskNotifyGive(g_worker);
}

void ensure_worker() {
  if (g_worker) return;
  if (!g_q_mtx) g_q_mtx = xSemaphoreCreateMutex();
  if (!g_q_mtx) return;

  TaskHandle_t created = nullptr;
  if (xTaskCreate(ingest_worker_entry, "potato-ingest", POTATO_MESH_INGEST_WORKER_STACK,
                   nullptr, 1, &created) != pdPASS) {
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

// --- JSON build helpers ---

static void bin_to_hex_lower(const uint8_t* b, size_t n, char* out) {
  static const char* hexd = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[i * 2] = hexd[b[i] >> 4]; out[i * 2 + 1] = hexd[b[i] & 0x0f]; }
  out[n * 2] = '\0';
}

static const char* adv_type_to_role(uint8_t adv_type) {
  switch (adv_type) {
    case ADV_TYPE_CHAT:     return "COMPANION";
    case ADV_TYPE_REPEATER: return "REPEATER";
    case ADV_TYPE_ROOM:     return "ROOM_SERVER";
    case ADV_TYPE_SENSOR:   return "SENSOR";
    default:                return nullptr;
  }
}

static void format_node_id(char out[12], const uint8_t pub_key[PUB_KEY_SIZE]) {
  out[0] = '!';
  bin_to_hex_lower(pub_key, 4, out + 1);
}

static bool append_json_escaped_name(char* dest, size_t dest_size, const char* name) {
  if (dest_size < 3) return false;
  char* p = dest;
  char* end = dest + dest_size - 1;
  *p++ = '"';
  while (name && *name && p < end - 1) {
    unsigned char c = (unsigned char)*name++;
    if (c == '"' || c == '\\') {
      if (p >= end - 2) break;
      *p++ = '\\'; *p++ = (char)c;
    } else if (c >= 32 && c < 127) {
      *p++ = (char)c;
    } else {
      *p++ = '?';
    }
  }
  if (p >= end) return false;
  *p++ = '"'; *p = '\0';
  return true;
}

static bool build_contact_ingest_json(const uint8_t self_pub_key[PUB_KEY_SIZE], const ContactInfo& contact,
                                      char* body, size_t body_cap, uint16_t* out_len, char node_id[12]) {
  format_node_id(node_id, contact.id.pub_key);
  char ingestor_id[12];
  char pub_hex[65];
  char name_json[40];
  char short_hex[5];

  format_node_id(ingestor_id, self_pub_key);
  bin_to_hex_lower(contact.id.pub_key, PUB_KEY_SIZE, pub_hex);
  if (!append_json_escaped_name(name_json, sizeof(name_json), contact.name)) {
    strncpy(name_json, "\"?\"", sizeof(name_json));
    name_json[sizeof(name_json) - 1] = '\0';
  }
  bin_to_hex_lower(contact.id.pub_key, 2, short_hex);

  uint32_t num = ((uint32_t)contact.id.pub_key[0] << 24) | ((uint32_t)contact.id.pub_key[1] << 16) |
                 ((uint32_t)contact.id.pub_key[2] << 8)  | (uint32_t)contact.id.pub_key[3];
  uint32_t last_heard = contact.last_advert_timestamp;
  if (last_heard == 0) last_heard = (uint32_t)(millis() / 1000);

  const char* role = adv_type_to_role(contact.type);
  int n;

  if (contact.gps_lat != 0 || contact.gps_lon != 0) {
    double lat = (double)contact.gps_lat / 1000000.0;
    double lon = (double)contact.gps_lon / 1000000.0;
    if (role) {
      n = snprintf(body, body_cap,
        "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshcore\","
        "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\",\"role\":\"%s\"},"
        "\"position\":{\"latitude\":%.6f,\"longitude\":%.6f,\"time\":%lu}},"
        "\"ingestor\":\"%s\"}",
        node_id, (unsigned long)num, (unsigned long)last_heard, name_json,
        short_hex, pub_hex, role, lat, lon, (unsigned long)last_heard, ingestor_id);
    } else {
      n = snprintf(body, body_cap,
        "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshcore\","
        "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\"},"
        "\"position\":{\"latitude\":%.6f,\"longitude\":%.6f,\"time\":%lu}},"
        "\"ingestor\":\"%s\"}",
        node_id, (unsigned long)num, (unsigned long)last_heard, name_json,
        short_hex, pub_hex, lat, lon, (unsigned long)last_heard, ingestor_id);
    }
  } else {
    if (role) {
      n = snprintf(body, body_cap,
        "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshcore\","
        "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\",\"role\":\"%s\"}},"
        "\"ingestor\":\"%s\"}",
        node_id, (unsigned long)num, (unsigned long)last_heard, name_json,
        short_hex, pub_hex, role, ingestor_id);
    } else {
      n = snprintf(body, body_cap,
        "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshcore\","
        "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\"}},"
        "\"ingestor\":\"%s\"}",
        node_id, (unsigned long)num, (unsigned long)last_heard, name_json,
        short_hex, pub_hex, ingestor_id);
    }
  }

  if (n <= 0 || (size_t)n >= body_cap) return false;
  *out_len = (uint16_t)n;
  return true;
}

} // namespace

// --- WiFi event helpers (file scope — externally visible) ---

void potato_mesh_register_sta_dns_override() {
#if !POTATO_MESH_STA_FORCE_PUBLIC_DNS
  // Keep DHCP-assigned DNS (typically the router). Forcing 1.1.1.1/8.8.8.8 via WiFi.config()
  // breaks many networks (captive portal, ISP-only DNS, firewall rules) and triggers hostByName failures.
  return;
#else
  static bool g_sta_dns_override_applied = false;
  static bool registered = false;
  if (registered) return;
  registered = true;
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    (void)info;
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) { g_sta_dns_override_applied = false; return; }
    if (event != ARDUINO_EVENT_WIFI_STA_GOT_IP || g_sta_dns_override_applied) return;
    g_sta_dns_override_applied = true;
    const IPAddress dns1(1, 1, 1, 1), dns2(8, 8, 8, 8);
    bool ok = WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
    if (!ok) g_sta_dns_override_applied = false;
    POTATO_MESH_DBG_LN("potato ingest: STA DNS public override ok=%s", ok ? "yes" : "no");
  });
#endif
}

void potato_mesh_register_wifi_event_logging() {
  static bool registered = false;
  if (registered) return;
  registered = true;
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (!potato_mesh_dbg_active()) return;
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_START:
        POTATO_MESH_DBG_LN("WiFi STA: started"); break;
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        POTATO_MESH_DBG_LN("WiFi STA: associated (ch %u)", (unsigned)info.wifi_sta_connected.channel); break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        POTATO_MESH_DBG_LN("WiFi STA: got IP %s gw=%s",
                            WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str()); break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        POTATO_MESH_DBG_LN("WiFi STA: disconnected reason=%u - auto-retry",
                            (unsigned)info.wifi_sta_disconnected.reason); break;
      default: break;
    }
  });
}

// --- Public PotatoMeshIngestor methods ---

static void mesh_pk_short2_hex(const uint8_t pk[PUB_KEY_SIZE], char out[5]) {
  static const char* hx = "0123456789abcdef";
  out[0] = hx[pk[0] >> 4];
  out[1] = hx[pk[0] & 0x0f];
  out[2] = hx[pk[1] >> 4];
  out[3] = hx[pk[1] & 0x0f];
  out[4] = '\0';
}

void PotatoMeshIngestor::postContactDiscovered(const uint8_t self_pub_key[PUB_KEY_SIZE],
                                               const ContactInfo& contact) {
  if (g_paused || !PotatoMeshConfig::instance().isIngestReady()) return;
  char node_id[12];
  char body[1800];
  uint16_t n;
  if (!build_contact_ingest_json(self_pub_key, contact, body, sizeof(body), &n, node_id)) {
    POTATO_MESH_DBG_LN("post abort %s: JSON build failed", node_id);
    return;
  }
  char short2[5];
  mesh_pk_short2_hex(contact.id.pub_key, short2);
  POTATO_MESH_DBG_LN(
      "post %s short=%s name=\"%.20s\" type=%u last_heard=%lu gps=(%ld,%ld)", node_id, short2,
      contact.name, (unsigned)contact.type, (unsigned long)contact.last_advert_timestamp,
      (long)contact.gps_lat, (long)contact.gps_lon);
  enqueue_pending(node_id, body, n);
}

uint8_t PotatoMeshIngestor::pendingQueueDepth() const { return potato_ingest_queue_depth(); }

void PotatoMeshIngestor::restartAfterConfigChange() { potato_ingest_clear_queue(); }

void PotatoMeshIngestor::service() {
  if (g_paused || !PotatoMeshConfig::instance().isIngestReady()) return;
  ensure_worker();
  if (!g_q_mtx || !g_worker) return;
  bool pending = false;
  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  pending = g_q.count > 0;
  xSemaphoreGive(g_q_mtx);
  if (pending && WiFi.status() == WL_CONNECTED) notify_worker();
}

void PotatoMeshIngestor::setPaused(bool paused) { g_paused = paused; }
bool PotatoMeshIngestor::isPaused() const { return g_paused; }
int  PotatoMeshIngestor::lastHttpCode() const { return g_last_http_code; }

#endif // ESP32
