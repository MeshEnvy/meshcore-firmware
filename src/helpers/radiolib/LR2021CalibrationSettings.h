#pragma once

#include <helpers/IdentityStore.h>

class CustomLR2021Wrapper;

namespace mesh { namespace lr2021 {

// Load before radio initialization. No record leaves native trim unchanged.
bool loadLR2021CalibrationSettings(FILESYSTEM* fs, CustomLR2021Wrapper& radio);
void loopLR2021CalibrationSettings(CustomLR2021Wrapper& radio);
bool handleLR2021CalibrationCommand(const char* command, char* reply,
                                   size_t reply_size, CustomLR2021Wrapper& radio);

}}  // namespace mesh::lr2021
