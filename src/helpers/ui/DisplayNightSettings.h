#pragma once

#include <helpers/IdentityStore.h>
#include "DisplayNightMode.h"

namespace mesh { namespace ui {
#ifdef MESHCORE_HAS_COLOR_DISPLAY
void loadDisplayNightSettings(FILESYSTEM* fs);
#else
inline void loadDisplayNightSettings(FILESYSTEM*) {}
#endif
} }
