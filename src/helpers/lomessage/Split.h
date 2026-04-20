#pragma once

#include <stddef.h>

namespace lomessage {

/** Next segment length for transport-sized chunks of a byte stream (UTF-8 safe as raw bytes; no decoding).
 *  When a chunk would use the full @p max_chunk window, shrink it to end at the last newline (0x0A)
 *  in the second half of that window so line-oriented text splits cleanly when possible.
 *
 *  @param data full message (NUL-terminated region is [0,total_len); bytes after NUL ignored)
 *  @param total_len logical length (usually strlen(data))
 *  @param offset bytes already emitted
 *  @param max_chunk maximum segment size (caller-defined, e.g. radio or protocol payload limit)
 *  @return 0 when done or invalid args; otherwise 1..max_chunk */
inline size_t next_chunk_len(const char* data, size_t total_len, size_t offset, size_t max_chunk) {
  if (!data || max_chunk == 0 || offset > total_len) return 0;
  size_t remaining = total_len - offset;
  if (remaining == 0) return 0;
  size_t chunk = remaining < max_chunk ? remaining : max_chunk;
  if (chunk == max_chunk) {
    const int hi = (int)max_chunk - 1;
    const int lo = (int)(max_chunk / 2);
    for (int j = hi; j >= lo; j--) {
      if (data[offset + (size_t)j] == '\n') {
        chunk = (size_t)j + 1;
        break;
      }
    }
  }
  return chunk;
}

}  // namespace lomessage
