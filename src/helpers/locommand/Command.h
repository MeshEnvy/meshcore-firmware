#pragma once

#include <helpers/lomessage/Buffer.h>

namespace locommand {

class Engine;
struct Command;

/** Per-invocation context passed to a leaf handler. */
struct Context {
  Engine* engine;              ///< Owning engine (root name, help, etc.).
  lomessage::Buffer& out;      ///< Handler appends the full reply here.
  const char* args_raw;       ///< Untokenized remainder after the matched path (trimmed of leading spaces).
  int argc;                   ///< Token count for remainder (capped at kMaxArgc).
  const char* const* argv;  ///< Pointers into dispatch scratch (null-terminated tokens).
  void* app_ctx;              ///< Opaque pointer from dispatch() (e.g. mesh + sender_ts).
  const Command* command;     ///< Current leaf being dispatched (for printHelp).
  const char* path_prefix;    ///< Space-separated path without root and without leaf name (e.g. "wifi " or "").

  /** Emit one-line usage for the current leaf into @ref out:
   *  "<root> [prefix]<name> [usage_suffix]  (hint) - <brief>\n". No-op if @ref command is null. */
  void printHelp() const;
};

typedef void (*Handler)(Context& ctx);

/** One node in the command tree: either a group (children only) or a leaf (handler only). */
struct Command {
  const char* name;           ///< Segment name; string literal, not copied.
  const char* usage_suffix;   ///< Optional; printed after path in flat help (leaves only).
  const char* hint;           ///< Optional parenthetical in flat help.
  const char* brief;        ///< Optional short description; appended in help as " - <brief>".
  Handler handler;            ///< Non-null on leaves; null on groups.
  Command* first_child;       ///< Non-null on groups; null on leaves.
  Command* next_sibling;      ///< Sibling chain under the same parent.
};

/** Max argv entries passed to a leaf handler. */
constexpr int kMaxArgc = 8;

/** Allocate a zeroed command node (used by Engine). */
Command* command_new(const char* name);

/** Free a subtree rooted at @p n (used by Engine). */
void command_free_tree(Command* n);

}  // namespace locommand
