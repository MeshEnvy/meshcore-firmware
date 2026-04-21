#include "LoSerial.h"

#include <cstdio>
#include <cstring>

namespace loserial {

namespace {

Stream* g_sink = nullptr;

Stream& sink() {
  if (g_sink) return *g_sink;
  return Serial;
}

constexpr size_t kChunk = 48;

}  // namespace

void LoSerial::begin(Stream& s) { g_sink = &s; }

Stream& LoSerial::stream() { return sink(); }

void LoSerial::writeChunked(const char* data, size_t len) {
  if (!data || len == 0) return;
  Stream& s = sink();
  size_t written = 0;
  while (written < len) {
    size_t take = len - written;
    if (take > kChunk) take = kChunk;
    s.write(reinterpret_cast<const uint8_t*>(data + written), take);
    written += take;
    yield();
  }
}

void LoSerial::printLine(const char* s) {
  if (s) writeChunked(s, strlen(s));
  sink().write(reinterpret_cast<const uint8_t*>("\r\n"), 2);
}

void LoSerial::printMeshCliReply(const char* reply) {
  if (!reply) return;
  Stream& s = sink();
  s.write(reinterpret_cast<const uint8_t*>("  -> "), 5);
  writeChunked(reply, strlen(reply));
  s.write(reinterpret_cast<const uint8_t*>("\r\n"), 2);
}

void LoSerial::printf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

void LoSerial::vprintf(const char* fmt, va_list ap) {
  if (!fmt) return;
  char buf[256];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (n <= 0) return;
  size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
  writeChunked(buf, len);
}

}  // namespace loserial
