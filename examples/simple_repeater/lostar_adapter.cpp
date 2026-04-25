#include "lostar_adapter.h"

#if defined(ESP32) && defined(LOTATO_PLATFORM_MESHCORE)

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include <FS.h>
#include <Identity.h>
#include <Mesh.h>
#include <MeshCore.h>
#include <Packet.h>
#include <lofs/adapters/ArduinoFsVolume.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/ClientACL.h>      // OUT_PATH_UNKNOWN
#include <helpers/TxtDataHelpers.h>  // TXT_TYPE_CLI_DATA

// lo-star / lotato / lostar API — all POD-only, no fork-native types.
#include <locommand/Engine.h>
#include <Lotato.h>
#include <lofi/Lofi.h>
#include <lolog/LoLog.h>
#include <lostar/Busy.h>
#include <lostar/Deferred.h>
#include <lostar/Host.h>
#include <lostar/Router.h>
#include <lostar/Types.h>
#include <lomessage/Queue.h>
#include <louser/Guard.h>
#include <louser/LoUser.h>

namespace {


/** Admin TXT_MSG single-packet reply scratch size — matches upstream `main.cpp reply[160]`. */
constexpr size_t kCliReplyCap = 160;
/** Max wire-safe text bytes per TXT_MSG chunk (payload includes a 5-byte TXT header). */
constexpr size_t kMaxTxtChunk = 155;

/** Inter-chunk delay; must be ≥ LoRa airtime for one ~160-byte TXT_MSG at current radio params. */
constexpr unsigned long kInterChunkDelayMs = 200;

/**
 * Upper bound on how long a route slot can sit unused before it's considered stale and eligible
 * for reuse. A wifi scan completes in ~5s; a connect in ~30s; 60s leaves ample headroom.
 */
constexpr uint32_t kRouteStaleMs = 60UL * 1000UL;

/* ── Layout sentinels: catch drift between this adapter TU and the lostar TU. ─────────── */

#if UINTPTR_MAX == 0xFFFFFFFFu
static_assert(sizeof(lostar_TextDm)         == 56,  "lostar_TextDm layout changed");
static_assert(sizeof(lostar_NodeAdvert)     == 104, "lostar_NodeAdvert layout changed");
static_assert(sizeof(lostar_host_ops)       == 20,  "lostar_host_ops layout changed");
static_assert(sizeof(lostar_deferred_reply) == 8,   "lostar_deferred_reply layout changed");
#endif

/* ── Reply route pool. Pool-allocated per ingress; holds the per-caller encryption secret +
 *    out_path so async replies (wifi scan, connect) can reach the original caller long after
 *    ingress returned. Released lazily: a slot is reusable once it has been idle for more than
 *    `kRouteStaleMs` ms (or was never in_use). 4 slots tolerates 4 concurrent async CLI ops. */

struct CliReplyRoute {
  bool     in_use;
  uint32_t last_use_ms;
  uint32_t sender_ts;
  uint8_t  pub_key[32];
  uint8_t  secret[32];
  uint8_t  out_path[MAX_PATH_SIZE];
  uint8_t  out_path_len;
  uint8_t  path_hash_size;
};

constexpr int kRoutePoolSize = 4;
CliReplyRoute g_routes[kRoutePoolSize] = {};

CliReplyRoute *route_alloc() {
  const uint32_t now = millis();
  for (int i = 0; i < kRoutePoolSize; ++i) {
    CliReplyRoute &r = g_routes[i];
    if (!r.in_use || (uint32_t)(now - r.last_use_ms) > kRouteStaleMs) {
      std::memset(&r, 0, sizeof(r));
      r.in_use      = true;
      r.last_use_ms = now;
      return &r;
    }
  }
  return nullptr;
}

void route_touch(CliReplyRoute *r) {
  if (r) r->last_use_ms = millis();
}

/* ── mesh TX sink: pulls jobs off `g_reply_queue` and encodes each chunk as TXT_MSG. ────── */

class MeshcoreReplySink : public lomessage::Sink {
public:
  lomessage::SendResult sendChunk(const uint8_t *data, size_t len, size_t chunk_idx,
                                  size_t total_chunks, bool /*is_final*/,
                                  void *user_ctx) override;
};

mesh::Mesh            *g_mesh = nullptr;
uint8_t                g_self_pub_key[32] = {};
lomessage::Queue       g_reply_queue;
MeshcoreReplySink      g_reply_sink;
bool                   g_installed = false;
static lofs::ArduinoFsVolume s_mc_internal_vol(nullptr);

/* ── Caller identity / advert helpers ─────────────────────────────────────────────────── */

uint32_t nodenum_from_pub_key(const uint8_t *pub_key) {
  return ((uint32_t)pub_key[0] << 24) | ((uint32_t)pub_key[1] << 16) |
         ((uint32_t)pub_key[2] << 8)  | (uint32_t)pub_key[3];
}

uint8_t mc_advert_type_from(uint8_t meshcore_type) {
  switch (meshcore_type) {
    case ADV_TYPE_CHAT:     return LOSTAR_ADVERT_TYPE_CHAT;
    case ADV_TYPE_REPEATER: return LOSTAR_ADVERT_TYPE_REPEATER;
    case ADV_TYPE_ROOM:     return LOSTAR_ADVERT_TYPE_ROOM;
    case ADV_TYPE_SENSOR:   return LOSTAR_ADVERT_TYPE_SENSOR;
    default:                return LOSTAR_ADVERT_TYPE_UNKNOWN;
  }
}

/* ── host_ops implementation ─────────────────────────────────────────────────────────── */

bool enqueue_reply_chunked(const CliReplyRoute &r, const char *text, uint32_t len);

void mc_send_text_dm(void * /*ctx*/, uint32_t to, const char *text, uint32_t len) {
  // Unsolicited DM: meshcore's ingestor heartbeat / pushed messages. Meshcore DMs require the
  // recipient's 32-byte public key and a shared secret, neither of which are available from a
  // 4-byte NodeId alone — so lotato's "send to `to`" contract isn't directly representable.
  // For now this is a no-op (lotato heartbeats are HTTP, not mesh). Keep a debug log so the next
  // caller surfaces the gap instead of silently dropping.
  (void)to;
  (void)text;
  (void)len;
  ::lolog::LoLog::debug("lostar.mc", "send_text_dm ignored (meshcore requires pub_key+secret)");
}

uint32_t mc_self_nodenum(void * /*ctx*/) { return nodenum_from_pub_key(g_self_pub_key); }

int mc_self_pubkey(void * /*ctx*/, uint8_t out[32]) {
  std::memcpy(out, g_self_pub_key, 32);
  return 1;
}

/* ── deferred reply: route_ctx is a pool-owned CliReplyRoute*. Fire enqueues a chunked job
 *    and touches the slot's timestamp; the slot itself is reclaimed lazily by `route_alloc`
 *    when it has been idle for longer than `kRouteStaleMs`. */

void mc_fire_reply(void *route_ctx, const char *text, uint32_t len) {
  auto *r = static_cast<CliReplyRoute *>(route_ctx);
  if (!r || !r->in_use || !text) return;
  (void)len;  // queue copies the c-string verbatim; len is advisory
  enqueue_reply_chunked(*r, text, len);
  route_touch(r);
}

/* ── reply-queue enqueue + sink ─────────────────────────────────────────────────────── */

bool enqueue_reply_chunked(const CliReplyRoute &r, const char *text, uint32_t /*len*/) {
  lomessage::Options opts;
  opts.max_chunk            = kMaxTxtChunk;
  opts.inter_chunk_delay_ms = kInterChunkDelayMs;
  opts.split_flags          = lomessage::CHUNK_ABSORB_LINE_BOUNDARY;

  const bool ok = g_reply_queue.send(text, &r, sizeof(r), opts, millis());
  if (!ok) {
    ::lolog::LoLog::debug("lostar.mc", "reply queue enqueue FAILED (empty or OOM)");
  }
  return ok;
}

lomessage::SendResult MeshcoreReplySink::sendChunk(const uint8_t *data, size_t len,
                                                   size_t chunk_idx, size_t total_chunks,
                                                   bool /*is_final*/, void *user_ctx) {
  auto *ctx = static_cast<CliReplyRoute *>(user_ctx);
  if (!ctx || !g_mesh) return lomessage::SendResult::Abandon;
  if (len == 0 || len > kMaxTxtChunk) {
    ::lolog::LoLog::debug("lostar.mc", "reply tx: BAD emit_len=%u (max=%u) — abandon job",
                          (unsigned)len, (unsigned)kMaxTxtChunk);
    return lomessage::SendResult::Abandon;
  }

  uint8_t        temp[5 + kMaxTxtChunk];
  const uint32_t ts = g_mesh->getRTCClock()->getCurrentTimeUnique();
  const uint32_t stamp = (ts == ctx->sender_ts) ? ts + 1 : ts;
  std::memcpy(temp, &stamp, 4);
  temp[4] = (TXT_TYPE_CLI_DATA << 2);
  std::memcpy(&temp[5], data, len);

  mesh::Identity recipient(ctx->pub_key);
  mesh::Packet  *pkt = g_mesh->createDatagram(PAYLOAD_TYPE_TXT_MSG, recipient, ctx->secret, temp,
                                              5 + len);
  if (!pkt) return lomessage::SendResult::Retry;
  if (ctx->out_path_len == OUT_PATH_UNKNOWN) {
    g_mesh->sendFlood(pkt, 0, ctx->path_hash_size);
  } else {
    g_mesh->sendDirect(pkt, ctx->out_path, ctx->out_path_len, 0);
  }

  ::lolog::LoLog::debug("lostar.mc", "reply tx: chunk %u/%u bytes=%u path_len=%u",
                        (unsigned)chunk_idx, (unsigned)total_chunks, (unsigned)len,
                        (unsigned)ctx->out_path_len);
  return lomessage::SendResult::Sent;
}

/* ── busy hint: keep the host awake while reply chunks are still in flight. ─────────── */

bool mc_reply_queue_busy(void * /*ctx*/) { return !g_reply_queue.empty(); }

void apply_mc_cli_policy() {
  auto &rt = lostar::router();
  if (auto *eng = rt.engineByName("lotato")) eng->setRootGuard(&louser::require_admin);
  if (auto *eng = rt.engineByName("config")) eng->setRootGuard(&louser::require_admin);
  if (auto *eng = rt.engineByName("wifi")) eng->setRootGuard(&louser::require_admin);
}

}  // namespace

/* ── public entry points ────────────────────────────────────────────────────────────── */

void lostar_mc_install(mesh::Mesh *mesh, fs::FS *internal_fs, const uint8_t self_pub_key[32]) {
  if (g_installed) return;
  g_installed = true;

  g_mesh = mesh;
  if (self_pub_key) std::memcpy(g_self_pub_key, self_pub_key, sizeof(g_self_pub_key));

  lostar_host_ops ops{};
  ops.size         = (uint32_t)sizeof(ops);
  ops.send_text_dm = &mc_send_text_dm;
  ops.self_nodenum = &mc_self_nodenum;
  ops.self_pubkey  = &mc_self_pubkey;
  ops.ctx          = nullptr;
  lostar_install_host(&ops);

  lofs::FsVolume *internal_vol = nullptr;
  if (internal_fs) {
    s_mc_internal_vol.bindFs(internal_fs);
    internal_vol = &s_mc_internal_vol;
  }
  lotato::init(LOSTAR_PROTOCOL_MESHCORE, internal_vol);
  louser::init();
  lofi::init();
  apply_mc_cli_policy();

  lostar_register_busy_hint(&mc_reply_queue_busy, nullptr);

  UBaseType_t words = uxTaskGetStackHighWaterMark(nullptr);
  ::lolog::LoLog::info("lostar.mc", "install: free_bytes=%u",
                       (unsigned)(words * sizeof(StackType_t)));
}

void lostar_mc_on_advert(const mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                         const uint8_t *app_data, std::size_t app_data_len) {
  if (!g_installed) return;

  AdvertDataParser parser(app_data, app_data_len);
  if (!parser.isValid()) return;

  lostar_NodeAdvert adv{};
  adv.protocol       = LOSTAR_PROTOCOL_MESHCORE;
  adv.advert_type    = mc_advert_type_from(parser.getType());
  adv.num            = nodenum_from_pub_key(id.pub_key);
  adv.last_heard     = timestamp;
  adv.latitude_i     = parser.hasLatLon() ? parser.getIntLat() : 0;
  adv.longitude_i    = parser.hasLatLon() ? parser.getIntLon() : 0;
  adv.hw_model       = 0;
  adv.public_key_len = 32;
  std::memcpy(adv.public_key, id.pub_key, 32);

  const char *name = parser.hasName() ? parser.getName() : "";
  std::strncpy(adv.long_name, name, sizeof(adv.long_name) - 1);
  adv.long_name[sizeof(adv.long_name) - 1] = '\0';
  adv.short_name[0]                        = '\0';

  char id_hex[9];
  static const char *const hexd = "0123456789abcdef";
  for (int i = 0; i < 4; i++) {
    id_hex[i * 2]     = hexd[id.pub_key[i] >> 4];
    id_hex[i * 2 + 1] = hexd[id.pub_key[i] & 0xf];
  }
  id_hex[8] = '\0';
  ::lolog::LoLog::debug("lostar.mc", "advert: !%s name=\"%.32s\" type=%u hops=%u ts=%lu gps=%s",
                        id_hex, adv.long_name, (unsigned)parser.getType(),
                        (unsigned)(packet ? packet->path_len : 0), (unsigned long)timestamp,
                        parser.hasLatLon() ? "yes" : "no");

  lostar_ingress_node_advert(&adv);
}

bool lostar_mc_on_admin_txt(uint32_t sender_ts, const uint8_t pub_key[32],
                            const uint8_t secret[32], const uint8_t *out_path,
                            uint8_t out_path_len, uint8_t path_hash_size, char *command,
                            bool is_retry) {
  if (!g_installed) return false;
  if (is_retry) return false;

  char cmd_buf[256];
  std::strncpy(cmd_buf, command, sizeof(cmd_buf) - 1);
  cmd_buf[sizeof(cmd_buf) - 1] = '\0';

  auto &rt = lostar::router();
  if (!rt.matchesAnyRoot(cmd_buf) && !rt.matchesGlobalHelp(cmd_buf)) return false;

  if (::lolog::LoLog::isVerbose()) {
    const size_t cmd_len = std::strlen(cmd_buf);
    const unsigned show  = cmd_len > 200u ? 200u : (unsigned)cmd_len;
    ::lolog::LoLog::debug("lostar.mc", "admin txt cmd len=%u preview: \"%.*s\"%s",
                          (unsigned)cmd_len, (int)show, cmd_buf,
                          cmd_len > 200u ? "..." : "");
  }

  CliReplyRoute *r = route_alloc();
  if (!r) {
    // Pool exhausted: emit a direct "busy" reply using a stack-local route so we don't stomp
    // an in-flight slot. Still consumes the command (upstream path skipped).
    CliReplyRoute fallback{};
    fallback.in_use        = true;
    fallback.sender_ts     = sender_ts;
    fallback.out_path_len  = out_path_len;
    fallback.path_hash_size = path_hash_size;
    std::memcpy(fallback.pub_key, pub_key, 32);
    std::memcpy(fallback.secret,  secret,  32);
    std::memcpy(fallback.out_path, out_path, sizeof(fallback.out_path));
    enqueue_reply_chunked(fallback, "Err - busy (reply pool full)", 0);
    return true;
  }

  r->sender_ts      = sender_ts;
  r->out_path_len   = out_path_len;
  r->path_hash_size = path_hash_size;
  std::memcpy(r->pub_key, pub_key, 32);
  std::memcpy(r->secret,  secret,  32);
  std::memcpy(r->out_path, out_path, sizeof(r->out_path));

  lostar_deferred_reply d{};
  d.fire      = &mc_fire_reply;
  d.route_ctx = r;
  lostar_ingress_attach_deferrer(&d);
  lostar_ingress_set_host_token(r);

  lostar_TextDm dm{};
  dm.from            = nodenum_from_pub_key(pub_key);
  dm.to              = mc_self_nodenum(nullptr);
  dm.rx_time         = sender_ts;
  dm.payload         = reinterpret_cast<const uint8_t *>(cmd_buf);
  dm.payload_len     = (uint32_t)std::strlen(cmd_buf);
  dm.from_pubkey_len = 32;
  std::memcpy(dm.from_pubkey, pub_key, 32);

  const bool consumed = lostar_ingress_text_dm(&dm);

  lostar_ingress_attach_deferrer(nullptr);
  lostar_ingress_set_host_token(nullptr);
  (void)consumed;
  return true;
}

void lostar_mc_tick() {
  if (!g_installed) return;
  g_reply_queue.service(millis(), g_reply_sink);
  lostar_tick();
}

bool lostar_mc_is_busy() {
  if (!g_installed) return false;
  return lostar_is_busy();
}

#endif  // ESP32 && LOTATO_PLATFORM_MESHCORE
