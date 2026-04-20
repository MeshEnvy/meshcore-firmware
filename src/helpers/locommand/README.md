# locommand

Small **transport-agnostic** nested command dispatcher for firmware CLIs. Depends only on [`lomessage::Buffer`](../lomessage/Buffer.h) for reply assembly.

## Rules

- **Group-or-leaf tree:** every registered node is either a **group** (`handler == nullptr`, has children) or a **leaf** (`handler != nullptr`, no children). `Engine::add()` rejects ambiguous paths.
- **Root name:** the engine owns a root token (e.g. `lotato`). `matchesRoot(line)` mirrors the usual `memcmp` + continuation check. The substring **after** the root token is passed to `dispatch()`.
- **Help:** bare root / empty input / `help` / `?` prints full flat help. `help <topic>` prints help for that subtree. At a group with no further tokens (e.g. `lotato wifi`), the engine prints that group’s subtree help.
- **Strings:** `path`, `usage_suffix`, `hint`, `brief`, and segment names passed to `add()` must be **string literals** (or otherwise live for the lifetime of the `Engine`); they are not copied.

## Example

```cpp
#include <helpers/locommand/Engine.h>
#include <helpers/lomessage/Buffer.h>

static void h_status(locommand::Context& ctx) {
  ctx.out.append("OK\n");
}

void setup_cli(locommand::Engine& eng) {
  eng.add("status", h_status);
  eng.add("wifi.scan", h_wifi_scan, nullptr, "(async)");
}

void on_line(char* line, char* reply_buf, size_t cap) {
  locommand::Engine eng("lotato");
  setup_cli(eng);
  if (!eng.matchesRoot(line)) return;
  lomessage::Buffer out(512);
  const char* after = line + strlen("lotato");
  eng.dispatch(after, out, nullptr);
  // copy out.c_str() into reply_buf / mesh queue as needed
}
```

## Context

Leaf handlers receive `locommand::Context`:

- `out` — append the full reply (`lomessage::Buffer`).
- `args_raw` — remainder of the line after the matched path (leading spaces trimmed). Use for values that may contain spaces (e.g. URLs).
- `argc` / `argv` — whitespace-split tokens of the remainder (capped at `kMaxArgc`).
- `printHelp()` — emit one-line usage for the current leaf (`<root> <path> [usage]  (hint)`). Use when arg parsing fails.
