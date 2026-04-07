#pragma once

#ifndef POTATO_MESH_DEBUG
#define POTATO_MESH_DEBUG 0
#endif

#if defined(ESP32) && defined(ARDUINO) && defined(POTATO_MESH_INGEST)
#include <Arduino.h>
#include <helpers/esp32/PotatoMeshConfig.h>
inline bool potato_mesh_dbg_active() {
#if POTATO_MESH_DEBUG
  return true;
#else
  return PotatoMeshConfig::instance().debugEnabled();
#endif
}
#define POTATO_MESH_DBG(F, ...)                                                                                        \
  do {                                                                                                                 \
    if (potato_mesh_dbg_active())                                                                                      \
      Serial.printf("PotatoMesh: " F, ##__VA_ARGS__);                                                                  \
  } while (0)
#define POTATO_MESH_DBG_LN(F, ...)                                                                                     \
  do {                                                                                                                 \
    if (potato_mesh_dbg_active())                                                                                      \
      Serial.printf("PotatoMesh: " F "\n", ##__VA_ARGS__);                                                             \
  } while (0)
#elif defined(ESP32) && defined(ARDUINO)
#include <Arduino.h>
#if POTATO_MESH_DEBUG
#define POTATO_MESH_DBG(F, ...) Serial.printf("PotatoMesh: " F, ##__VA_ARGS__)
#define POTATO_MESH_DBG_LN(F, ...) Serial.printf("PotatoMesh: " F "\n", ##__VA_ARGS__)
#else
#define POTATO_MESH_DBG(...) ((void)0)
#define POTATO_MESH_DBG_LN(...) ((void)0)
#endif
#else
#define POTATO_MESH_DBG(...) ((void)0)
#define POTATO_MESH_DBG_LN(...) ((void)0)
#endif
