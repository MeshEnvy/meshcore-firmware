#pragma once

#include <stddef.h>
#include <stdarg.h>

namespace lomessage {

/** Heap-backed growable byte buffer for assembling long text (e.g. multi-line responses).
 *  Transport-agnostic: no I/O or product-specific types. Hard-capped at @p max_bytes. */
class Buffer {
public:
  explicit Buffer(size_t max_bytes = 4096);
  ~Buffer();

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  void clear();

  bool append(const char* s);
  bool append(const char* s, size_t n);
  bool append_char(char c);
  bool appendf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

  const char* c_str() const { return _buf ? _buf : ""; }
  size_t length() const { return _len; }
  bool truncated() const { return _truncated; }
  size_t max_bytes() const { return _max; }

private:
  bool grow_for(size_t extra);

  char* _buf;
  size_t _len;
  size_t _cap;
  size_t _max;
  bool _truncated;
};

}  // namespace lomessage
