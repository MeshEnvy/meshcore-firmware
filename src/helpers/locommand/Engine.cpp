#include "Engine.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace locommand {

namespace {

void append_child(Command* parent, Command* child) {
  child->next_sibling = nullptr;
  if (!parent->first_child) {
    parent->first_child = child;
    return;
  }
  Command* t = parent->first_child;
  while (t->next_sibling) t = t->next_sibling;
  t->next_sibling = child;
}

Command* find_child(Command* parent, const char* name) {
  for (Command* c = parent->first_child; c; c = c->next_sibling) {
    if (strcmp(c->name, name) == 0) return c;
  }
  return nullptr;
}

/** Trim leading spaces in-place; return pointer into @p s. */
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

/** First token in @p s (mutable); null-terminates token; returns start of token. @p rest set to byte after token. */
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

void format_help_recursive(const char* root_name, const Command* parent, const char* prefix,
                           lomessage::Buffer& out) {
  for (Command* c = parent->first_child; c; c = c->next_sibling) {
    if (c->handler && !c->first_child) {
      out.appendf("%s ", root_name);
      if (prefix && prefix[0] != '\0') out.append(prefix);
      out.append(c->name);
      if (c->usage_suffix && c->usage_suffix[0] != '\0') out.appendf(" %s", c->usage_suffix);
      if (c->hint && c->hint[0] != '\0') out.appendf("  (%s)", c->hint);
      out.append("\n");
    } else if (!c->handler && c->first_child) {
      char next[120];
      if (prefix && prefix[0] != '\0') {
        snprintf(next, sizeof(next), "%s%s ", prefix, c->name);
      } else {
        snprintf(next, sizeof(next), "%s ", c->name);
      }
      format_help_recursive(root_name, c, next, out);
    }
  }
}

void format_one_leaf_line(const char* root_name, const char* prefix, const Command* leaf,
                          lomessage::Buffer& out) {
  out.appendf("%s ", root_name);
  if (prefix && prefix[0] != '\0') out.append(prefix);
  out.append(leaf->name);
  if (leaf->usage_suffix && leaf->usage_suffix[0] != '\0') out.appendf(" %s", leaf->usage_suffix);
  if (leaf->hint && leaf->hint[0] != '\0') out.appendf("  (%s)", leaf->hint);
  out.append("\n");
}

}  // namespace

void Context::printHelp() const {
  if (!command || !engine) return;
  format_one_leaf_line(engine->rootName(), path_prefix, command, out);
}

namespace {

const Command* find_node(const Command* cur, const char** tokens, int ntok, int idx) {
  if (idx == ntok) return cur;
  for (Command* c = cur->first_child; c; c = c->next_sibling) {
    if (strcmp(c->name, tokens[idx]) == 0) return find_node(c, tokens, ntok, idx + 1);
  }
  return nullptr;
}

/** Parse @p sub into at most @p max_tok tokens; pointers into @p work (strtok_r mutates). */
int tokenize_sub_path(char* work, const char** tokens, int max_tok) {
  int n = 0;
  char* save = nullptr;
  for (char* t = strtok_r(work, " \t\r\n.", &save); t && n < max_tok; t = strtok_r(nullptr, " \t\r\n.", &save)) {
    tokens[n++] = t;
  }
  return n;
}

void build_argv_from_rest(char* rest, const char** argv, int* argc) {
  *argc = 0;
  char* p = rest;
  while (*p && *argc < kMaxArgc) {
    char* tail = nullptr;
    char* tok = pop_token(p, &tail);
    if (*tok == '\0') break;
    argv[(*argc)++] = tok;
    p = tail;
  }
}

}  // namespace

Engine::Engine(const char* root_name) {
  memset(&_root, 0, sizeof(_root));
  _root.name = root_name;
}

Engine::~Engine() {
  Command* c = _root.first_child;
  while (c) {
    Command* nx = c->next_sibling;
    command_free_tree(c);
    c = nx;
  }
  _root.first_child = nullptr;
}

Command* Engine::add(const char* path, Handler handler, const char* usage_suffix, const char* hint,
                     const char* brief) {
  if (!path || !path[0] || !handler) return nullptr;

  char work[128];
  strncpy(work, path, sizeof(work) - 1);
  work[sizeof(work) - 1] = '\0';

  const char* segs[16];
  int nseg = 0;
  char* save = nullptr;
  for (char* t = strtok_r(work, ".", &save); t && nseg < 16; t = strtok_r(nullptr, ".", &save)) {
    segs[nseg++] = t;
  }
  if (nseg == 0) return nullptr;

  Command* cur = &_root;
  for (int i = 0; i < nseg - 1; ++i) {
    Command* ch = find_child(cur, segs[i]);
    if (!ch) {
      ch = command_new(segs[i]);
      if (!ch) return nullptr;
      ch->handler = nullptr;
      ch->first_child = nullptr;
      append_child(cur, ch);
    } else {
      if (ch->handler != nullptr || ch->first_child == nullptr) {
        return nullptr;
      }
    }
    cur = ch;
  }

  const char* leaf_name = segs[nseg - 1];
  Command* existing = find_child(cur, leaf_name);
  if (existing) return nullptr;

  Command* leaf = command_new(leaf_name);
  if (!leaf) return nullptr;
  leaf->handler = handler;
  leaf->usage_suffix = usage_suffix;
  leaf->hint = hint;
  leaf->brief = brief;
  leaf->first_child = nullptr;
  append_child(cur, leaf);
  return leaf;
}

bool Engine::matchesRoot(const char* cmd) const {
  if (!cmd || !_root.name) return false;
  size_t rn = strlen(_root.name);
  if (strncmp(cmd, _root.name, rn) != 0) return false;
  unsigned char c = static_cast<unsigned char>(cmd[rn]);
  return cmd[rn] == '\0' || c == '?' || isspace(c) != 0;
}

void Engine::formatHelp(lomessage::Buffer& out, const char* sub_path) const {
  if (!sub_path || sub_path[0] == '\0') {
    format_help_recursive(_root.name, &_root, "", out);
    return;
  }

  char w[128];
  strncpy(w, sub_path, sizeof(w) - 1);
  w[sizeof(w) - 1] = '\0';
  trim_right(trim_left(w));

  const char* tokens[16];
  int n = tokenize_sub_path(w, tokens, 16);
  if (n == 0) {
    format_help_recursive(_root.name, &_root, "", out);
    return;
  }

  const Command* node = find_node(&_root, tokens, n, 0);
  if (!node) {
    out.appendf("Unknown help topic\n");
    format_help_recursive(_root.name, &_root, "", out);
    return;
  }

  if (node->handler && !node->first_child) {
    char prefix[96] = {};
    if (n > 1) {
      size_t pos = 0;
      for (int i = 0; i < n - 1 && pos + 1 < sizeof(prefix); ++i) {
        const char* seg = tokens[i];
        size_t sl = strlen(seg);
        if (pos + sl + 2 > sizeof(prefix)) break;
        memcpy(prefix + pos, seg, sl);
        pos += sl;
        prefix[pos++] = ' ';
        prefix[pos] = '\0';
      }
    }
    format_one_leaf_line(_root.name, prefix[0] ? prefix : nullptr, node, out);
    return;
  }

  char prefix[96] = {};
  size_t pos = 0;
  for (int i = 0; i < n && pos + 1 < sizeof(prefix); ++i) {
    const char* seg = tokens[i];
    size_t sl = strlen(seg);
    if (pos + sl + 2 > sizeof(prefix)) break;
    memcpy(prefix + pos, seg, sl);
    pos += sl;
    prefix[pos++] = ' ';
    prefix[pos] = '\0';
  }
  format_help_recursive(_root.name, node, prefix, out);
}

void Engine::dispatch(const char* input_after_root, lomessage::Buffer& out, void* app_ctx) {
  char scratch[512];
  if (!input_after_root) input_after_root = "";
  strncpy(scratch, input_after_root, sizeof(scratch) - 1);
  scratch[sizeof(scratch) - 1] = '\0';
  trim_right(trim_left(scratch));

  char* p = scratch;

  if (*p == '\0' || strcmp(p, "help") == 0 || strcmp(p, "?") == 0) {
    formatHelp(out, nullptr);
    return;
  }

  if (strncmp(p, "help ", 5) == 0) {
    char* sub = trim_left(p + 5);
    trim_right(sub);
    formatHelp(out, sub[0] != '\0' ? sub : nullptr);
    return;
  }

  Command* cur = &_root;
  char prefix_buf[128] = {};
  size_t prefix_len = 0;

  while (true) {
    p = trim_left(p);
    if (*p == '\0') {
      if (cur == &_root) {
        formatHelp(out, nullptr);
      } else {
        format_help_recursive(_root.name, cur, prefix_buf, out);
      }
      return;
    }

    char* tail = nullptr;
    char* tok = pop_token(p, &tail);
    p = tail;

    Command* ch = find_child(cur, tok);
    if (!ch) {
      format_help_recursive(_root.name, cur, prefix_buf, out);
      return;
    }

    size_t leaf_prefix_len = prefix_len;

    if (prefix_len + strlen(ch->name) + 2 > sizeof(prefix_buf)) {
      out.append("Err - command path too long\n");
      return;
    }
    memcpy(prefix_buf + prefix_len, ch->name, strlen(ch->name));
    prefix_len += strlen(ch->name);
    prefix_buf[prefix_len++] = ' ';
    prefix_buf[prefix_len] = '\0';

    if (ch->handler && !ch->first_child) {
      char* rest = trim_left(p);

      static const char* argv_storage[kMaxArgc];
      int ac = 0;
      char arg_copy[384];
      arg_copy[0] = '\0';
      if (rest && rest[0] != '\0') {
        strncpy(arg_copy, rest, sizeof(arg_copy) - 1);
        arg_copy[sizeof(arg_copy) - 1] = '\0';
        build_argv_from_rest(arg_copy, argv_storage, &ac);
      }

      char leaf_prefix[128];
      memcpy(leaf_prefix, prefix_buf, leaf_prefix_len);
      leaf_prefix[leaf_prefix_len] = '\0';

      Context ctx{this, out, rest, ac, ac > 0 ? argv_storage : nullptr, app_ctx, ch, leaf_prefix};
      ch->handler(ctx);
      return;
    }

    if (!ch->handler && ch->first_child) {
      cur = ch;
      continue;
    }

    out.append("Err - invalid command tree\n");
    return;
  }
}

}  // namespace locommand
