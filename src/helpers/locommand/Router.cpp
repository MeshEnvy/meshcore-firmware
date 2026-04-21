#include "Router.h"

#include "Engine.h"

#include <cctype>
#include <cstring>

namespace locommand {

namespace {

char* trim_left(char* s) {
  if (!s) return s;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  return s;
}

void trim_right(char* s) {
  if (!s) return;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
    s[--n] = '\0';
  }
}

char* pop_token(char* s, char** rest) {
  char* p = trim_left(s);
  *rest = p;
  if (*p == '\0') return p;
  char* start = p;
  while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
  if (*p != '\0') {
    *p++ = '\0';
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  }
  *rest = p;
  return start;
}

}  // namespace

Router::Router() : _engines{}, _n(0) {}

void Router::add(Engine* e) {
  if (!e || _n >= kMaxEngines) return;
  _engines[_n++] = e;
}

void Router::clear() {
  for (int i = 0; i < kMaxEngines; i++) _engines[i] = nullptr;
  _n = 0;
}

bool Router::matchesAnyRoot(const char* cmd) const {
  if (!cmd) return false;
  char scratch[256];
  strncpy(scratch, cmd, sizeof(scratch) - 1);
  scratch[sizeof(scratch) - 1] = '\0';
  char* p = trim_left(scratch);
  for (int i = 0; i < _n; i++) {
    if (_engines[i] && _engines[i]->matchesRoot(p)) return true;
  }
  return false;
}

bool Router::matchesGlobalHelp(const char* cmd) const {
  if (!cmd) return false;
  char scratch[512];
  strncpy(scratch, cmd, sizeof(scratch) - 1);
  scratch[sizeof(scratch) - 1] = '\0';
  trim_right(scratch);
  char* p = trim_left(scratch);
  if (*p == '\0') return false;
  if (strcmp(p, "help") == 0 || strcmp(p, "?") == 0) return true;
  if (strncmp(p, "help ", 5) == 0) return true;
  return false;
}

void Router::formatGlobalHelp(lomessage::Buffer& out) const {
  out.append("CLI roots:\n");
  for (int i = 0; i < _n; i++) {
    Engine* e = _engines[i];
    if (!e) continue;
    const char* br = e->rootBrief();
    out.appendf("  %s", e->rootName());
    if (br && br[0]) out.appendf(" - %s", br);
    out.append("\n");
  }
  out.append("\nUse: help <root>  or  <root> help  for commands under that root.\n");
}

bool Router::dispatch(const char* command, lomessage::Buffer& out, void* app_ctx) {
  if (!command) return false;
  char scratch[512];
  strncpy(scratch, command, sizeof(scratch) - 1);
  scratch[sizeof(scratch) - 1] = '\0';
  trim_right(scratch);
  char* p = trim_left(scratch);

  if (*p == '\0' || strcmp(p, "help") == 0 || strcmp(p, "?") == 0) {
    formatGlobalHelp(out);
    return true;
  }

  if (strncmp(p, "help ", 5) == 0) {
    char* sub = trim_left(p + 5);
    trim_right(sub);
    if (*sub == '\0') {
      formatGlobalHelp(out);
      return true;
    }
    char* rest = nullptr;
    char* root_tok = pop_token(sub, &rest);
    for (int i = 0; i < _n; i++) {
      Engine* e = _engines[i];
      if (!e) continue;
      if (strcmp(root_tok, e->rootName()) == 0) {
        rest = trim_left(rest);
        e->formatHelp(out, *rest ? rest : nullptr);
        return true;
      }
    }
    out.appendf("Unknown CLI root: %s\n", root_tok);
    formatGlobalHelp(out);
    return true;
  }

  for (int i = 0; i < _n; i++) {
    Engine* e = _engines[i];
    if (!e || !e->matchesRoot(p)) continue;
    size_t rn = strlen(e->rootName());
    char* after = p + rn;
    while (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n') after++;
    // `after` points into `scratch`; Engine::dispatch copies into its own buffer before mutating.
    e->dispatch(after, out, app_ctx);
    return true;
  }
  return false;
}

}  // namespace locommand
