#include "Arduino.h"
#include "BootloaderMigrationResource.h"
#include "SHA256.h"
#include "helpers/ota/OtaSelf.h"
#include "nrf.h"
#include "nrf_mbr.h"
#include "nrf_sdm.h"
#include "nrf_soc.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace migration_test {
uint32_t now = 0;
unsigned endf_checks = 0;
unsigned softdevice_checks = 0;
unsigned disables = 0;
unsigned resets = 0;
unsigned copies = 0;

void mbr_command() {
  ++copies;
  throw std::runtime_error("unexpected MBR command in state-machine test");
}
} // namespace migration_test

// Include the actual driver. Only its target-specific naked SVC adapter is
// replaced: function-like 'volatile(...)' captures the extended assembly, but
// leaves ordinary 'volatile uint8_t *' memory reads unchanged. The adapter is
// an explicit failing trap, never a successful simulated flash operation.
#define __attribute__(...)
#define __asm
#define volatile(...) migration_test::mbr_command()
#include "helpers/ota/BootloaderMigration.cpp"
#undef volatile
#undef __asm
#undef __attribute__

const uint8_t migration_candidate[40960] = {};
const uint8_t migration_candidate_sha256[32] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
const uint8_t migration_legacy_sha256[32] = {};

MigrationTestPower power_registers = {};
MigrationTestPower *NRF_POWER = &power_registers;
MigrationTestFicr *NRF_FICR = nullptr;
MigrationTestAcl *NRF_ACL = nullptr;
MigrationTestUicr *NRF_UICR = nullptr;
MigrationTestWdt *NRF_WDT = nullptr;

uint32_t millis() {
  return migration_test::now;
}

void NVIC_SystemReset() {
  ++migration_test::resets;
  throw std::runtime_error("unexpected reset in state-machine test");
}

uint32_t sd_softdevice_is_enabled(uint8_t *enabled) {
  ++migration_test::softdevice_checks;
  *enabled = 0;
  return NRF_SUCCESS;
}

uint32_t sd_softdevice_disable() {
  ++migration_test::disables;
  throw std::runtime_error("unexpected SoftDevice disable in state-machine test");
}

uint32_t sd_power_gpregret_get(uint32_t, uint32_t *result) {
  *result = 0;
  return NRF_SUCCESS;
}

void SHA256::reset() {
  throw std::runtime_error("unexpected hashing in state-machine test");
}
void SHA256::update(const void *, size_t) {
  throw std::runtime_error("unexpected hashing");
}
void SHA256::finalize(void *, size_t) {
  throw std::runtime_error("unexpected hashing");
}

namespace mesh {
namespace ota {
bool ota_self_firmware(SelfFwInfo &) {
  ++migration_test::endf_checks;
  return false; // Stop at the first install preflight, before hardware access.
}
} // namespace ota
} // namespace mesh

namespace {
using namespace mesh::ota;
const char confirmation[] = "0123456789ABCDEF";
char reply[160];

void arm() {
  assert(bootloader_migration_request(confirmation, reply, sizeof(reply)));
  assert(bootloader_migration_pending());
  assert(std::strstr(reply, "OK migration armed"));
}

void no_install() {
  assert(migration_test::endf_checks == 0);
  assert(migration_test::softdevice_checks == 0);
  assert(migration_test::disables == 0);
  assert(migration_test::resets == 0);
  assert(migration_test::copies == 0);
}

void status_result(unsigned result) {
  bootloader_migration_status(reply, sizeof(reply));
  char expected[24];
  std::snprintf(expected, sizeof(expected), "last=%u", result);
  assert(std::strstr(reply, expected));
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  using namespace migration_test;
  const char *scenario = argv[1];
  if (std::strcmp(scenario, "wrong-confirmation") == 0) {
    assert(!bootloader_migration_request("0123456789ABCDE0", reply, sizeof(reply)));
    assert(!bootloader_migration_request("0123456789abcdef", reply, sizeof(reply)));
    assert(!bootloader_migration_request("0123456789ABCDEF ", reply, sizeof(reply)));
    assert(!bootloader_migration_pending());
    now = 2000;
    bootloader_migration_tick(true, true);
    no_install();
  } else if (std::strcmp(scenario, "grace-and-drain") == 0) {
    arm();
    now = 1499;
    bootloader_migration_tick(true, true);
    no_install();
    now = 1500;
    bootloader_migration_tick(false, true);
    no_install();
    now = 29999;
    bootloader_migration_tick(false, true);
    no_install();
    assert(bootloader_migration_pending());
    bootloader_migration_tick(true, true);
    assert(endf_checks == 1);
    assert(!bootloader_migration_pending());
    assert(disables == 0 && resets == 0 && copies == 0);
    status_result(1);
  } else if (std::strcmp(scenario, "duplicate-arm") == 0) {
    arm();
    now = 1499;
    assert(!bootloader_migration_request(confirmation, reply, sizeof(reply)));
    assert(std::strstr(reply, "already armed"));
    now = 1500;
    bootloader_migration_tick(true, true);
    assert(endf_checks == 1); // A duplicate must not extend the grace period.
  } else if (std::strcmp(scenario, "timeout") == 0) {
    arm();
    now = 30000;
    bootloader_migration_tick(true, true);
    assert(!bootloader_migration_pending());
    no_install();
    status_result(15);
    bootloader_migration_tick(true, true);
    assert(endf_checks == 0);
  } else if (std::strcmp(scenario, "ota-cancel-rearm") == 0) {
    arm();
    now = 100;
    bootloader_migration_tick(false, false);
    assert(!bootloader_migration_pending());
    no_install();
    status_result(16);
    now = 2000;
    bootloader_migration_tick(true, true);
    assert(endf_checks == 0); // Becoming idle must not revive cancelled work.
    arm();
    now = 3499;
    bootloader_migration_tick(true, true);
    assert(endf_checks == 0);
    now = 3500;
    bootloader_migration_tick(true, true);
    assert(endf_checks == 1);
  } else if (std::strcmp(scenario, "ota-cancel-at-install") == 0) {
    arm();
    now = 1500;
    bootloader_migration_tick(true, false);
    assert(!bootloader_migration_pending());
    no_install();
    status_result(16);
  } else if (std::strcmp(scenario, "stale-retained-result") == 0) {
    power_registers.GPREGRET = 0xCC;
    status_result(12);
    arm();
    status_result(0);
    now = 30000;
    bootloader_migration_tick(true, true);
    assert(!bootloader_migration_pending());
    status_result(15);
    arm();
    status_result(0);
    ++now;
    bootloader_migration_tick(true, false);
    assert(!bootloader_migration_pending());
    status_result(16);
    assert(power_registers.GPREGRET == 0xCC);
    assert(endf_checks == 0);
  } else if (std::strcmp(scenario, "clock-wrap") == 0) {
    now = UINT32_MAX - 999u;
    arm();
    now += 1499u;
    bootloader_migration_tick(true, true);
    no_install();
    ++now;
    bootloader_migration_tick(true, true);
    assert(endf_checks == 1);
  } else {
    throw std::runtime_error("unknown scenario");
  }
  assert(disables == 0 && resets == 0 && copies == 0);
}
