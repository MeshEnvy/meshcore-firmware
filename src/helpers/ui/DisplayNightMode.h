#pragma once

#include "DisplayBuildFlags.h"
#include <stddef.h>
#include <stdint.h>

namespace mesh { namespace ui {

// These values are also the small on-disk mode encoding.
enum DisplayNightMode : uint8_t { NightOff = 0, NightOn = 1, NightAuto = 2 };

#ifdef MESHCORE_HAS_COLOR_DISPLAY
uint8_t displayNightMode();
int16_t displayNightDelayMinutes();
bool displayNightActive();
bool handleDisplayNightCommand(const char* command, char* reply, size_t capacity);
// UTC and position are observations only; refreshing them never writes flash.
void updateDisplayNightMode(uint32_t utc, bool has_location,
                            int32_t latitude_e6 = 0, int32_t longitude_e6 = 0);
#else
inline uint8_t displayNightMode() { return NightOff; }
inline int16_t displayNightDelayMinutes() { return 60; }
inline bool displayNightActive() { return false; }
inline bool handleDisplayNightCommand(const char*, char*, size_t) { return false; }
inline void updateDisplayNightMode(uint32_t, bool, int32_t = 0, int32_t = 0) {}
#endif

} }
