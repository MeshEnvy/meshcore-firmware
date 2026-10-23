#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mesh {
namespace ota {
namespace migration {

static const uint32_t APP_BASE = 0x26000u;
static const uint32_t APP_CEILING = 0xED000u;
static const uint32_t BOOT_BASE = 0xF4000u;
static const uint32_t BOOT_SIZE = 0xA000u;
static const uint32_t PARAMS_BASE = 0xFE000u;
static const uint32_t SETTINGS_BASE = 0xFF000u;
static const uint32_t ACL_READ_DENIED = 1u << 2;
static const uint32_t ACL_WRITE_DENIED = 1u << 1;

struct AclRegion {
  uint32_t address;
  uint32_t size;
  uint32_t permissions;
};

struct Snapshot {
  uint32_t part;
  uint32_t flash_page_size;
  uint32_t flash_pages;
  uint32_t softdevice_size;
  uint16_t softdevice_fwid;
  uint32_t mbr_boot_override;
  uint32_t mbr_params_override;
  uint32_t uicr_boot;
  uint32_t uicr_params;
  uint32_t source;
  uint32_t source_size;
  uint32_t app_body_size;
  AclRegion acl[8];
};

enum class Gate { Ok, Chip, Softdevice, BootAddress, ParamsAddress, Source, Protection };

inline bool overlaps(uint32_t start, uint32_t size, uint32_t lo, uint32_t hi) {
  return size && uint64_t(start) < hi && uint64_t(start) + size > lo;
}

inline Gate preflight(const Snapshot &s) {
  if (s.part != 0x52840u || s.flash_page_size != 4096u || s.flash_pages != 256u) return Gate::Chip;
  if (s.softdevice_size != APP_BASE || s.softdevice_fwid != 0xB6u) return Gate::Softdevice;
  const uint32_t boot = s.mbr_boot_override == UINT32_MAX ? s.uicr_boot : s.mbr_boot_override;
  const uint32_t params = s.mbr_params_override == UINT32_MAX ? s.uicr_params : s.mbr_params_override;
  if (boot != BOOT_BASE) return Gate::BootAddress;
  if (params != PARAMS_BASE) return Gate::ParamsAddress;
  // The source is a linked application resource, never SRAM, external memory,
  // a filesystem, the live bootloader, or a dynamically chosen scratch bank.
  if (s.source_size != BOOT_SIZE || (s.source & 3u) || s.source < APP_BASE || !s.app_body_size ||
      s.app_body_size > APP_CEILING - APP_BASE ||
      uint64_t(s.source) + BOOT_SIZE > uint64_t(APP_BASE) + s.app_body_size)
    return Gate::Source;
  for (const auto &r : s.acl) {
    // nRF52840 ACL bit 2 denies reads; bit 1 denies writes. COPY_BL also needs its
    // parameter page writable. Never try to unlock an existing ACL region.
    if ((r.permissions & ACL_READ_DENIED) &&
        (overlaps(r.address, r.size, APP_BASE, APP_BASE + s.app_body_size) ||
         overlaps(r.address, r.size, BOOT_BASE, SETTINGS_BASE)))
      return Gate::Protection;
    if ((r.permissions & ACL_WRITE_DENIED) && overlaps(r.address, r.size, BOOT_BASE, SETTINGS_BASE))
      return Gate::Protection;
  }
  return Gate::Ok;
}

} // namespace migration
} // namespace ota
} // namespace mesh
