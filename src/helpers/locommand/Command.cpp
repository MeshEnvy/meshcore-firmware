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

}  // namespace locommand
