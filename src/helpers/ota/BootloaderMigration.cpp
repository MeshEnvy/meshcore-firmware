#if defined(OTA_BOOTLOADER_MIGRATION)
#if !defined(NRF52_PLATFORM) || !defined(RAK_4631)
#error Bootloader migration is restricted to the RAK4631 qualification application
#endif

#include "BootloaderMigration.h"

#include "BootloaderMigrationPolicy.h"
#include "BootloaderMigrationResource.h"
#include "OtaSelf.h"

#include <Arduino.h>
#include <SHA256.h>
#include <nrf.h>
#include <nrf_mbr.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>
#include <stdio.h>
#include <string.h>

namespace mesh {
namespace ota {
static_assert(migration::ACL_READ_DENIED == ACL_ACL_PERM_READ_Msk, "ACL read permission bit");
static_assert(migration::ACL_WRITE_DENIED == ACL_ACL_PERM_WRITE_Msk, "ACL write permission bit");
namespace {
bool pending = false;
uint32_t armed_at = 0;
uint8_t last_result = 0;
bool result_is_current = false;

// The pinned framework's historical SVC wrapper has no memory clobber or IPA
// barrier. The MBR reads this pointed-to command after exception entry, which
// must remain visible to the compiler even under whole-program optimization.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
__attribute__((naked, noipa)) uint32_t migration_mbr_command(sd_mbr_command_t *) {
  __asm volatile("svc %0\n"
                 "bx lr"
                 :
                 : "I"(SD_MBR_COMMAND)
                 : "r0", "memory");
}
#pragma GCC diagnostic pop

void hex8(const uint8_t *value, char output[17]) {
  static const char digits[] = "0123456789ABCDEF";
  for (unsigned i = 0; i < 8; ++i) {
    output[2 * i] = digits[value[i] >> 4];
    output[2 * i + 1] = digits[value[i] & 15];
  }
  output[16] = 0;
}

void hash_region(uint32_t address, uint32_t size, uint8_t output[32]) {
  SHA256 hash;
  hash.reset();
  uint8_t buffer[128];
  const volatile uint8_t *source = reinterpret_cast<const volatile uint8_t *>(address);
  while (size) {
    const uint32_t take = size < sizeof(buffer) ? size : sizeof(buffer);
    for (uint32_t i = 0; i < take; ++i)
      buffer[i] = source[i];
    hash.update(buffer, take);
    source += take;
    size -= take;
  }
  hash.finalize(output, 32);
}

uint8_t retained_result() {
  if (result_is_current) return last_result;
  uint8_t enabled = 0;
  uint32_t result = 0;
  if (sd_softdevice_is_enabled(&enabled) != NRF_SUCCESS) return 0;
  if (enabled) {
    if (sd_power_gpregret_get(0, &result) != NRF_SUCCESS) return 0;
  } else {
    result = NRF_POWER->GPREGRET;
  }
  return result >= 0xC0 && result <= 0xCF ? result & 15 : last_result;
}

// Called only after the operator reply has drained and SoftDevice is stopped.
// GPREGRET (not GPREGRET2) survives this legacy loader's diagnostic writes.
void fail_and_reset(uint8_t result) {
  NRF_POWER->GPREGRET = 0xC0u | (result & 15u);
  NVIC_SystemReset();
  while (true) {
  }
}

void install() {
  SelfFwInfo app;
  if (!ota_self_firmware(app) || !app.valid) {
    pending = false;
    last_result = 1;
    return;
  }
  uint8_t enabled = 0;
  if (sd_softdevice_is_enabled(&enabled) != NRF_SUCCESS ||
      (enabled && sd_softdevice_disable() != NRF_SUCCESS)) {
    pending = false;
    last_result = 2;
    return;
  }

  // ACL is SoftDevice-owned while enabled. Inspect it only after disabling
  // SoftDevice; no ACL/MPU writes or attempts to defeat locked regions.
  migration::Snapshot snapshot = {};
  snapshot.part = NRF_FICR->INFO.PART;
  snapshot.flash_page_size = NRF_FICR->CODEPAGESIZE;
  snapshot.flash_pages = NRF_FICR->CODESIZE;
  for (unsigned i = 0; i < 8; ++i) {
    snapshot.acl[i] = { NRF_ACL->ACL[i].ADDR, NRF_ACL->ACL[i].SIZE, NRF_ACL->ACL[i].PERM };
    const auto &region = snapshot.acl[i];
    if ((region.permissions & migration::ACL_READ_DENIED) &&
        (migration::overlaps(region.address, region.size, 0, migration::APP_CEILING) ||
         migration::overlaps(region.address, region.size, migration::BOOT_BASE, migration::SETTINGS_BASE)))
      fail_and_reset(9);
  }
  snapshot.softdevice_size = *reinterpret_cast<const volatile uint32_t *>(0x3008);
  snapshot.softdevice_fwid = *reinterpret_cast<const volatile uint16_t *>(0x300C);
  snapshot.mbr_boot_override = *reinterpret_cast<const volatile uint32_t *>(0xFF8);
  snapshot.mbr_params_override = *reinterpret_cast<const volatile uint32_t *>(0xFFC);
  snapshot.uicr_boot = NRF_UICR->NRFFW[0];
  snapshot.uicr_params = NRF_UICR->NRFFW[1];
  snapshot.source = reinterpret_cast<uint32_t>(migration_candidate);
  snapshot.source_size = sizeof(migration_candidate);
  snapshot.app_body_size = app.body_len;
  const auto gate = migration::preflight(snapshot);
  if (gate != migration::Gate::Ok) fail_and_reset(3 + static_cast<uint8_t>(gate));

  uint8_t digest[32];
  hash_region(snapshot.source, snapshot.source_size, digest);
  if (memcmp(digest, migration_candidate_sha256, sizeof(digest)) != 0) fail_and_reset(10);
  hash_region(migration::BOOT_BASE, migration::BOOT_SIZE, digest);
  if (memcmp(digest, migration_candidate_sha256, sizeof(digest)) == 0) fail_and_reset(11);
  if (memcmp(digest, migration_legacy_sha256, sizeof(digest)) != 0) fail_and_reset(12);

  // Do not start an uninterruptible MBR copy under an unexpectedly short WDT.
  if (NRF_WDT->RUNSTATUS && NRF_WDT->CRV < 5u * 32768u) fail_and_reset(13);
  if (NRF_WDT->RUNSTATUS) {
    for (unsigned i = 0; i < 8; ++i) {
      if (NRF_WDT->RREN & (1u << i)) NRF_WDT->RR[i] = 0x6E524635u;
    }
  }
  NRF_POWER->GPREGRET = 0;
  NRF_POWER->GPREGRET2 = 0;
  sd_mbr_command_t command = {};
  command.command = SD_MBR_COMMAND_COPY_BL;
  command.params.copy_bl.bl_src = reinterpret_cast<uint32_t *>(snapshot.source);
  command.params.copy_bl.bl_len = migration::BOOT_SIZE / sizeof(uint32_t);
  // MBR owns the erase/copy and persistent recovery parameters. It never
  // returns on success. No application, filesystem or settings erase occurs.
  (void)migration_mbr_command(&command);
  fail_and_reset(14);
}
} // namespace

bool bootloader_migration_pending() {
  return pending;
}

void bootloader_migration_status(char *reply, size_t capacity) {
  char hash[17];
  hex8(migration_candidate_sha256, hash);
  snprintf(reply, capacity, "BL migration: internal embedded 40960 B; hash=%s; %s; last=%u", hash,
           pending ? "armed" : "idle", retained_result());
}

bool bootloader_migration_request(const char *confirmation, char *reply, size_t capacity) {
  char hash[17];
  hex8(migration_candidate_sha256, hash);
  if (pending) {
    snprintf(reply, capacity, "ERR bootloader migration is already armed");
    return false;
  }
  if (strcmp(confirmation, hash) != 0) {
    snprintf(reply, capacity, "ERR confirm exact image: ota bootloader install %s", hash);
    return false;
  }
  pending = true;
  armed_at = millis();
  last_result = 0;
  result_is_current = true;
  snprintf(reply, capacity, "OK migration armed; checking exact old loader and protection after TX drains");
  return true;
}

void bootloader_migration_audit(char *reply, size_t capacity) {
  SelfFwInfo app;
  if (!ota_self_firmware(app) || !app.valid) {
    snprintf(reply, capacity, "ERR no valid application EndF");
    return;
  }
  uint8_t digest[32];
  char app_hash[17], settings_hash[17], fs_hash[17], sd_hash[17];
  hash_region(migration::APP_BASE, app.image_len, digest);
  hex8(digest, app_hash);
  hash_region(migration::SETTINGS_BASE, 4096, digest);
  hex8(digest, settings_hash);
  hash_region(migration::APP_CEILING, migration::BOOT_BASE - migration::APP_CEILING, digest);
  hex8(digest, fs_hash);
  hash_region(0, migration::APP_BASE, digest);
  hex8(digest, sd_hash);
  snprintf(reply, capacity, "app=%s settings=%s fs=%s sd=%s", app_hash, settings_hash, fs_hash, sd_hash);
}

void bootloader_migration_tick(bool outbound_idle, bool ota_idle) {
  if (!pending) return;
  const uint32_t elapsed = millis() - armed_at;
  // An incoming advert may have started autofetch since the admin command.
  // Never disable SoftDevice while another OTA session owns the flash store.
  if (!ota_idle) {
    pending = false;
    last_result = 16;
  } else if (elapsed >= 30000u) {
    pending = false;
    last_result = 15;
  } else if (elapsed >= 1500u && outbound_idle) {
    install();
  }
}
} // namespace ota
} // namespace mesh
#endif
