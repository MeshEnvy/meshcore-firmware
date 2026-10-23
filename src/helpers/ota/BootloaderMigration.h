#pragma once

#if defined(OTA_BOOTLOADER_MIGRATION)
#include <stddef.h>

namespace mesh {
namespace ota {
bool bootloader_migration_pending();
bool bootloader_migration_request(const char *confirmation, char *reply, size_t capacity);
void bootloader_migration_status(char *reply, size_t capacity);
void bootloader_migration_audit(char *reply, size_t capacity);
void bootloader_migration_tick(bool outbound_idle, bool ota_idle);
} // namespace ota
} // namespace mesh
#endif
