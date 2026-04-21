#include "Queue.h"

#include <cstdlib>
#include <cstring>

namespace lomessage {

struct Queue::Job {
  Job* next;
  char* text;
  size_t total_len;
  size_t offset;
  size_t max_chunk;
  unsigned split_flags;
  unsigned long inter_chunk_delay_ms;
  unsigned long next_send_at;
  void* user_ctx;
  size_t user_ctx_len;
  /** 1-based index for the next wire emission (line-absorb can advance offset without emitting). */
  unsigned emit_index;
};

Queue::Queue() : _head(nullptr), _tail(nullptr) {}

Queue::~Queue() { clear(); }

namespace {
inline void free_job_mem(Queue::Job* j) {
  if (!j) return;
  free(j->text);
  if (j->user_ctx) free(j->user_ctx);
  free(j);
}
}  // namespace

bool Queue::send(const char* text, const void* user_ctx, size_t user_ctx_len,
                 const Options& opts, unsigned long first_send_at_ms) {
  if (!text) return false;
  size_t tlen = strlen(text);
  if (tlen == 0) return false;

  Job* job = (Job*)malloc(sizeof(Job));
  if (!job) return false;
  job->next = nullptr;
  job->text = (char*)malloc(tlen + 1);
  if (!job->text) { free(job); return false; }
  memcpy(job->text, text, tlen + 1);
  job->total_len = tlen;
  job->offset = 0;
  job->max_chunk = opts.max_chunk > 0 ? opts.max_chunk : 1;
  job->split_flags = opts.split_flags;
  job->inter_chunk_delay_ms = opts.inter_chunk_delay_ms;
  job->next_send_at = first_send_at_ms;
  job->user_ctx = nullptr;
  job->user_ctx_len = 0;
  job->emit_index = 0;

  if (user_ctx && user_ctx_len > 0) {
    job->user_ctx = malloc(user_ctx_len);
    if (!job->user_ctx) { free(job->text); free(job); return false; }
    memcpy(job->user_ctx, user_ctx, user_ctx_len);
    job->user_ctx_len = user_ctx_len;
  }

  if (_tail) {
    _tail->next = job;
    _tail = job;
  } else {
    _head = _tail = job;
  }
  return true;
}

void Queue::service(unsigned long now_ms, Sink& sink) {
  if (!_head) return;
  Job* job = _head;
  if ((long)(now_ms - job->next_send_at) < 0) return;

  Chunk seg = next_chunk(job->text, job->total_len, job->offset,
                         job->max_chunk, job->split_flags);
  if (seg.consume_len == 0) {
    _head = job->next;
    if (!_head) _tail = nullptr;
    free_job_mem(job);
    return;
  }

  bool is_final = job->offset + seg.consume_len >= job->total_len;

  SendResult r = SendResult::Sent;
  if (seg.emit_len > 0) {
    unsigned part = job->emit_index + 1;
    size_t approx_total = (job->total_len + job->max_chunk - 1) / job->max_chunk;
    if (approx_total == 0) approx_total = 1;
    if ((size_t)part > approx_total) approx_total = (size_t)part;
    r = sink.sendChunk(reinterpret_cast<const uint8_t*>(job->text + job->offset),
                       seg.emit_len, (size_t)part, approx_total, is_final,
                       job->user_ctx);
  }

  switch (r) {
    case SendResult::Sent:
      job->offset += seg.consume_len;
      if (seg.emit_len > 0) job->emit_index++;
      if (job->offset >= job->total_len) {
        _head = job->next;
        if (!_head) _tail = nullptr;
        free_job_mem(job);
      } else {
        job->next_send_at = now_ms + job->inter_chunk_delay_ms;
      }
      break;
    case SendResult::Retry:
      job->next_send_at = now_ms + job->inter_chunk_delay_ms;
      break;
    case SendResult::Abandon:
      _head = job->next;
      if (!_head) _tail = nullptr;
      free_job_mem(job);
      break;
  }
}

void Queue::clear() {
  while (_head) {
    Job* j = _head;
    _head = j->next;
    free_job_mem(j);
  }
  _tail = nullptr;
}

size_t Queue::pendingJobs() const {
  size_t n = 0;
  for (Job* j = _head; j; j = j->next) n++;
  return n;
}

}  // namespace lomessage
