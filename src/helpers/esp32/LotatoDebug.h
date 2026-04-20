#pragma once

#ifndef LOTATO_DEBUG
#define LOTATO_DEBUG 1
#endif

#ifdef ESP32
#include <Arduino.h>
#include <helpers/esp32/LotatoConfig.h>
inline bool lotato_dbg_active() {
#if LOTATO_DEBUG
  return true;
#else
  return LotatoConfig::instance().debugEnabled();
#endif
}
/** When debug is on: log full CLI command and reply (may include secrets — for field debug only). */
void lotato_dbg_trace_cli_exchange(const char* route_tag, const char* cmd_snapshot, const char* reply);
#define LOTATO_DBG(F, ...) \
  do { if (lotato_dbg_active()) Serial.printf("Lotato: " F, ##__VA_ARGS__); } while (0)
#define LOTATO_DBG_LN(F, ...) \
  do { if (lotato_dbg_active()) Serial.printf("Lotato: " F "\n", ##__VA_ARGS__); } while (0)
#else
#define LOTATO_DBG(...) ((void)0)
#define LOTATO_DBG_LN(...) ((void)0)
#endif
