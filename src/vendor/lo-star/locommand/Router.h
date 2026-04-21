#pragma once

#include <lomessage/Buffer.h>

namespace locommand {

class Engine;

/** Dispatches among multiple CLI roots (e.g. lotato, wifi, config). Non-owning engines. */
class Router {
public:
  static constexpr int kMaxEngines = 8;

  Router();

  void add(Engine* e);
  void clear();

  /** True if @p cmd matches any registered engine root (leading spaces skipped). */
  bool matchesAnyRoot(const char* cmd) const;

  /** True for bare `help` / `?` / `help …` (same cases `dispatch` handles before root match). */
  bool matchesGlobalHelp(const char* cmd) const;

  /**
   * Dispatch a full command line (including root token). Returns false if no engine matched
   * (caller may fall through to legacy CLI).
   */
  bool dispatch(const char* command, lomessage::Buffer& out, void* app_ctx);

  /** One line per root (brief) plus each engine's flat help. */
  void formatGlobalHelp(lomessage::Buffer& out) const;

private:
  Engine* _engines[kMaxEngines];
  int _n;
};

}  // namespace locommand
