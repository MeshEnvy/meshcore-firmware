#pragma once

#include "Command.h"

namespace locommand {

/** Nested CLI: strict group-or-leaf tree, flat help, root name match. */
class Engine {
public:
  /** @param root_name literal for the root token (e.g. "lotato"); not copied, must outlive Engine. */
  explicit Engine(const char* root_name);
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  /** Register a dotted path under the root (e.g. "wifi.scan"). Creates group segments as needed.
   *  Enforces group-or-leaf invariant. Returns the new leaf or nullptr on conflict / OOM. */
  Command* add(const char* path, Handler handler, const char* usage_suffix = nullptr,
               const char* hint = nullptr, const char* brief = nullptr);

  /** True if @p cmd starts with @ref rootName followed by '\0', whitespace, or '?'. */
  bool matchesRoot(const char* cmd) const;

  /** Dispatch text after the root token (e.g. " wifi status"). Bare / help / ? => full help. */
  void dispatch(const char* input_after_root, lomessage::Buffer& out, void* app_ctx);

  /** One help line per leaf: "<root> <path> [usage]  (hint) - <brief>". @p sub_path is space-separated segments
   *  under root (e.g. "wifi") or nullptr for all commands. */
  void formatHelp(lomessage::Buffer& out, const char* sub_path = nullptr) const;

  const char* rootName() const { return _root.name; }

private:
  Command _root;
};

}  // namespace locommand
