#pragma once

#include <cstdarg>
#include <cstddef>

namespace lolog {

/**
 * LoLog - leveled logging with a runtime `verbose` flag persisted via LoSettings/ConfigHub.
 *
 * - Backing store: LoSettings namespace `lolog`, key `verbose` (Bool), registered into ConfigHub.
 * - Hot path never reads LoSettings; an in-RAM cache is refreshed on boot / set / on_change.
 * - Output is routed through `loserial::LoSerial` so line endings + chunked write + yield policy
 *   are consistent across ESP32 and nRF52.
 *
 * Levels:
 *   debug - emitted only when verbose is on
 *   info  - always
 *   warn  - always
 *   error - always
 */
class LoLog {
public:
  /** Register `lolog.verbose` into ConfigHub (idempotent). Call once at boot. */
  static void registerConfigSchema();

  /** Refresh in-RAM verbose cache from LoSettings. Safe to call any time. */
  static void loadFromSettings();

  static bool isVerbose();

  /** Update cache + persist to LoSettings. */
  static void setVerbose(bool on);

  static void debug(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  static void info(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  static void warn(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  static void error(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};

}  // namespace lolog
