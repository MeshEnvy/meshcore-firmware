#pragma once

#include <lomessage/Buffer.h>

namespace locommand {

class Engine;
struct Command;

/** Optional per-argument metadata for richer help (`help <path>`). */
struct ArgSpec {
  const char* name;
  const char* type_hint;   ///< e.g. "url", "uint", "bool", "string", "secret", nullptr
  const char* choices;     ///< e.g. "on|off" for enums; nullptr if N/A
  bool required;           ///< true => <name>, false => [name]
  const char* description;
};

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
  const char* name;           ///< Segment name; heap copy from command_new.
  const char* usage_suffix;   ///< Optional; printed after path in flat help (leaves only).
  const char* hint;           ///< Optional parenthetical in flat help.
  const char* brief;        ///< Optional short description; appended in help as " - <brief>".
  Handler handler;            ///< Non-null on leaves; null on groups.
  Command* first_child;       ///< Non-null on groups; null on leaves.
  Command* next_sibling;      ///< Sibling chain under the same parent.

  const ArgSpec* arg_specs;   ///< nullptr if legacy registration only.
  int n_arg_specs;
  const char* details;        ///< Optional extra paragraph after Arguments block.
  char usage_storage[96];     ///< When arg_specs set, generated usage lives here; usage_suffix may point here.
};

/** Max argv entries passed to a leaf handler. */
constexpr int kMaxArgc = 8;

/** Allocate a zeroed command node (used by Engine). */
Command* command_new(const char* name);

/** Free a subtree rooted at @p n (used by Engine). */
void command_free_tree(Command* n);

/** Append Arguments block for a leaf with arg_specs (internal). */
void command_append_arg_help(const Command* leaf, const char* root_name, const char* path_prefix,
                             lomessage::Buffer& out);

}  // namespace locommand
