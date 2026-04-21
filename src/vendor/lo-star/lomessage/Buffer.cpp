#include "Buffer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lomessage {

Buffer::Buffer(size_t max_bytes)
    : _buf(nullptr), _len(0), _cap(0), _max(max_bytes > 0 ? max_bytes : 1), _truncated(false) {}

Buffer::~Buffer() { clear(); }

void Buffer::clear() {
  free(_buf);
  _buf = nullptr;
  _len = 0;
  _cap = 0;
  _truncated = false;
}

bool Buffer::grow_for(size_t extra) {
  if (_len + extra > _max) return false;
  size_t need = _len + extra + 1;
  if (need <= _cap) return true;
  size_t ncap = _cap ? _cap : 64;
  while (ncap < need) {
    if (ncap >= _max) {
      ncap = need;
      break;
    }
    size_t doubled = ncap * 2;
    if (doubled < ncap) return false;
    ncap = doubled;
  }
  if (ncap > _max + 1) ncap = _max + 1;
  char* nb = (char*)realloc(_buf, ncap);
  if (!nb) return false;
  _buf = nb;
  _cap = ncap;
  return true;
}

bool Buffer::append(const char* s, size_t n) {
  if (!s || n == 0) return true;
  if (_len + n > _max) {
    _truncated = true;
    return false;
  }
  if (!grow_for(n)) {
    _truncated = true;
    return false;
  }
  memcpy(_buf + _len, s, n);
  _len += n;
  _buf[_len] = '\0';
  return true;
}

bool Buffer::append(const char* s) { return s ? append(s, strlen(s)) : true; }

bool Buffer::append_char(char c) { return append(&c, 1); }

bool Buffer::appendf(const char* fmt, ...) {
  char scratch[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(scratch, sizeof(scratch), fmt, ap);
  va_end(ap);
  if (n < 0) return false;
  if ((size_t)n < sizeof(scratch)) return append(scratch, (size_t)n);
  va_start(ap, fmt);
  int big = vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  if (big < 0) return false;
  size_t need = (size_t)big + 1;
  if (need > _max) {
    _truncated = true;
    return false;
  }
  if (!grow_for((size_t)big)) {
    _truncated = true;
    return false;
  }
  va_start(ap, fmt);
  vsnprintf(_buf + _len, _cap - _len, fmt, ap);
  va_end(ap);
  _len += (size_t)big;
  _buf[_len] = '\0';
  return true;
}

}  // namespace lomessage
