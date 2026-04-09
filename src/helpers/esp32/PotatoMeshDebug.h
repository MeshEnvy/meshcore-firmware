#pragma once

#ifndef POTATO_MESH_DEBUG
#define POTATO_MESH_DEBUG 0
#endif

#ifdef ESP32
#include <Arduino.h>
#include <helpers/esp32/PotatoMeshConfig.h>
inline bool potato_mesh_dbg_active() {
#if POTATO_MESH_DEBUG
  return true;
#else
  return PotatoMeshConfig::instance().debugEnabled();
#endif
}
/** When debug is on: log full CLI command and reply (may include secrets — for field debug only). */
void potato_mesh_dbg_trace_cli_exchange(const char* route_tag, const char* cmd_snapshot, const char* reply);
#define POTATO_MESH_DBG(F, ...) \
  do { if (potato_mesh_dbg_active()) Serial.printf("PotatoMesh: " F, ##__VA_ARGS__); } while (0)
#define POTATO_MESH_DBG_LN(F, ...) \
  do { if (potato_mesh_dbg_active()) Serial.printf("PotatoMesh: " F "\n", ##__VA_ARGS__); } while (0)
#else
#define POTATO_MESH_DBG(...) ((void)0)
#define POTATO_MESH_DBG_LN(...) ((void)0)
#endif
