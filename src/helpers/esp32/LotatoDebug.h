#pragma once

#ifdef ESP32
#include <Arduino.h>
#include <stddef.h>
#include <helpers/esp32/LotatoConfig.h>
inline bool lotato_dbg_active() { return LotatoConfig::instance().debugEnabled(); }
/** When debug is on: log full CLI command and reply (may include secrets — for field debug only). */
void lotato_dbg_trace_cli_exchange(const char* route_tag, const char* cmd_snapshot, const char* reply);

/** Mesh PAYLOAD_TYPE_TXT_MSG admin path: decrypted frame metadata + command preview (debug only). */
void lotato_dbg_mesh_txt_rx(uint32_t sender_ts, uint8_t raw_flags_byte, uint8_t txt_type_flags, size_t decrypt_len,
                            int is_retry, const char* cmd_cstr);
/** After handleCommand on mesh: what landed in the fixed reply[] and whether we enqueue it as TXT. */
void lotato_dbg_mesh_after_handle(const char* reply_cstr, int will_enqueue_reply_nonempty);
/** Right before enqueueTxtCliReply from reply[] (short mesh replies). */
void lotato_dbg_mesh_enqueue_short(const char* text);
/** After `lotato …` dispatch: heap-buffered CLI output size (may be split into many TXT chunks). */
void lotato_dbg_lotato_dispatch_stats(size_t out_len, int truncated);
/** Each outbound TXT chunk for CLI reply queue (indices are 1-based for human log). */
void lotato_dbg_cli_tx_chunk(uint32_t rtc_ts_used, unsigned chunk_idx, unsigned total_chunks, size_t emit_len,
                             const char* mode, int acl_idx, bool sent);
/** Current task stack headroom (bytes) from `uxTaskGetStackHighWaterMark`; log label @p tag. */
void lotato_dbg_task_stack(const char* tag);
#define LOTATO_DBG(F, ...) \
  do { if (lotato_dbg_active()) Serial.printf("Lotato: " F, ##__VA_ARGS__); } while (0)
#define LOTATO_DBG_LN(F, ...) \
  do { if (lotato_dbg_active()) Serial.printf("Lotato: " F "\n", ##__VA_ARGS__); } while (0)
#else
#define LOTATO_DBG(...) ((void)0)
#define LOTATO_DBG_LN(...) ((void)0)
#endif
