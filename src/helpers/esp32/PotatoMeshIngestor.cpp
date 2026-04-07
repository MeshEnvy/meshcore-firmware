#include "PotatoMeshIngestor.h"

#if defined(ESP32) && defined(POTATO_MESH_INGEST)

#include <helpers/AdvertDataHelpers.h>
#include <helpers/esp32/PotatoMeshConfig.h>
#include <HTTPClient.h>
#include <WiFi.h>
#if defined(POTATO_MESH_USE_WOLFSSL)
#include <wolfssl.h>
#include <cerrno>
#else
#include <WiFiClientSecure.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <algorithm>
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
/**
 * On ESP32-S3 + BLE, the hardware SHA driver allocates from DMA-capable internal SRAM
 * (heap_caps_malloc MALLOC_CAP_DMA). BLE occupies most of this pool, leaving little for
 * the TLS handshake SHA operations.  We gate HTTPS attempts on a minimum DMA free size
 * to avoid the "esp-sha: Failed to allocate buf memory" hard error and tight retry loop.
 * Plain HTTP connections are not affected.
 */
#ifndef POTATO_MESH_HTTPS_DMA_MIN_BYTES
#define POTATO_MESH_HTTPS_DMA_MIN_BYTES 8192
#endif
#ifndef POTATO_MESH_INGEST_WORKER_STACK
/* wolfSSL software RSA/ECC operations during TLS handshake need significant stack.
 * 24KB is a safe default when POTATO_MESH_USE_WOLFSSL is active; 12KB otherwise. */
#if defined(POTATO_MESH_USE_WOLFSSL)
#define POTATO_MESH_INGEST_WORKER_STACK 24576
#else
#define POTATO_MESH_INGEST_WORKER_STACK 12288
#endif
#endif
#ifndef POTATO_MESH_INGEST_QUEUE_DEPTH
#define POTATO_MESH_INGEST_QUEUE_DEPTH 8
#endif
#ifndef POTATO_MESH_WIFI_DOWN_LOG_INTERVAL_MS
#define POTATO_MESH_WIFI_DOWN_LOG_INTERVAL_MS 8000
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
  /** Grows on POST failure (capped) to reduce tight retry loops and heap pressure. */
  uint32_t fail_backoff_ms;
};

IngestQueue g_q{};
/** Rate-limited Serial line when ingest is blocked on WiFi (proves STA still trying / wrong AP). */
static uint32_t g_last_wifi_down_log_ms = 0;
bool g_paused = false;
SemaphoreHandle_t g_q_mtx = nullptr;
TaskHandle_t g_worker = nullptr;
portMUX_TYPE g_worker_init = portMUX_INITIALIZER_UNLOCKED;

void notify_worker();
void ensure_worker();

bool is_https_origin(const char* base) { return strncmp(base, "https://", 8) == 0; }

/** Discard HTTP body without Arduino String (avoids heap fragmentation during ingest). */
class DevNullStream : public Stream {
public:
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t c) override {
    (void)c;
    return 1;
  }
  size_t write(const uint8_t* buf, size_t len) override {
    (void)buf;
    return len;
  }
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
    if (n < 0) {
      n = 0;
    }
    buf[std::min(n, 512)] = '\0';
    POTATO_MESH_DBG_LN("post %s: response body (%d bytes): %s", node_id, sz, buf);
    return;
  }
  discard_http_body(http);
}

#if defined(POTATO_MESH_USE_WOLFSSL)
/**
 * Minimal WiFiClient subclass backed by wolfSSL (software crypto via NO_ESP32_CRYPT).
 * Uses custom I/O callbacks so it doesn't rely on WiFiClient::fd().
 * One shared WOLFSSL_CTX per process; one WOLFSSL* per TCP connection.
 */
class WolfTLSClient final : public WiFiClient {
  static WOLFSSL_CTX* s_ctx;
  WOLFSSL*   _ssl{};
  WiFiClient _tcp;
  bool       _ssl_up{false};

  static void ensure_ctx() {
    if (s_ctx) return;
    wolfSSL_Init();
    wolfSSL_Debugging_ON();
    s_ctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
    if (!s_ctx) {
      POTATO_MESH_DBG_LN("WolfTLS: CTX alloc failed heap=%u blk=%u",
                          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      return;
    }
    wolfSSL_CTX_set_verify(s_ctx, WOLFSSL_VERIFY_NONE, nullptr);
    POTATO_MESH_DBG_LN("WolfTLS: CTX created heap=%u blk=%u",
                        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  }

public:
  void setInsecure() { /* WOLFSSL_VERIFY_NONE already set on CTX */ }

  int connect(const char* host, uint16_t port) override {
    stop();
    ensure_ctx();
    if (!s_ctx) return 0;
    POTATO_MESH_DBG_LN("WolfTLS: TCP connect %s:%u heap=%u blk=%u",
                        host, (unsigned)port, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    if (!_tcp.connect(host, port)) {
      POTATO_MESH_DBG_LN("WolfTLS: TCP connect failed");
      return 0;
    }
    _ssl = wolfSSL_new(s_ctx);
    if (!_ssl) {
      POTATO_MESH_DBG_LN("WolfTLS: wolfSSL_new failed heap=%u blk=%u",
                          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      _tcp.stop(); return 0;
    }
    /* Use the underlying socket fd so wolfSSL drives I/O directly (avoids USE_WOLFSSL_IO
     * conflicts with custom callbacks in TLS 1.3 code paths). */
    wolfSSL_set_fd(_ssl, _tcp.fd());
    /* SNI is required by most modern HTTPS endpoints (including ngrok, Cloudflare).
     * Without it the server can't route the connection and sends a TLS alert / RST. */
    wolfSSL_UseSNI(_ssl, WOLFSSL_SNI_HOST_NAME, host, (unsigned short)strlen(host));
    unsigned long deadline = millis() + 15000;
    int last_err = 0;
    do {
      int r = wolfSSL_connect(_ssl);
      if (r == WOLFSSL_SUCCESS) { _ssl_up = true; return 1; }
      last_err = wolfSSL_get_error(_ssl, r);
      if (last_err != WOLFSSL_ERROR_WANT_READ && last_err != WOLFSSL_ERROR_WANT_WRITE) break;
      delay(5);
    } while (millis() < deadline);
    POTATO_MESH_DBG_LN("WolfTLS: handshake failed err=%d errno=%d heap=%u blk=%u",
                        last_err, errno, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    if (_ssl) { wolfSSL_free(_ssl); _ssl = nullptr; }
    _tcp.stop();
    return 0;
  }
  int connect(IPAddress ip, uint16_t port) override {
    return connect(ip.toString().c_str(), port);
  }

  size_t write(const uint8_t* buf, size_t size) override {
    if (!_ssl_up) return 0;
    int r = wolfSSL_write(_ssl, buf, (int)size);
    return r > 0 ? (size_t)r : 0;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  int read() override { uint8_t b; return read(&b, 1) == 1 ? b : -1; }
  int read(uint8_t* buf, size_t size) override {
    if (!_ssl_up) return -1;
    int r = wolfSSL_read(_ssl, buf, (int)size);
    return r > 0 ? r : -1;
  }
  int available() override {
    if (!_ssl_up) return 0;
    int p = wolfSSL_pending(_ssl);
    if (p > 0) return p;
    /* wolfSSL_set_fd drives the socket; fall back to checking the underlying TCP. */
    return _tcp.available();
  }
  int peek() override { return -1; }
  void flush() override {}
  void stop() override {
    if (_ssl) {
      if (_ssl_up) wolfSSL_shutdown(_ssl);  /* only for successfully connected sessions */
      wolfSSL_free(_ssl);
      _ssl = nullptr;
    }
    _ssl_up = false;
    _tcp.stop();
  }
  uint8_t connected() override { return (_ssl_up && _tcp.connected()) ? 1 : 0; }
  explicit operator bool() { return connected(); }
};
WOLFSSL_CTX* WolfTLSClient::s_ctx = nullptr;
#endif

/** Long-lived HTTP + TCP/TLS: avoids HTTPClient destructor calling stop() every POST, enables keep-alive. */
#if defined(POTATO_MESH_USE_WOLFSSL)
static WolfTLSClient g_ingest_tls;
#else
static WiFiClientSecure g_ingest_tls;
static bool g_ingest_tls_insecure = false;
#endif
static WiFiClient g_ingest_plain;
static HTTPClient g_ingest_http;

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
#if !defined(POTATO_MESH_USE_WOLFSSL)
  g_ingest_tls_insecure = false;
#endif
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
#if defined(POTATO_MESH_USE_WOLFSSL)
    /* wolfSSL uses software SHA/AES (NO_ESP32_CRYPT) so all crypto allocates from regular
     * heap — no DMA-capable SRAM needed. setInsecure() is idempotent; safe to call each time
     * a new session is started. */
    g_ingest_tls.setInsecure();
    ok_begin = g_ingest_http.begin(g_ingest_tls, full_url);
#else
    /* WiFiClientSecure uses the IDF's pre-compiled mbedTLS + hardware SHA which allocates
     * from DMA-capable SRAM. Gate the attempt when that pool is too low. */
    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t dma_blk  = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    POTATO_MESH_DBG_LN("post %s: dma_free=%u dma_blk=%u", node_id, (unsigned)dma_free, (unsigned)dma_blk);
    if (dma_blk < (size_t)POTATO_MESH_HTTPS_DMA_MIN_BYTES) {
      POTATO_MESH_DBG_LN("post %s: skip HTTPS – DMA heap too low (%u blk < %u needed)", node_id,
                          (unsigned)dma_blk, (unsigned)POTATO_MESH_HTTPS_DMA_MIN_BYTES);
      return false;
    }
    if (!g_ingest_tls_insecure) {
      g_ingest_tls.setInsecure();
      g_ingest_tls_insecure = true;
    }
    ok_begin = g_ingest_http.begin(g_ingest_tls, full_url);
#endif
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

#if defined(POTATO_MESH_USE_WOLFSSL)
  /* HTTPClient::connect() calls the NON-VIRTUAL WiFiClient::connect(host, port, timeout),
   * bypassing WolfTLSClient's override entirely. Pre-connect TLS here so HTTPClient sees
   * connected()=true and skips its own connect() call. */
  if (is_https_origin(origin) && !g_ingest_tls.connected()) {
    const char* hs = origin + 8;  /* skip "https://" */
    const char* colon = strchr(hs, ':');
    const char* slash = strchr(hs, '/');
    char wolf_host[128];
    uint16_t wolf_port = 443;
    size_t hlen = (colon && (!slash || colon < slash)) ? (size_t)(colon - hs)
                                                       : (slash ? (size_t)(slash - hs) : strlen(hs));
    if (hlen == 0 || hlen >= sizeof(wolf_host)) {
      reset_ingest_http_session();
      return false;
    }
    if (colon && (!slash || colon < slash)) wolf_port = (uint16_t)atoi(colon + 1);
    memcpy(wolf_host, hs, hlen);
    wolf_host[hlen] = '\0';
    POTATO_MESH_DBG_LN("post %s: WolfTLS connecting %s:%u heap=%u blk=%u", node_id,
                        wolf_host, (unsigned)wolf_port,
                        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    if (!g_ingest_tls.connect(wolf_host, wolf_port)) {
      POTATO_MESH_DBG_LN("post %s: WolfTLS connect failed", node_id);
      reset_ingest_http_session();
      return false;
    }
    POTATO_MESH_DBG_LN("post %s: WolfTLS connected", node_id);
  }
#endif

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
    discard_http_body(g_ingest_http);
    return true;
  }

  log_or_discard_response_body(g_ingest_http, node_id);
  if (code < 0) {
    POTATO_MESH_DBG_LN("post %s: transport error %d", node_id, code);
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
    g_q.fail_backoff_ms = 0;
    POTATO_MESH_DBG_LN("ingest queue cleared (config change)");
    return;
  }
  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  g_q.head = g_q.tail = g_q.count = 0;
  g_q.next_retry_ms = 0;
  g_q.fail_backoff_ms = 0;
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
    if (potato_mesh_dbg_active()) {
      PotatoMeshConfig& pc = PotatoMeshConfig::instance();
      uint32_t now = millis();
      if (pc.ssid()[0] != '\0' &&
          (g_last_wifi_down_log_ms == 0 ||
           (int32_t)(now - g_last_wifi_down_log_ms) >= (int32_t)POTATO_MESH_WIFI_DOWN_LOG_INTERVAL_MS)) {
        g_last_wifi_down_log_ms = now;
        POTATO_MESH_DBG_LN("ingest: waiting on WiFi (configured ssid=%s, status=%d)", pc.ssid(),
                            (int)WiFi.status());
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
      g_q.fail_backoff_ms = (uint32_t)POTATO_MESH_HTTP_RETRY_DELAY_MS;
    } else {
      uint32_t b = g_q.fail_backoff_ms;
      if (b < (uint32_t)POTATO_MESH_HTTP_RETRY_DELAY_MS) {
        b = (uint32_t)POTATO_MESH_HTTP_RETRY_DELAY_MS;
      } else {
        constexpr uint32_t kBackoffMax = 10000;
        b = std::min(b * 2, kBackoffMax);
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

static const char* wifi_sta_reason_tag(uint8_t r) {
  switch (r) {
    case 200:
      return "BEACON_TIMEOUT";
    case 201:
      return "NO_AP_FOUND";
    case 202:
      return "AUTH_FAIL";
    case 203:
      return "ASSOC_FAIL";
    case 2:
      return "AUTH_EXPIRE";
    case 15:
      return "4WAY_HANDSHAKE_TIMEOUT";
    case 204:
      return "HANDSHAKE_TIMEOUT";
    default:
      return NULL;
  }
}

} // namespace

/** WiFi.config() in GOT_IP retriggers GOT_IP; only apply DNS override once per association. */
static bool g_sta_dns_override_applied = false;

void potato_mesh_register_sta_dns_override() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    (void)info;
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      g_sta_dns_override_applied = false;
      return;
    }
    if (event != ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      return;
    }
    if (g_sta_dns_override_applied) {
      return;
    }
    /* Public resolvers; DHCP-provided DNS is often the router relay and can fail hostByName while browsers use DoH.
     * Set flag before WiFi.config: that call can emit another GOT_IP; re-entering would loop forever. */
    g_sta_dns_override_applied = true;
    const IPAddress dns1(1, 1, 1, 1);
    const IPAddress dns2(8, 8, 8, 8);
    bool ok = WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
    if (!ok) {
      g_sta_dns_override_applied = false;
    }
    POTATO_MESH_DBG_LN("potato ingest: STA DNS override primary=%s secondary=%s WiFi.config=%s",
                       dns1.toString().c_str(), dns2.toString().c_str(), ok ? "ok" : "failed");
  });
}

void potato_mesh_register_wifi_event_logging() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (!potato_mesh_dbg_active()) {
      return;
    }
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_START:
        POTATO_MESH_DBG_LN("WiFi STA: started");
        break;
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        POTATO_MESH_DBG_LN("WiFi STA: associated (channel %u)", (unsigned)info.wifi_sta_connected.channel);
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        POTATO_MESH_DBG_LN("WiFi STA: got IP %s gw=%s", WiFi.localIP().toString().c_str(),
                            WiFi.gatewayIP().toString().c_str());
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
        uint8_t r = info.wifi_sta_disconnected.reason;
        const char* tag = wifi_sta_reason_tag(r);
        const char* cfg_ssid = PotatoMeshConfig::instance().ssid();
        if (tag) {
          POTATO_MESH_DBG_LN("WiFi STA: disconnected reason=%u (%s) — auto-retry (target ssid=%s)", (unsigned)r, tag,
                              cfg_ssid[0] ? cfg_ssid : "(none)");
        } else {
          POTATO_MESH_DBG_LN("WiFi STA: disconnected reason=%u — auto-retry (target ssid=%s)", (unsigned)r,
                              cfg_ssid[0] ? cfg_ssid : "(none)");
        }
        break;
      }
      default:
        break;
    }
  });
}

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
