#include "Command.h"

#include <cstdlib>
#include <cstring>

namespace locommand {

Command* command_new(const char* name) {
  auto* n = static_cast<Command*>(calloc(1, sizeof(Command)));
  if (!n) return nullptr;
  if (name) {
    size_t len = strlen(name);
    char* copy = static_cast<char*>(malloc(len + 1));
    if (!copy) {
      free(n);
      return nullptr;
    }
    memcpy(copy, name, len + 1);
    n->name = copy;
  }
  return n;
}

void command_free_tree(Command* n) {
  if (!n) return;
  Command* c = n->first_child;
  while (c) {
    Command* nx = c->next_sibling;
    command_free_tree(c);
    c = nx;
  }
  free(const_cast<char*>(n->name));
  free(n);
}

void command_append_arg_help(const Command* leaf, const char* root_name, const char* path_prefix,
                             lomessage::Buffer& out) {
  if (!leaf || !leaf->arg_specs || leaf->n_arg_specs <= 0) return;
  out.append("\nArguments:\n");
  for (int i = 0; i < leaf->n_arg_specs; i++) {
    const ArgSpec& a = leaf->arg_specs[i];
    out.appendf("  %s", a.name ? a.name : "?");
    if (a.type_hint && a.type_hint[0]) out.appendf("  (%s)", a.type_hint);
    if (a.choices && a.choices[0]) out.appendf("  choices: %s", a.choices);
    if (a.description && a.description[0]) out.appendf("  - %s", a.description);
    out.append("\n");
  }
  if (leaf->details && leaf->details[0]) {
    out.append("\n");
    out.append(leaf->details);
    out.append("\n");
  }
}

}  // namespace locommand
