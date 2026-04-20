#pragma once

#include <stddef.h>

namespace lomessage {

/** Flags for next_chunk(). */
enum : unsigned {
  CHUNK_DEFAULT = 0u,
  /** For non-final chunks that end exactly at a newline, emit one byte less and consume the '\n'
   *  past it. Useful when the consumer renders each chunk as its own line and appends its own '\n'
   *  (otherwise you get a doubled blank line between chunks at a line boundary). */
  CHUNK_ABSORB_LINE_BOUNDARY = 1u << 0,
};

/** Describes one emitted chunk. @a emit_len bytes should be written to the transport;
 *  the caller should advance its read cursor by @a consume_len (>= @a emit_len). */
struct Chunk {
  size_t emit_len;
  size_t consume_len;
};

/** Next segment length for transport-sized chunks of a byte stream (raw bytes; no decoding).
 *  When a chunk would use the full @p max_chunk window, shrink it to end at the last newline (0x0A)
 *  in the second half of that window so line-oriented text splits cleanly when possible.
 *
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

/** Line-aware variant of next_chunk_len() that distinguishes bytes to emit from bytes to consume.
 *
 *  With CHUNK_ABSORB_LINE_BOUNDARY set, a non-final chunk ending with '\n' is reported with
 *  @a emit_len = raw_len - 1 and @a consume_len = raw_len, so the '\n' is dropped rather than
 *  emitted at the tail of this chunk or left as a leading byte of the next chunk.
 *
 *  The final chunk is always reported as-is (emit_len == consume_len == raw_len).
 *
 *  @return {0,0} when done or invalid args. */
inline Chunk next_chunk(const char* data, size_t total_len, size_t offset, size_t max_chunk,
                        unsigned flags = CHUNK_DEFAULT) {
  Chunk out = {0, 0};
  size_t raw = next_chunk_len(data, total_len, offset, max_chunk);
  if (raw == 0) return out;
  out.emit_len = raw;
  out.consume_len = raw;
  const bool is_final = offset + raw >= total_len;
  if (!is_final && (flags & CHUNK_ABSORB_LINE_BOUNDARY) && data[offset + raw - 1] == '\n') {
    out.emit_len = raw - 1;
  }
  return out;
}

}  // namespace lomessage
