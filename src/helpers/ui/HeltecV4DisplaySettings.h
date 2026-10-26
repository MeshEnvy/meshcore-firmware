#pragma once

#include <helpers/IdentityStore.h>
#include "HeltecV4Display.h"

namespace mesh { namespace ui {

// Load before the first display probe, including a saved Off selection.
bool loadHeltecV4DisplaySettings(FILESYSTEM* fs, HeltecV4Display& display);
bool handleHeltecV4DisplayCommand(const char* command, char* reply,
                                 size_t reply_size, HeltecV4Display& display);

}}  // namespace mesh::ui
