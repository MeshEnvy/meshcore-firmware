#include "LotatoIngestor.h"

#ifdef ESP32

#include <MeshCore.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/esp32/LotatoConfig.h>
#include <helpers/esp32/LotatoDebug.h>
#include <helpers/esp32/LotatoNodeStore.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_http_client.h>
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <time.h>

#ifndef LOTATO_INGEST_API_PATH
#define LOTATO_INGEST_API_PATH "/api/nodes"
#endif
/** Matches Python :func:`queue_ingestor_heartbeat` → ``POST /api/ingestors``. */
#ifndef LOTATO_INGESTORS_API_PATH
#define LOTATO_INGESTORS_API_PATH "/api/ingestors"
#endif
/** Ingestor ``version`` field (required by web ``upsert_ingestor``); override at build time if needed. */
#ifndef LOTATO_INGESTOR_VERSION
#define LOTATO_INGESTOR_VERSION "meshcore-esp32"
#endif
/** Default aligns with :data:`HEARTBEAT_INTERVAL_SECS` in ``data/mesh_ingestor/ingestors.py``. */
#ifndef LOTATO_INGESTOR_HEARTBEAT_MS
#define LOTATO_INGESTOR_HEARTBEAT_MS (60UL * 60UL * 1000UL)
#endif
#ifndef LOTATO_HTTP_RETRY_DELAY_MS
#define LOTATO_HTTP_RETRY_DELAY_MS 400
#endif
#ifndef LOTATO_HTTP_TIMEOUT_MS
#define LOTATO_HTTP_TIMEOUT_MS 12000
#endif
#ifndef LOTATO_HTTP_CONNECT_TIMEOUT_MS
#define LOTATO_HTTP_CONNECT_TIMEOUT_MS 8000
#endif
#ifndef LOTATO_INGEST_WORKER_STACK
#define LOTATO_INGEST_WORKER_STACK 12288
#endif
/** Max JSON body size for one /api/nodes batch POST (ESP32 heap). */
#ifndef LOTATO_INGEST_BODY_CAP
#define LOTATO_INGEST_BODY_CAP 4096
#endif
/** Max node entries merged into one POST (also limited by body cap). */
#ifndef LOTATO_INGEST_BATCH_MAX_SLOTS
#define LOTATO_INGEST_BATCH_MAX_SLOTS 48
#endif
#ifndef LOTATO_WIFI_DOWN_LOG_INTERVAL_MS
#define LOTATO_WIFI_DOWN_LOG_INTERVAL_MS 8000
#endif
/** Set to 1 to force 1.1.1.1/8.8.8.8 on GOT_IP (can break LANs that block third-party DNS). */
#ifndef LOTATO_STA_FORCE_PUBLIC_DNS
#define LOTATO_STA_FORCE_PUBLIC_DNS 0
#endif


namespace {

constexpr size_t kBodyCap = LOTATO_INGEST_BODY_CAP;
constexpr size_t kBatchMaxSlots = LOTATO_INGEST_BATCH_MAX_SLOTS;

char g_batch_body[kBodyCap]{};
uint16_t g_batch_len = 0;
uint8_t g_batch_n = 0;
uint16_t g_batch_slots[kBatchMaxSlots]{};
char g_batch_node_ids[kBatchMaxSlots][12]{};
LotatoNodeStore* g_batch_store = nullptr;
uint8_t g_batch_self_pk[PUB_KEY_SIZE]{};
uint32_t g_batch_next_retry_ms = 0;
uint32_t g_batch_fail_backoff_ms = 0;

/** Scratch for try_build_batch_from_store — must not live on loopTask stack (~12KB would overflow). */
static char g_build_merged[kBodyCap];
static char g_build_frag[kBodyCap];
static char g_build_merge_tmp[kBodyCap];

static uint32_t g_last_wifi_down_log_ms = 0;
bool g_paused = false;
static int g_last_http_code = 0;
/** Wall-clock start_time for ingestor payload; set first time we see plausible Unix time. */
static uint32_t g_ingestor_start_unix = 0;
/** millis() of last successful ``POST /api/ingestors`` (reference: Python ingestor heartbeat). */
static uint32_t g_ingestor_heartbeat_ok_ms = 0;
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
  if (!lotato_dbg_active()) return;
  char host[96];
  if (!ingest_copy_url_hostname(full_url, host, sizeof(host))) return;
  IPAddress ip;
  if (WiFi.hostByName(host, ip)) {
    LOTATO_DBG_LN("lotato ingest: DNS ok host=%.64s -> %s", host, ip.toString().c_str());
  } else {
    LOTATO_DBG_LN(
        "lotato ingest: DNS failed host=%.64s (use router DNS; check tunnel/ngrok up)",
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
    LOTATO_DBG_LN("post %s: response body (%d bytes): %s", node_id, sz, buf);
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
  cfg.timeout_ms = (int)LOTATO_HTTP_TIMEOUT_MS;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    LOTATO_DBG_LN("post %s: esp_http_client_init failed", post_label);
    g_last_http_code = 0;
    return false;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  char auth_hdr[200];
  snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", LotatoConfig::instance().apiToken());
  esp_http_client_set_header(client, "Authorization", auth_hdr);
  const char* origin = LotatoConfig::instance().ingestOrigin();
  if (origin && strstr(origin, "ngrok") != nullptr) {
    esp_http_client_set_header(client, "ngrok-skip-browser-warning", "true");
  }

  if (esp_http_client_set_post_field(client, body, (int)n) != ESP_OK) {
    LOTATO_DBG_LN("post %s: esp_http set_post_field failed", post_label);
    esp_http_client_cleanup(client);
    g_last_http_code = 0;
    return false;
  }

  LOTATO_DBG_LN("post %s: heap=%u POST %u bytes to %s (esp_http)", post_label,
                      (unsigned)ESP.getFreeHeap(), (unsigned)n, full_url);

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  g_last_http_code = status;
  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    LOTATO_DBG_LN("post %s: esp_http err=%s", post_label, esp_err_to_name(err));
    return false;
  }

  if (status >= 200 && status < 300) {
    LOTATO_DBG_LN("post %s: HTTP %d ok", post_label, status);
    return true;
  }

  LOTATO_DBG_LN("post %s: HTTP %d (will retry)", post_label, status);
  return false;
}

static bool ingest_http_session_matches(const char* full_url) {
  if (!g_ingest_sess.active) return false;
  LotatoConfig& cfg = LotatoConfig::instance();
  if (strcmp(g_ingest_sess.full_url, full_url) != 0 ||
      strcmp(g_ingest_sess.origin, cfg.ingestOrigin()) != 0 ||
      strcmp(g_ingest_sess.token, cfg.apiToken()) != 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  return (uint32_t)WiFi.localIP() == g_ingest_sess.ip4;
}

static void capture_ingest_http_session(const char* full_url) {
  LotatoConfig& cfg = LotatoConfig::instance();
  strncpy(g_ingest_sess.full_url, full_url, sizeof(g_ingest_sess.full_url) - 1);
  g_ingest_sess.full_url[sizeof(g_ingest_sess.full_url) - 1] = '\0';
  strncpy(g_ingest_sess.origin, cfg.ingestOrigin(), sizeof(g_ingest_sess.origin) - 1);
  g_ingest_sess.origin[sizeof(g_ingest_sess.origin) - 1] = '\0';
  strncpy(g_ingest_sess.token, cfg.apiToken(), sizeof(g_ingest_sess.token) - 1);
  g_ingest_sess.token[sizeof(g_ingest_sess.token) - 1] = '\0';
  g_ingest_sess.ip4 = (uint32_t)WiFi.localIP();
  g_ingest_sess.active = true;
}

static bool build_ingest_post_url_for_path(char* full, size_t cap, const char* path) {
  const char* base = LotatoConfig::instance().ingestOrigin();
  if (!base || base[0] == '\0') return false;
  if (!path || path[0] != '/') return false;
  size_t blen = strlen(base);
  while (blen > 0 && base[blen - 1] == '/') blen--;
  int n = snprintf(full, cap, "%.*s%s", (int)blen, base, path);
  return n > 0 && (size_t)n < cap;
}

static bool build_ingest_post_url(char* full, size_t cap) {
  return build_ingest_post_url_for_path(full, cap, LOTATO_INGEST_API_PATH);
}

static bool try_post_once_at_path(const char* api_path, const char* post_label, const char* body, uint16_t n) {
  char full_url[256];
  if (!build_ingest_post_url_for_path(full_url, sizeof(full_url), api_path)) {
    LOTATO_DBG_LN("post %s: ingest URL build failed", post_label);
    return false;
  }

  const char* origin = LotatoConfig::instance().ingestOrigin();
  if (is_https_origin(origin)) {
    log_ingest_dns_for_host(full_url);
    if (lotato_dbg_active()) {
      LOTATO_DBG_LN("lotato ingest: HTTPS esp_http (IDF tls + CA bundle)");
    }
    return try_post_https_esp_http(full_url, post_label, body, n);
  }

  if (!ingest_http_session_matches(full_url)) {
    reset_ingest_http_session();
  }

  g_ingest_http.setReuse(true);
  g_ingest_http.setConnectTimeout((int32_t)LOTATO_HTTP_CONNECT_TIMEOUT_MS);
  g_ingest_http.setTimeout((uint32_t)LOTATO_HTTP_TIMEOUT_MS);

  bool ok_begin = false;
  if (g_ingest_sess.active) {
    ok_begin = true;
  } else {
    log_ingest_dns_for_host(full_url);
    ok_begin = g_ingest_http.begin(g_ingest_plain, full_url);
  }

  if (!ok_begin) {
    LOTATO_DBG_LN("post %s: http.begin() failed URL=%s", post_label, full_url);
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
  snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", LotatoConfig::instance().apiToken());
  g_ingest_http.addHeader("Authorization", auth_hdr);

  LOTATO_DBG_LN("post %s: heap=%u POST %u bytes to %s", post_label,
                      (unsigned)ESP.getFreeHeap(), (unsigned)n, full_url);

  int code = g_ingest_http.POST((uint8_t*)body, (size_t)n);
  g_last_http_code = code;

  if (code >= 200 && code < 300) {
    LOTATO_DBG_LN("post %s: HTTP %d ok", post_label, code);
    discard_http_body(g_ingest_http);
    return true;
  }

  log_or_discard_response_body(g_ingest_http, post_label);
  if (code < 0) {
    LOTATO_DBG_LN("post %s: transport error %d", post_label, code);
  } else {
    LOTATO_DBG_LN("post %s: HTTP %d (will retry)", post_label, code);
  }
  if (code < 0 || code == 401) {
    reset_ingest_http_session();
  }
  return false;
}

static bool try_post_once(const char* post_label, const char* body, uint16_t n) {
  return try_post_once_at_path(LOTATO_INGEST_API_PATH, post_label, body, n);
}

uint8_t lotato_ingest_queue_depth() {
  if (!g_q_mtx) return g_batch_n;
  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  uint8_t n = g_batch_n;
  xSemaphoreGive(g_q_mtx);
  return n;
}

void lotato_ingest_clear_queue() {
  reset_ingest_http_session();
  g_ingestor_heartbeat_ok_ms = 0;
  if (!g_q_mtx) {
    g_batch_len = 0;
    g_batch_n = 0;
    g_batch_store = nullptr;
    g_batch_next_retry_ms = g_batch_fail_backoff_ms = 0;
    LOTATO_DBG_LN("ingest batch cleared");
    return;
  }
  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  g_batch_len = 0;
  g_batch_n = 0;
  g_batch_store = nullptr;
  g_batch_next_retry_ms = g_batch_fail_backoff_ms = 0;
  xSemaphoreGive(g_q_mtx);
  LOTATO_DBG_LN("ingest batch cleared");
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

// --- JSON build (used by batch builder; defined before ingest_try_step) ---

static void bin_to_hex_lower_pre(const uint8_t* b, size_t n, char* out) {
  static const char* hexd = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hexd[b[i] >> 4];
    out[i * 2 + 1] = hexd[b[i] & 0x0f];
  }
  out[n * 2] = '\0';
}

static const char* adv_type_to_role_pre(uint8_t adv_type) {
  switch (adv_type) {
    case ADV_TYPE_CHAT:     return "COMPANION";
    case ADV_TYPE_REPEATER: return "REPEATER";
    case ADV_TYPE_ROOM:     return "ROOM_SERVER";
    case ADV_TYPE_SENSOR:   return "SENSOR";
    default:                return nullptr;
  }
}

static void format_node_id_pre(char out[12], const uint8_t pub_key[PUB_KEY_SIZE]) {
  out[0] = '!';
  bin_to_hex_lower_pre(pub_key, 4, out + 1);
}

static bool self_pub_key_nonzero(const uint8_t pk[PUB_KEY_SIZE]) {
  for (size_t i = 0; i < PUB_KEY_SIZE; i++) {
    if (pk[i] != 0) return true;
  }
  return false;
}

static bool build_ingestor_heartbeat_body(const uint8_t self_pub[PUB_KEY_SIZE], char* buf, size_t cap,
                                          uint16_t* out_len) {
  char node_id[12];
  format_node_id_pre(node_id, self_pub);
  time_t t = time(nullptr);
  const bool unix_plausible = (t > (time_t)1700000000);
  int n;
  if (unix_plausible) {
    if (g_ingestor_start_unix == 0) g_ingestor_start_unix = (uint32_t)t;
    n = snprintf(buf, cap,
                 "{\"node_id\":\"%s\",\"start_time\":%lu,\"last_seen_time\":%lu,\"version\":\"%s\","
                 "\"protocol\":\"meshcore\"}",
                 node_id, (unsigned long)g_ingestor_start_unix, (unsigned long)t, LOTATO_INGESTOR_VERSION);
  } else {
    n = snprintf(buf, cap,
                 "{\"node_id\":\"%s\",\"version\":\"%s\",\"protocol\":\"meshcore\"}",
                 node_id, LOTATO_INGESTOR_VERSION);
  }
  if (n <= 0 || (size_t)n >= cap) return false;
  *out_len = (uint16_t)n;
  return true;
}

/** Before node batches, register like Python ``queue_ingestor_heartbeat`` (meshcore ``_process_self_info``). */
static void maybe_post_ingestor_heartbeat(const uint8_t self_pub[PUB_KEY_SIZE]) {
  if (!self_pub_key_nonzero(self_pub)) return;
  uint32_t now_ms = millis();
  if (g_ingestor_heartbeat_ok_ms != 0) {
    uint32_t elapsed = now_ms - g_ingestor_heartbeat_ok_ms;
    if (elapsed < LOTATO_INGESTOR_HEARTBEAT_MS) return;
  }

  char body[320];
  uint16_t blen = 0;
  if (!build_ingestor_heartbeat_body(self_pub, body, sizeof(body), &blen)) {
    LOTATO_DBG_LN("lotato ingest: ingestor heartbeat JSON build failed");
    return;
  }

  char full_url[256];
  if (!build_ingest_post_url_for_path(full_url, sizeof(full_url), LOTATO_INGESTORS_API_PATH)) {
    LOTATO_DBG_LN("lotato ingest: ingestor URL build failed");
    return;
  }

  const char* origin = LotatoConfig::instance().ingestOrigin();
  bool ok = false;
  if (is_https_origin(origin)) {
    log_ingest_dns_for_host(full_url);
    ok = try_post_https_esp_http(full_url, "ingestor", body, blen);
  } else {
    ok = try_post_once_at_path(LOTATO_INGESTORS_API_PATH, "ingestor", body, blen);
  }

  if (ok) {
    g_ingestor_heartbeat_ok_ms = millis();
    LOTATO_DBG_LN("lotato ingest: ingestor registered (meshcore)");
  } else {
    LOTATO_DBG_LN("lotato ingest: ingestor POST failed (nodes will retry; protocol may default)");
  }
}

static bool append_json_escaped_name_pre(char* dest, size_t dest_size, const char* name) {
  if (dest_size < 3) return false;
  char* p = dest;
  char* end = dest + dest_size - 1;
  *p++ = '"';
  while (name && *name && p < end - 1) {
    unsigned char c = (unsigned char)*name++;
    if (c == '"' || c == '\\') {
      if (p >= end - 2) break;
      *p++ = '\\';
      *p++ = (char)c;
    } else if (c >= 32 && c < 127) {
      *p++ = (char)c;
    } else {
      *p++ = '?';
    }
  }
  if (p >= end) return false;
  *p++ = '"';
  *p = '\0';
  return true;
}

static bool build_record_ingest_json(const uint8_t self_pub_key[PUB_KEY_SIZE], const LotatoNodeRecord& rec,
                                     char* body, size_t body_cap, uint16_t* out_len, char node_id[12]) {
  format_node_id_pre(node_id, rec.pub_key);
  char ingestor_id[12];
  char pub_hex[65];
  char name_json[40];
  char short_hex[5];

  format_node_id_pre(ingestor_id, self_pub_key);
  bin_to_hex_lower_pre(rec.pub_key, PUB_KEY_SIZE, pub_hex);
  if (!append_json_escaped_name_pre(name_json, sizeof(name_json), rec.name)) {
    strncpy(name_json, "\"?\"", sizeof(name_json));
    name_json[sizeof(name_json) - 1] = '\0';
  }
  bin_to_hex_lower_pre(rec.pub_key, 2, short_hex);

  uint32_t num = ((uint32_t)rec.pub_key[0] << 24) | ((uint32_t)rec.pub_key[1] << 16) |
                 ((uint32_t)rec.pub_key[2] << 8) | (uint32_t)rec.pub_key[3];
  uint32_t last_heard = rec.last_advert;
  if (last_heard == 0) last_heard = (uint32_t)(millis() / 1000);

  const char* role = adv_type_to_role_pre(rec.type);
  int n;

  if (rec.gps_lat != 0 || rec.gps_lon != 0) {
    double lat = (double)rec.gps_lat / 1000000.0;
    double lon = (double)rec.gps_lon / 1000000.0;
    if (role) {
      n = snprintf(body, body_cap,
                   "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshcore\","
                   "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\",\"role\":\"%s\"},"
                   "\"position\":{\"latitude\":%.6f,\"longitude\":%.6f,\"time\":%lu}},"
                   "\"ingestor\":\"%s\"}",
                   node_id, (unsigned long)num, (unsigned long)last_heard, name_json, short_hex, pub_hex, role, lat,
                   lon, (unsigned long)last_heard, ingestor_id);
    } else {
      n = snprintf(body, body_cap,
                   "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshcore\","
                   "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\"},"
                   "\"position\":{\"latitude\":%.6f,\"longitude\":%.6f,\"time\":%lu}},"
                   "\"ingestor\":\"%s\"}",
                   node_id, (unsigned long)num, (unsigned long)last_heard, name_json, short_hex, pub_hex, lat, lon,
                   (unsigned long)last_heard, ingestor_id);
    }
  } else {
    if (role) {
      n = snprintf(body, body_cap,
                   "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshcore\","
                   "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\",\"role\":\"%s\"}},"
                   "\"ingestor\":\"%s\"}",
                   node_id, (unsigned long)num, (unsigned long)last_heard, name_json, short_hex, pub_hex, role,
                   ingestor_id);
    } else {
      n = snprintf(body, body_cap,
                   "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshcore\","
                   "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\"}},"
                   "\"ingestor\":\"%s\"}",
                   node_id, (unsigned long)num, (unsigned long)last_heard, name_json, short_hex, pub_hex,
                   ingestor_id);
    }
  }

  if (n <= 0 || (size_t)n >= body_cap) return false;
  *out_len = (uint16_t)n;
  return true;
}

/** Fill g_batch_* from store (caller holds g_q_mtx). */
static void try_build_batch_from_store(LotatoNodeStore& store, const uint8_t self_pk[PUB_KEY_SIZE]) {
  if (g_batch_len > 0) return;

  char batch_ids[kBatchMaxSlots][12];
  uint16_t merged_len = 0;
  uint8_t n = 0;
  uint16_t slots[kBatchMaxSlots];
  uint32_t now_ms = millis();

  for (int slot = 0; slot < LotatoNodeStore::MAX; slot++) {
    if (!store.dueForIngest(slot, now_ms)) continue;
    LotatoNodeRecord rec{};
    if (!store.readRecord(slot, rec)) continue;
    char nid[12];
    uint16_t flen = 0;
    if (!build_record_ingest_json(self_pk, rec, g_build_frag, sizeof(g_build_frag), &flen, nid)) continue;

    if (n == 0) {
      memcpy(g_build_merged, g_build_frag, flen + 1);
      merged_len = flen;
      memcpy(batch_ids[0], nid, 12);
      slots[0] = (uint16_t)slot;
      n = 1;
      continue;
    }
    if (n >= kBatchMaxSlots) break;
    uint16_t new_len = 0;
    if (!merge_ingest_bodies(g_build_merged, g_build_frag, flen, g_build_merge_tmp, sizeof(g_build_merge_tmp),
                             &new_len))
      break;
    memcpy(g_build_merged, g_build_merge_tmp, new_len + 1);
    merged_len = new_len;
    memcpy(batch_ids[n], nid, 12);
    slots[n] = (uint16_t)slot;
    n++;
  }

  if (n == 0) return;

  memcpy(g_batch_body, g_build_merged, merged_len + 1);
  g_batch_len = merged_len;
  g_batch_n = n;
  memcpy(g_batch_slots, slots, n * sizeof(uint16_t));
  for (uint8_t i = 0; i < n; i++) memcpy(g_batch_node_ids[i], batch_ids[i], 12);
  memcpy(g_batch_self_pk, self_pk, PUB_KEY_SIZE);
  g_batch_store = &store;
  if (lotato_dbg_active()) {
    LOTATO_DBG_LN("lotato ingest: built batch %u nodes %u bytes", (unsigned)n, (unsigned)merged_len);
  }
}

bool ingest_try_step() {
  static char local_body[kBodyCap];
  static uint16_t local_slots[kBatchMaxSlots];
  uint16_t local_len = 0;
  uint8_t local_n = 0;
  LotatoNodeStore* local_store = nullptr;
  char post_label[96];
  char batch_ids[kBatchMaxSlots][12];
  uint8_t local_self_pk[PUB_KEY_SIZE]{};

  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  if (g_paused || !LotatoConfig::instance().isIngestReady() || g_batch_len == 0) {
    xSemaphoreGive(g_q_mtx);
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (lotato_dbg_active()) {
      uint32_t t = millis();
      if (g_last_wifi_down_log_ms == 0 ||
          (int32_t)(t - g_last_wifi_down_log_ms) >= (int32_t)LOTATO_WIFI_DOWN_LOG_INTERVAL_MS) {
        g_last_wifi_down_log_ms = t;
        LOTATO_DBG_LN("ingest: waiting on WiFi (status=%d)", (int)WiFi.status());
      }
    }
    xSemaphoreGive(g_q_mtx);
    vTaskDelay(pdMS_TO_TICKS(200));
    return true;
  }
  uint32_t now = millis();
  if ((int32_t)(now - g_batch_next_retry_ms) < 0) {
    uint32_t wait = g_batch_next_retry_ms - now;
    xSemaphoreGive(g_q_mtx);
    if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait));
    return true;
  }

  local_len = g_batch_len;
  memcpy(local_body, g_batch_body, local_len + 1);
  local_n = g_batch_n;
  memcpy(local_slots, g_batch_slots, local_n * sizeof(uint16_t));
  local_store = g_batch_store;
  for (uint8_t i = 0; i < local_n; i++) memcpy(batch_ids[i], g_batch_node_ids[i], 12);
  memcpy(local_self_pk, g_batch_self_pk, PUB_KEY_SIZE);

  format_ingest_post_label(post_label, sizeof(post_label), batch_ids, local_n);
  xSemaphoreGive(g_q_mtx);

  maybe_post_ingestor_heartbeat(local_self_pk);
  bool ok = try_post_once(post_label, local_body, local_len);

  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  if (ok && g_batch_len == local_len && g_batch_n == local_n && g_batch_store == local_store &&
      memcmp(g_batch_body, local_body, local_len) == 0) {
    uint32_t posted_ms = millis();
    if (local_store) {
      for (uint8_t i = 0; i < local_n; i++) {
        local_store->markPosted(local_slots[i], posted_ms);
      }
    }
    g_batch_len = 0;
    g_batch_n = 0;
    g_batch_store = nullptr;
    g_batch_next_retry_ms = 0;
    g_batch_fail_backoff_ms = (uint32_t)LOTATO_HTTP_RETRY_DELAY_MS;
  } else if (!ok) {
    uint32_t b = g_batch_fail_backoff_ms;
    if (b < (uint32_t)LOTATO_HTTP_RETRY_DELAY_MS) {
      b = (uint32_t)LOTATO_HTTP_RETRY_DELAY_MS;
    } else {
      b = std::min(b * 2, (uint32_t)10000);
    }
    g_batch_fail_backoff_ms = b;
    g_batch_next_retry_ms = millis() + b;
  }
  bool more = (g_batch_len > 0) && !g_paused;
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
  if (xTaskCreate(ingest_worker_entry, "lotato-ingest", LOTATO_INGEST_WORKER_STACK,
                   nullptr, 1, &created) != pdPASS) {
    LOTATO_DBG_LN("ingest worker xTaskCreate failed");
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

// --- WiFi event helpers (file scope — externally visible) ---

void lotato_register_sta_dns_override() {
#if !LOTATO_STA_FORCE_PUBLIC_DNS
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
    LOTATO_DBG_LN("lotato ingest: STA DNS public override ok=%s", ok ? "yes" : "no");
  });
#endif
}

void lotato_register_wifi_event_logging() {
  static bool registered = false;
  if (registered) return;
  registered = true;
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (!lotato_dbg_active()) return;
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_START:
        LOTATO_DBG_LN("WiFi STA: started"); break;
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        LOTATO_DBG_LN("WiFi STA: associated (ch %u)", (unsigned)info.wifi_sta_connected.channel); break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        LOTATO_DBG_LN("WiFi STA: got IP %s gw=%s",
                            WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str()); break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        LOTATO_DBG_LN("WiFi STA: disconnected reason=%u - auto-retry",
                            (unsigned)info.wifi_sta_disconnected.reason); break;
      default: break;
    }
  });
}

#ifndef LOTATO_WIFI_FAILOVER_DEBOUNCE_MS
#define LOTATO_WIFI_FAILOVER_DEBOUNCE_MS 1500
#endif

namespace {

bool lotato_wifi_reason_failover(uint8_t reason) {
  // ESP-IDF: join failures where trying another saved SSID may help (not e.g. local deauth storms).
  switch (reason) {
    case 201:  // WIFI_REASON_NO_AP_FOUND
    case 202:  // WIFI_REASON_AUTH_FAIL
    case 203:  // WIFI_REASON_ASSOC_FAIL
    case 204:  // WIFI_REASON_HANDSHAKE_TIMEOUT
      return true;
    default:
      return false;
  }
}

uint8_t lotato_known_index_for_disconnected(const WiFiEventInfo_t& info, LotatoConfig& cfg) {
  char ev_ssid[33]{};
  uint8_t len = info.wifi_sta_disconnected.ssid_len;
  if (len > sizeof(info.wifi_sta_disconnected.ssid)) len = sizeof(info.wifi_sta_disconnected.ssid);
  if (len > sizeof(ev_ssid) - 1) len = sizeof(ev_ssid) - 1;
  if (len > 0) {
    memcpy(ev_ssid, info.wifi_sta_disconnected.ssid, len);
    ev_ssid[len] = '\0';
  }
  uint8_t n = cfg.knownWifiCount();
  for (uint8_t i = 0; i < n; i++) {
    char s[33], p[65];
    if (!cfg.getKnownWifi(i, s, sizeof(s), p, sizeof(p))) continue;
    if (ev_ssid[0] != '\0' && strcmp(s, ev_ssid) == 0) return i;
  }
  for (uint8_t i = 0; i < n; i++) {
    char s[33], p[65];
    if (!cfg.getKnownWifi(i, s, sizeof(s), p, sizeof(p))) continue;
    if (strcmp(s, cfg.ssid()) == 0) return i;
  }
  return 0;
}

} // namespace

static bool g_lotato_sta_failover_suppress = false;

void lotato_sta_failover_suppress(bool suppress) { g_lotato_sta_failover_suppress = suppress; }

void lotato_register_sta_known_wifi_failover() {
  static bool registered = false;
  if (registered) return;
  registered = true;
  static uint32_t g_last_sta_wifi_action_ms = 0;

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
    if (g_lotato_sta_failover_suppress) return;

    LotatoConfig& cfg = LotatoConfig::instance();
    uint8_t n = cfg.knownWifiCount();
    if (n < 2) {
      WiFi.setAutoReconnect(true);
      return;
    }
    WiFi.setAutoReconnect(false);

    uint32_t now = millis();
    if ((int32_t)(now - g_last_sta_wifi_action_ms) < (int32_t)LOTATO_WIFI_FAILOVER_DEBOUNCE_MS) return;
    g_last_sta_wifi_action_ms = now;

    uint8_t reason = info.wifi_sta_disconnected.reason;
    if (lotato_wifi_reason_failover(reason)) {
      uint8_t idx = lotato_known_index_for_disconnected(info, cfg);
      uint8_t next = (uint8_t)((idx + 1) % n);
      char ssid[33], pwd[65];
      if (!cfg.getKnownWifi(next, ssid, sizeof(ssid), pwd, sizeof(pwd))) return;
      const char* pw = (pwd[0] != '\0') ? pwd : nullptr;
      LOTATO_DBG_LN("lotato wifi /sta: failover reason=%u from idx %u -> %u ssid=%.32s",
                         (unsigned)reason, (unsigned)idx, (unsigned)next, ssid);
      WiFi.disconnect(false, false);
      WiFi.begin(ssid, pw);
      WiFi.setSleep(WIFI_PS_NONE);
      return;
    }
    LOTATO_DBG_LN("lotato wifi /sta: reconnect same AP (reason=%u)", (unsigned)reason);
    WiFi.reconnect();
  });
}

namespace {

void dbg_serial_write_full_body(const char* body) {
  if (!body || !body[0]) return;
  constexpr size_t kChunk = 48;
  size_t n = 0;
  for (const char* p = body; *p != '\0'; ++p) {
    Serial.write(static_cast<uint8_t>(*p));
    if (++n % kChunk == 0) yield();
  }
}

} // namespace

void lotato_dbg_trace_cli_exchange(const char* route_tag, const char* cmd_snapshot, const char* reply) {
  if (!lotato_dbg_active()) return;
  const char* tag = route_tag ? route_tag : "?";
  const char* cmd = cmd_snapshot ? cmd_snapshot : "";
  const char* rp = reply ? reply : "";
  Serial.print("Lotato: CLI ");
  Serial.print(tag);
  Serial.print(": cmd len=");
  Serial.print((unsigned)strlen(cmd));
  Serial.print("\r\n");
  dbg_serial_write_full_body(cmd);
  Serial.print("\r\n");
  Serial.print("Lotato: CLI ");
  Serial.print(tag);
  Serial.print(": reply len=");
  Serial.print((unsigned)strlen(rp));
  Serial.print("\r\n");
  dbg_serial_write_full_body(rp);
  Serial.print("\r\n");
}

// --- Public LotatoIngestor methods ---

uint8_t LotatoIngestor::pendingQueueDepth() const { return lotato_ingest_queue_depth(); }

void LotatoIngestor::restartAfterConfigChange() { lotato_ingest_clear_queue(); }

void LotatoIngestor::service(LotatoNodeStore* node_store, const uint8_t* self_pub_key) {
  if (g_paused || !LotatoConfig::instance().isIngestReady()) return;
  ensure_worker();
  if (!g_q_mtx || !g_worker) return;

  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  if (node_store && self_pub_key && g_batch_len == 0) {
    try_build_batch_from_store(*node_store, self_pub_key);
  }
  bool pending = g_batch_len > 0;
  xSemaphoreGive(g_q_mtx);

  if (pending && WiFi.status() == WL_CONNECTED) notify_worker();
}

void LotatoIngestor::setPaused(bool paused) { g_paused = paused; }
bool LotatoIngestor::isPaused() const { return g_paused; }
int  LotatoIngestor::lastHttpCode() const { return g_last_http_code; }

#endif // ESP32
