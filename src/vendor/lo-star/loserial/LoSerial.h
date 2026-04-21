#pragma once

#include <Arduino.h>
#include <cstdarg>
#include <cstddef>

namespace loserial {

/**
 * LoSerial - cross-platform console output.
 *
 * Thin wrapper around a default Arduino `Stream&` (typically `Serial`) that owns a consistent
 * output policy: CRLF line endings, chunked writes with `yield()` every few dozen bytes so long
 * lines don't starve USB CDC / WiFi / SoftDevice tasks on ESP32 or nRF52.
 *
 * Intentional callers: LoLog (diagnostics sink) and CLI echo helpers. Not a general "replace all
 * Serial.print" shim; framed binary transports (ArduinoSerialInterface etc.) stay separate.
 */
class LoSerial {
public:
  /** Set the default output stream. Defaults to `Serial` if never called. */
  static void begin(Stream& s);

  /** Current sink (for callers that need to pass a Stream&). */
  static Stream& stream();

  /** Chunked byte write + `yield()` each chunk. Safe for long payloads on ESP32 / nRF52. */
  static void writeChunked(const char* data, size_t len);

  /** Chunked write + CRLF. `s` may be nullptr (no-op). */
  static void printLine(const char* s);

  /** Mesh/USB CLI reply: `"  -> "` prefix + chunked body + CRLF. `reply` may be nullptr (no-op). */
  static void printMeshCliReply(const char* reply);

  /** Printf-style via an internal stack buffer (capped). */
  static void printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

  /** Same as printf for pre-packaged va_list. */
  static void vprintf(const char* fmt, va_list ap);
};

}  // namespace loserial
