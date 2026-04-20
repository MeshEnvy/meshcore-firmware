# lomessage

Small, **transport-agnostic** helpers for building, splitting, queuing, and driving long text messages through any byte-oriented transport.

See also **[`locommand`](../locommand/README.md)** — nested CLI dispatcher that uses `lomessage::Buffer` for command replies.

- **`Buffer`** ([`Buffer.h`](./Buffer.h), [`Buffer.cpp`](./Buffer.cpp)) — heap-backed `append` / `appendf` with a hard max size cap.
- **`Split.h`** — stateless chunking policy
  - `next_chunk_len(data, total_len, offset, max_chunk)` — raw chunk length; prefers breaking at the last newline in the second half of a full-sized window.
  - `next_chunk(..., flags)` — returns `{emit_len, consume_len}`. With `CHUNK_ABSORB_LINE_BOUNDARY`, a non-final chunk ending with `\n` is reported with `emit_len = raw_len - 1` and `consume_len = raw_len` so the `\n` is neither emitted nor left as the first byte of the next chunk. Useful when the consumer renders each chunk as its own line and adds its own newline.
- **`Queue`** ([`Queue.h`](./Queue.h), [`Queue.cpp`](./Queue.cpp)) — FIFO of outbound text jobs. Owns heap copies of each message and its per-message opaque `user_ctx` routing blob, along with `max_chunk`, `split_flags`, and inter-chunk delay. One call per loop tick:

  ```cpp
  lomessage::Queue q;
  // per-message:
  q.send(text, &my_route_ctx, sizeof(my_route_ctx), opts, now_ms);
  // per loop tick:
  q.service(now_ms, my_sink);
  ```

- **`Sink`** — the one thing the application must implement: `sendChunk(data, len, chunk_idx, total_chunks, is_final, user_ctx) -> SendResult { Sent | Retry | Abandon }`. This is where transport-specific framing lives (e.g. LoRa/MeshCore `TXT_MSG`, UART, TCP). `lomessage` itself has no radio, network, or `Serial` dependencies.
