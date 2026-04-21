#include "LotatoIngestor.h"

#ifdef ESP32

#include <MeshCore.h>
#include <helpers/AdvertDataHelpers.h>
#include <LotatoConfig.h>
#include <LotatoDebug.h>
#include <LotatoNodeStore.h>
#include <lofi/Lofi.h>
#include <losettings/LoSettings.h>
#include <WiFi.h>
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

static bool is_https_origin(const char* base) { return base && strncmp(base, "https://", 8) == 0; }

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
    LOTATO_DBG_LN("lotato ingest: DNS failed host=%.64s", host);
  }
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

static int g_last_http_code = 0;

static bool lotato_post(const char* api_path, const char* post_label, const char* body, uint16_t n) {
  char full_url[256];
  if (!build_ingest_post_url_for_path(full_url, sizeof(full_url), api_path)) {
    LOTATO_DBG_LN("post %s: url build failed", post_label);
    g_last_http_code = 0;
    return false;
  }
  const char* origin = LotatoConfig::instance().ingestOrigin();
  const char* ngrok_hdr = (origin && strstr(origin, "ngrok")) ? "ngrok-skip-browser-warning" : nullptr;
  const char* ngrok_val = ngrok_hdr ? "true" : nullptr;
  if (is_https_origin(origin)) log_ingest_dns_for_host(full_url);
  auto r = lofi::Lofi::instance().httpPost(full_url, LotatoConfig::instance().apiToken(), body, n, ngrok_hdr,
                                           ngrok_val);
  g_last_http_code = r.status;
  if (r.status >= 200 && r.status < 300) {
    LOTATO_DBG_LN("post %s: HTTP %d ok", post_label, r.status);
    return true;
  }
  LOTATO_DBG_LN("post %s: HTTP %d err=%d", post_label, r.status, r.err);
  return false;
}

}  // namespace

// --- STA DNS / event logging / known-wifi failover ---

void lotato_register_sta_dns_override() {
#if LOTATO_STA_FORCE_PUBLIC_DNS
  lofi::Lofi::instance().setForcePublicDns(true);
#endif
}

void lotato_register_wifi_event_logging() {
  // Lofi already logs STA events via weak `lofi_log`; a strong override routes them through LOTATO_DBG.
}

void lotato_register_sta_known_wifi_failover() {
  // Implemented by lofi::Lofi::begin() (reads `known_wifi` LoDB table).
}

void lotato_sta_failover_suppress(bool suppress) { lofi::Lofi::instance().staFailoverSuppress(suppress); }

extern "C" void lofi_log(const char* msg) { LOTATO_DBG_LN("%s", msg); }

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
/** Wall-clock start_time for ingestor payload; set first time we see plausible Unix time. */
static uint32_t g_ingestor_start_unix = 0;
/** millis() of last successful ``POST /api/ingestors`` (reference: Python ingestor heartbeat). */
static uint32_t g_ingestor_heartbeat_ok_ms = 0;
SemaphoreHandle_t g_q_mtx = nullptr;
TaskHandle_t g_worker = nullptr;
portMUX_TYPE g_worker_init = portMUX_INITIALIZER_UNLOCKED;

void notify_worker();
void ensure_worker();

static bool ingest_paused() { return LotatoConfig::instance().ingestPaused(); }

uint8_t lotato_ingest_queue_depth() {
  if (!g_q_mtx) return g_batch_n;
  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  uint8_t n = g_batch_n;
  xSemaphoreGive(g_q_mtx);
  return n;
}

void lotato_ingest_clear_queue() {
  lofi::Lofi::instance().resetHttpTransport();
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

  bool ok = lotato_post(LOTATO_INGESTORS_API_PATH, "ingestor", body, blen);

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
  if (ingest_paused() || !LotatoConfig::instance().isIngestReady() || g_batch_len == 0) {
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
  bool ok = lotato_post(LOTATO_INGEST_API_PATH, post_label, local_body, local_len);

  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  if (ok && g_batch_len == local_len && g_batch_n == local_n && g_batch_store == local_store &&
      memcmp(g_batch_body, local_body, local_len) == 0) {
    uint32_t posted_unix = (uint32_t)time(nullptr);
    if (local_store) {
      for (uint8_t i = 0; i < local_n; i++) {
        local_store->markPosted(local_slots[i], posted_unix);
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
  bool more = (g_batch_len > 0) && !ingest_paused();
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

void lotato_ingest_restart_after_config() { lotato_ingest_clear_queue(); }

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
  if (ingest_paused() || !LotatoConfig::instance().isIngestReady()) return;
  ensure_worker();
  if (!g_q_mtx || !g_worker) return;

  static uint32_t s_last_gc_ms = 0;
  uint32_t tms = millis();
  if (node_store && (tms - s_last_gc_ms >= 60000u || s_last_gc_ms == 0)) {
    s_last_gc_ms = tms;
    node_store->gcSweepStale();
  }

  xSemaphoreTake(g_q_mtx, portMAX_DELAY);
  if (node_store && self_pub_key && g_batch_len == 0) {
    try_build_batch_from_store(*node_store, self_pub_key);
  }
  bool pending = g_batch_len > 0;
  xSemaphoreGive(g_q_mtx);

  if (pending && WiFi.status() == WL_CONNECTED) notify_worker();
}

void LotatoIngestor::setPaused(bool paused) {
  losettings::LoSettings("lotato").setBool("ingest.paused", paused);
  LotatoConfig::instance().refreshFromLoSettings();
}
bool LotatoIngestor::isPaused() const { return ingest_paused(); }
int LotatoIngestor::lastHttpCode() const { return g_last_http_code; }

#endif // ESP32
