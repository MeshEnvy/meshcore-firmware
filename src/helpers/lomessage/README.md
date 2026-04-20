# lomessage

Small, **transport-agnostic** helpers for building and segmenting text:

- **`Split.h`** — `next_chunk_len(...)`: choose the next chunk length for a bounded transport size, preferring newline breaks in the second half of each full-sized window so lines stay intact when possible.
- **`Buffer`** — heap-backed `append` / `appendf` with a hard max size cap.

No radio stack, network stack, or host `Serial` dependencies in this folder.
