#include <helpers/esp32/LotatoDebug.h>

#ifdef ESP32

#include <Arduino.h>
#include <cstring>

namespace {

void print_cstr_preview(const char* label, const char* s) {
  if (!s) s = "";
  size_t n = strlen(s);
  Serial.printf("Lotato: %s len=%u heap=%u\r\n", label, (unsigned)n, (unsigned)ESP.getFreeHeap());
  if (n == 0) return;
  constexpr unsigned kMax = 200;
  unsigned show = (unsigned)((n > kMax) ? kMax : n);
  Serial.printf("Lotato: %s preview: \"%.*s\"%s\r\n", label, (int)show, s, n > kMax ? "…" : "");
}

}  // namespace

void lotato_dbg_mesh_txt_rx(uint32_t sender_ts, uint8_t raw_flags_byte, uint8_t txt_type_flags, size_t decrypt_len,
                            int is_retry, const char* cmd_cstr) {
  if (!lotato_dbg_active()) return;
  Serial.printf(
      "Lotato: mesh txt rx: ts=%lu raw_b4=0x%02x txt_flags=%u decrypt_len=%zu is_retry=%d heap=%u\r\n",
      (unsigned long)sender_ts, (unsigned)raw_flags_byte, (unsigned)txt_type_flags, decrypt_len, is_retry,
      (unsigned)ESP.getFreeHeap());
  print_cstr_preview("mesh txt cmd", cmd_cstr);
}

void lotato_dbg_mesh_after_handle(const char* reply_cstr, int will_enqueue_reply_nonempty) {
  if (!lotato_dbg_active()) return;
  size_t n = reply_cstr ? strlen(reply_cstr) : 0;
  Serial.printf("Lotato: mesh after handle: reply_len=%zu will_enqueue=%d heap=%u\r\n", n,
                will_enqueue_reply_nonempty, (unsigned)ESP.getFreeHeap());
  print_cstr_preview("mesh reply[]", reply_cstr);
}

void lotato_dbg_lotato_dispatch_stats(size_t out_len, int truncated) {
  if (!lotato_dbg_active()) return;
  Serial.printf("Lotato: lotato dispatch: out_len=%zu truncated=%d heap=%u\r\n", out_len, truncated,
                (unsigned)ESP.getFreeHeap());
}

void lotato_dbg_mesh_enqueue_short(const char* text) {
  if (!lotato_dbg_active()) return;
  print_cstr_preview("mesh enqueue (from reply[])", text);
}

void lotato_dbg_cli_tx_chunk(uint32_t rtc_ts_used, unsigned chunk_idx, unsigned total_chunks, size_t emit_len,
                             const char* mode, int acl_idx, bool sent) {
  if (!lotato_dbg_active()) return;
  Serial.printf(
      "Lotato: cli tx chunk: rtc_ts=%lu idx=%u/%u emit_len=%zu mode=%s acl=%d sent=%d heap=%u\r\n",
      (unsigned long)rtc_ts_used, chunk_idx, total_chunks, emit_len, mode ? mode : "?", acl_idx, sent ? 1 : 0,
      (unsigned)ESP.getFreeHeap());
}

#endif
