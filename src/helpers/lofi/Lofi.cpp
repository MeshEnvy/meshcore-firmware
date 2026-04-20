#include <helpers/lofi/Lofi.h>

#ifdef ESP32

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_http_client.h>
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);

#include <losettings/LoSettings.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifndef LOFI_HTTP_TIMEOUT_MS
#define LOFI_HTTP_TIMEOUT_MS 12000
#endif
#ifndef LOFI_HTTP_CONNECT_TIMEOUT_MS
#define LOFI_HTTP_CONNECT_TIMEOUT_MS 8000
#endif
#ifndef LOFI_WIFI_FAILOVER_DEBOUNCE_MS
#define LOFI_WIFI_FAILOVER_DEBOUNCE_MS 1500
#endif
#ifndef LOFI_WIFI_SCAN_TIMEOUT_MS
#define LOFI_WIFI_SCAN_TIMEOUT_MS 30000
#endif

extern "C" __attribute__((weak)) void lofi_log(const char*) {}
extern "C" __attribute__((weak)) void lofi_async_busy(bool) {}

namespace {

void dbg(const char* fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  lofi_log(buf);
}

constexpr int kScanMax = 20;

struct ScanEntry {
  char ssid[33];
  int32_t rssi;
};
static ScanEntry s_scan[kScanMax];
static int s_scan_count = 0;

enum class WifiScanPhase : uint8_t { Idle, DisconnectWait, Scanning };
static WifiScanPhase s_wscan_phase = WifiScanPhase::Idle;
static uint32_t s_wscan_t0 = 0;
static bool s_wscan_results_ready = false;

static int cmp_known_by_time(const void* a, const void* b) {
  const KnownWifi* x = (const KnownWifi*)a;
  const KnownWifi* y = (const KnownWifi*)b;
  if (x->last_connected != y->last_connected) return x->last_connected > y->last_connected ? -1 : 1;
  return strcmp(x->ssid, y->ssid);
}

static void scan_rssi_bars(int32_t rssi, char out[7]) {
  int lv = rssi >= -50 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
  out[0] = '[';
  for (int b = 0; b < 4; b++) out[1 + b] = b < lv ? '|' : '.';
  out[5] = ']';
  out[6] = '\0';
}

static bool is_https(const char* url) { return url && strncmp(url, "https://", 8) == 0; }

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

static WiFiClient g_plain;
static HTTPClient g_http;

struct HttpSession {
  char full_url[256];
  char bearer[200];
  uint32_t ip4;
  bool active;
};
static HttpSession g_sess{};

static void reset_session() {
  g_http.setReuse(false);
  g_http.end();
  g_plain.stop();
  g_sess.active = false;
  g_sess.full_url[0] = '\0';
}

static bool session_matches(const char* full_url, const char* bearer) {
  if (!g_sess.active) return false;
  if (strcmp(g_sess.full_url, full_url) != 0) return false;
  if (strcmp(g_sess.bearer, bearer ? bearer : "") != 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  return (uint32_t)WiFi.localIP() == g_sess.ip4;
}

static void capture_session(const char* full_url, const char* bearer) {
  strncpy(g_sess.full_url, full_url, sizeof(g_sess.full_url) - 1);
  g_sess.full_url[sizeof(g_sess.full_url) - 1] = '\0';
  strncpy(g_sess.bearer, bearer ? bearer : "", sizeof(g_sess.bearer) - 1);
  g_sess.bearer[sizeof(g_sess.bearer) - 1] = '\0';
  g_sess.ip4 = (uint32_t)WiFi.localIP();
  g_sess.active = true;
}

static lofi::HttpResult post_https(const char* url, const char* bearer, const char* body, uint16_t n,
                                    const char* hdr_name, const char* hdr_val) {
  lofi::HttpResult r{0, 0};
  esp_http_client_config_t cfg{};
  cfg.url = url;
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = (int)LOFI_HTTP_TIMEOUT_MS;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    dbg("lofi https: init failed");
    return r;
  }
  esp_http_client_set_header(client, "Content-Type", "application/json");
  if (bearer && bearer[0]) {
    char auth[220];
    snprintf(auth, sizeof(auth), "Bearer %s", bearer);
    esp_http_client_set_header(client, "Authorization", auth);
  }
  if (hdr_name && hdr_name[0] && hdr_val) esp_http_client_set_header(client, hdr_name, hdr_val);

  if (esp_http_client_set_post_field(client, body, (int)n) != ESP_OK) {
    esp_http_client_cleanup(client);
    return r;
  }
  esp_err_t err = esp_http_client_perform(client);
  r.status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  r.err = (int)err;
  if (err != ESP_OK) dbg("lofi https: err=%s", esp_err_to_name(err));
  return r;
}

static lofi::HttpResult post_plain(const char* url, const char* bearer, const char* body, uint16_t n,
                                    const char* hdr_name, const char* hdr_val) {
  lofi::HttpResult r{0, 0};
  if (!session_matches(url, bearer)) reset_session();

  g_http.setReuse(true);
  g_http.setConnectTimeout((int32_t)LOFI_HTTP_CONNECT_TIMEOUT_MS);
  g_http.setTimeout((uint32_t)LOFI_HTTP_TIMEOUT_MS);

  bool ok_begin = g_sess.active;
  if (!ok_begin) ok_begin = g_http.begin(g_plain, url);
  if (!ok_begin) {
    reset_session();
    return r;
  }
  if (!g_sess.active) capture_session(url, bearer);

  g_http.addHeader("Content-Type", "application/json");
  if (bearer && bearer[0]) {
    char auth[220];
    snprintf(auth, sizeof(auth), "Bearer %s", bearer);
    g_http.addHeader("Authorization", auth);
  }
  if (hdr_name && hdr_name[0] && hdr_val) g_http.addHeader(hdr_name, hdr_val);

  int code = g_http.POST((uint8_t*)body, (size_t)n);
  r.status = code;
  discard_http_body(g_http);
  if (code < 0 || code == 401) reset_session();
  return r;
}

static bool wifi_reason_failover(uint8_t reason) {
  switch (reason) {
    case 201:
    case 202:
    case 203:
    case 204:
      return true;
    default:
      return false;
  }
}

}  // namespace

namespace lofi {

Lofi& Lofi::instance() {
  static Lofi inst;
  return inst;
}

Lofi::Lofi() : _db("lofi") {}

void Lofi::ensureTables() {
  if (_tables_registered) return;
  _db.registerTable("known_wifi", &KnownWifi_msg, sizeof(KnownWifi));
  _db.registerTable("scan_seen", &ScanSeen_msg, sizeof(ScanSeen));
  _tables_registered = true;
}

void Lofi::rememberKnownUnlocked(const char* ssid, const char* psk) {
  if (!ssid || !ssid[0]) return;
  lodb_uuid_t id = lodb_new_uuid(ssid, 0);
  KnownWifi k = KnownWifi_init_zero;
  strncpy(k.ssid, ssid, sizeof(k.ssid) - 1);
  strncpy(k.psk, psk ? psk : "", sizeof(k.psk) - 1);
  k.last_connected = (uint32_t)(millis() / 1000);
  KnownWifi ex = KnownWifi_init_zero;
  if (_db.get("known_wifi", id, &ex) == LODB_OK) {
    k.favorite = ex.favorite;
    k.connect_count = ex.connect_count + 1;
    (void)_db.update("known_wifi", id, &k);
  } else {
    k.connect_count = 1;
    (void)_db.insert("known_wifi", id, &k);
  }
  while (_db.count("known_wifi") > 8) {
    auto rows = _db.select("known_wifi", LoDbFilter(), cmp_known_by_time, 0);
    if (rows.empty()) break;
    KnownWifi* oldest = (KnownWifi*)rows.back();
    char ssid_copy[33];
    strncpy(ssid_copy, oldest->ssid, sizeof(ssid_copy) - 1);
    ssid_copy[sizeof(ssid_copy) - 1] = '\0';
    LoDb::freeRecords(rows);
    (void)_db.deleteRecord("known_wifi", lodb_new_uuid(ssid_copy, 0));
  }
}

void Lofi::saveWifiConnect(const char* ssid, const char* psk) {
  ensureTables();
  losettings::LoSettings st("lofi");
  st.setString("active.ssid", ssid ? ssid : "");
  st.setString("active.psk", psk ? psk : "");
  rememberKnownUnlocked(ssid, psk);
}

uint8_t Lofi::knownWifiCount() {
  ensureTables();
  int c = _db.count("known_wifi");
  if (c < 0) return 0;
  if (c > 255) return 255;
  return (uint8_t)c;
}

bool Lofi::getKnownWifi(uint8_t idx, char* out_ssid, size_t ssid_cap, char* out_pwd, size_t pwd_cap) {
  ensureTables();
  if (!out_ssid || ssid_cap < 1) return false;
  auto rows = _db.select("known_wifi", LoDbFilter(), cmp_known_by_time, 0);
  if (idx >= rows.size()) {
    LoDb::freeRecords(rows);
    return false;
  }
  const KnownWifi* w = (const KnownWifi*)rows[idx];
  strncpy(out_ssid, w->ssid, ssid_cap - 1);
  out_ssid[ssid_cap - 1] = '\0';
  if (out_pwd && pwd_cap > 0) {
    strncpy(out_pwd, w->psk, pwd_cap - 1);
    out_pwd[pwd_cap - 1] = '\0';
  }
  LoDb::freeRecords(rows);
  return true;
}

bool Lofi::getKnownWifiPassword(const char* ssid, char* out_pwd, size_t pwd_cap) {
  ensureTables();
  if (!ssid || !out_pwd || pwd_cap < 1) return false;
  lodb_uuid_t id = lodb_new_uuid(ssid, 0);
  KnownWifi w = KnownWifi_init_zero;
  if (_db.get("known_wifi", id, &w) != LODB_OK) return false;
  strncpy(out_pwd, w.psk, pwd_cap - 1);
  out_pwd[pwd_cap - 1] = '\0';
  return true;
}

bool Lofi::forgetKnownWifi(const char* ssid) {
  ensureTables();
  if (!ssid || !ssid[0]) return false;
  lodb_uuid_t id = lodb_new_uuid(ssid, 0);
  bool ok = _db.deleteRecord("known_wifi", id) == LODB_OK;
  if (ok) {
    char active[33];
    losettings::LoSettings st("lofi");
    st.getString("active.ssid", active, sizeof(active), "");
    if (strcmp(active, ssid) == 0) {
      st.setString("active.ssid", "");
      st.setString("active.psk", "");
    }
  }
  return ok;
}

void Lofi::resumeStaSavedCredentials() {
  char s[33]{}, p[65]{};
  losettings::LoSettings st("lofi");
  st.getString("active.ssid", s, sizeof(s), "");
  st.getString("active.psk", p, sizeof(p), "");
  if (s[0] == '\0') return;
  const char* pw = p[0] ? p : nullptr;
  WiFi.begin(s, pw);
  WiFi.setSleep(WIFI_PS_NONE);
}

void Lofi::getActiveCredentials(char* ssid_out, size_t ssid_cap, char* psk_out, size_t psk_cap) {
  losettings::LoSettings st("lofi");
  if (ssid_out && ssid_cap) st.getString("active.ssid", ssid_out, ssid_cap, "");
  if (psk_out && psk_cap) st.getString("active.psk", psk_out, psk_cap, "");
}

HttpResult Lofi::httpPost(const char* url, const char* bearer, const char* body, uint16_t n, const char* hn,
                          const char* hv) {
  HttpResult r{0, 0};
  if (!url || !body) return r;
  if (is_https(url)) return post_https(url, bearer, body, n, hn, hv);
  return post_plain(url, bearer, body, n, hn, hv);
}

void Lofi::resetHttpTransport() { reset_session(); }

void Lofi::setScanCompleteCallback(void (*fn)(void*, const char*), void* ctx) {
  _scan_cb = fn;
  _scan_cb_ctx = ctx;
}

void Lofi::registerWifiHandlers() {
  if (_wifi_handlers_registered) return;
  _wifi_handlers_registered = true;

  static bool g_dns_applied = false;
  Lofi* self = this;
  WiFi.onEvent([self](WiFiEvent_t event, WiFiEventInfo_t info) {
    (void)info;
    if (!self->_force_public_dns) return;
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      g_dns_applied = false;
      return;
    }
    if (event != ARDUINO_EVENT_WIFI_STA_GOT_IP || g_dns_applied) return;
    g_dns_applied = true;
    const IPAddress d1(1, 1, 1, 1), d2(8, 8, 8, 8);
    (void)WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), d1, d2);
  });

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_START:
        dbg("wifi sta: started");
        break;
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        dbg("wifi sta: associated ch=%u", (unsigned)info.wifi_sta_connected.channel);
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        dbg("wifi sta: got ip %s", WiFi.localIP().toString().c_str());
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        dbg("wifi sta: disconnected reason=%u", (unsigned)info.wifi_sta_disconnected.reason);
        break;
      default:
        break;
    }
  });

  static uint32_t g_last_action_ms = 0;
  WiFi.onEvent([self](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
    if (self->_failover_suppress) return;

    uint8_t n = self->knownWifiCount();
    if (n < 2) {
      WiFi.setAutoReconnect(true);
      return;
    }
    WiFi.setAutoReconnect(false);

    uint32_t now = millis();
    if ((int32_t)(now - g_last_action_ms) < (int32_t)LOFI_WIFI_FAILOVER_DEBOUNCE_MS) return;
    g_last_action_ms = now;

    uint8_t reason = info.wifi_sta_disconnected.reason;
    if (!wifi_reason_failover(reason)) {
      WiFi.reconnect();
      return;
    }

    char ev_ssid[33]{};
    uint8_t len = info.wifi_sta_disconnected.ssid_len;
    if (len > sizeof(info.wifi_sta_disconnected.ssid)) len = sizeof(info.wifi_sta_disconnected.ssid);
    if (len > sizeof(ev_ssid) - 1) len = sizeof(ev_ssid) - 1;
    if (len > 0) {
      memcpy(ev_ssid, info.wifi_sta_disconnected.ssid, len);
      ev_ssid[len] = '\0';
    }

    uint8_t idx = 0;
    bool found = false;
    for (uint8_t i = 0; i < n; i++) {
      char s[33], p[65];
      if (!self->getKnownWifi(i, s, sizeof(s), p, sizeof(p))) continue;
      if (ev_ssid[0] != '\0' && strcmp(s, ev_ssid) == 0) {
        idx = i;
        found = true;
        break;
      }
    }
    if (!found) {
      char cur[33];
      losettings::LoSettings st("lofi");
      st.getString("active.ssid", cur, sizeof(cur), "");
      for (uint8_t i = 0; i < n; i++) {
        char s[33], p[65];
        if (!self->getKnownWifi(i, s, sizeof(s), p, sizeof(p))) continue;
        if (strcmp(s, cur) == 0) {
          idx = i;
          break;
        }
      }
    }

    uint8_t next = (uint8_t)((idx + 1) % n);
    char ssid[33], pwd[65];
    if (!self->getKnownWifi(next, ssid, sizeof(ssid), pwd, sizeof(pwd))) return;
    const char* pw = pwd[0] ? pwd : nullptr;
    dbg("wifi sta: failover reason=%u idx %u -> %u ssid=%.32s", (unsigned)reason, (unsigned)idx,
        (unsigned)next, ssid);
    WiFi.disconnect(false, false);
    WiFi.begin(ssid, pw);
    WiFi.setSleep(WIFI_PS_NONE);
  });
}

void Lofi::begin() {
  ensureTables();
  registerWifiHandlers();
}

static void wifi_scan_fill_from_driver(int n) {
  s_scan_count = 0;
  for (int i = 0; i < n && s_scan_count < kScanMax; i++) {
    String ss = WiFi.SSID(i);
    if (ss.length() == 0) continue;
    int32_t rssi = WiFi.RSSI(i);
    bool found = false;
    for (int j = 0; j < s_scan_count; j++) {
      if (strcmp(s_scan[j].ssid, ss.c_str()) == 0) {
        if (rssi > s_scan[j].rssi) s_scan[j].rssi = rssi;
        found = true;
        break;
      }
    }
    if (!found) {
      snprintf(s_scan[s_scan_count].ssid, sizeof(s_scan[0].ssid), "%s", ss.c_str());
      s_scan[s_scan_count].rssi = rssi;
      s_scan_count++;
    }
  }
  for (int a = 0; a < s_scan_count - 1; a++) {
    for (int b = a + 1; b < s_scan_count; b++) {
      if (s_scan[b].rssi > s_scan[a].rssi) {
        ScanEntry t = s_scan[a];
        s_scan[a] = s_scan[b];
        s_scan[b] = t;
      }
    }
  }
}

void Lofi::formatScanBody(lomessage::Buffer& buf) {
  if (s_scan_count == 0) {
    buf.append("No scan results. Request a scan, then list.\n");
    return;
  }
  buf.appendf("WiFi scan (%d nets):\n", s_scan_count);
  for (int i = 0; i < s_scan_count; i++) {
    char bars[7];
    scan_rssi_bars(s_scan[i].rssi, bars);
    if (!buf.appendf("%d. %s %s\n", i + 1, s_scan[i].ssid, bars)) break;
  }
}

int Lofi::scanSnapshotCount() { return s_scan_count; }

bool Lofi::scanSnapshotEntry(int idx, char ssid_out[33], int32_t* rssi_out) {
  if (idx < 0 || idx >= s_scan_count || !ssid_out) return false;
  strncpy(ssid_out, s_scan[idx].ssid, 32);
  ssid_out[32] = '\0';
  if (rssi_out) *rssi_out = s_scan[idx].rssi;
  return true;
}

void Lofi::serviceWifiScan() {
  switch (s_wscan_phase) {
    case WifiScanPhase::Idle:
      break;
    case WifiScanPhase::DisconnectWait:
      if ((int32_t)(millis() - s_wscan_t0) < 120) break;
      {
        int16_t started = WiFi.scanNetworks(true, false, false, 300);
        if (started == WIFI_SCAN_FAILED) {
          WiFi.scanDelete();
          staFailoverSuppress(false);
          resumeStaSavedCredentials();
          s_wscan_phase = WifiScanPhase::Idle;
          s_wscan_results_ready = false;
          if (_scan_cb) _scan_cb(_scan_cb_ctx, "Err - WiFi scan start failed");
          lofi_async_busy(false);
        } else {
          s_wscan_phase = WifiScanPhase::Scanning;
        }
      }
      break;
    case WifiScanPhase::Scanning: {
      if ((uint32_t)(millis() - s_wscan_t0) > LOFI_WIFI_SCAN_TIMEOUT_MS) {
        WiFi.scanDelete();
        s_scan_count = 0;
        staFailoverSuppress(false);
        resumeStaSavedCredentials();
        s_wscan_phase = WifiScanPhase::Idle;
        if (_scan_cb) _scan_cb(_scan_cb_ctx, "Err - WiFi scan timed out");
        lofi_async_busy(false);
        break;
      }
      int16_t cnt = WiFi.scanComplete();
      if (cnt == WIFI_SCAN_RUNNING) break;
      if (cnt == WIFI_SCAN_FAILED) {
        WiFi.scanDelete();
        s_scan_count = 0;
        s_wscan_results_ready = false;
        if (_scan_cb) _scan_cb(_scan_cb_ctx, "Err - WiFi scan failed");
      } else {
        wifi_scan_fill_from_driver((int)cnt);
        WiFi.scanDelete();
        s_wscan_results_ready = true;
        if (_scan_cb) {
          lomessage::Buffer scan_buf;
          formatScanBody(scan_buf);
          _scan_cb(_scan_cb_ctx, scan_buf.c_str());
        }
      }
      staFailoverSuppress(false);
      resumeStaSavedCredentials();
      s_wscan_phase = WifiScanPhase::Idle;
      lofi_async_busy(false);
      break;
    }
  }
}

void Lofi::requestWifiScan() {
  serviceWifiScan();
  if (s_wscan_phase != WifiScanPhase::Idle) return;
  if (s_wscan_results_ready) {
    s_wscan_results_ready = false;
    return;
  }
  staFailoverSuppress(true);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  s_wscan_t0 = millis();
  s_wscan_phase = WifiScanPhase::DisconnectWait;
  lofi_async_busy(true);
}

}  // namespace lofi

#endif
