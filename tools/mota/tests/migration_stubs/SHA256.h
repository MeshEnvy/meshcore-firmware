#pragma once
#include <stddef.h>

// State-machine tests stop at the EndF lookup, before any hash operation.
class SHA256 {
public:
  void reset();
  void update(const void *, size_t);
  void finalize(void *, size_t);
};
