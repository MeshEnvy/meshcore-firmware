#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Split.h"

namespace lomessage {

/** Result of a sink dispatch for one chunk. */
enum class SendResult {
  Sent,     ///< chunk handed off; advance and continue
  Retry,    ///< transport not ready; reschedule this chunk
  Abandon,  ///< fatal for this message; drop the whole job
};

/** Transport-specific hook called by Queue::service() for each outgoing chunk.
 *  Implementations build and transmit one wire frame; lomessage owns splitting, scheduling,
 *  and job lifetime. */
class Sink {
public:
  virtual ~Sink() = default;

  /** @param data       pointer into the queued text (may be 0 bytes when absorbed)
   *  @param len        bytes to transmit
   *  @param chunk_idx  1-based index in the (max-sized) partition of total_len
   *  @param total_chunks count based on max_chunk (approximation)
   *  @param is_final   true when this emission completes the job
   *  @param user_ctx   per-message opaque blob provided at enqueue time (heap copy) */
  virtual SendResult sendChunk(const uint8_t* data, size_t len,
                               size_t chunk_idx, size_t total_chunks,
                               bool is_final,
                               void* user_ctx) = 0;
};

/** Per-message scheduling and split policy. */
struct Options {
  size_t max_chunk;                      ///< per-chunk cap (wire payload budget)
  unsigned long inter_chunk_delay_ms;    ///< spacing between chunks after a successful send or retry
  unsigned split_flags;                  ///< lomessage::CHUNK_* flags
};

/** FIFO of outbound text jobs; each job is chunked on demand through a Sink. */
class Queue {
public:
  Queue();
  ~Queue();
  Queue(const Queue&) = delete;
  Queue& operator=(const Queue&) = delete;

  /** Heap-copy @p text and @p user_ctx; append to the FIFO.
   *  @return false on empty text or OOM. */
  bool send(const char* text,
            const void* user_ctx, size_t user_ctx_len,
            const Options& opts,
            unsigned long first_send_at_ms);

  /** Drive the queue: emit at most one chunk from the head if its schedule is due. */
  void service(unsigned long now_ms, Sink& sink);

  bool empty() const { return _head == nullptr; }
  void clear();
  size_t pendingJobs() const;

  struct Job;  ///< opaque; defined in Queue.cpp

private:
  Job* _head;
  Job* _tail;
};

}  // namespace lomessage
