#!/usr/bin/env python3
"""Host checks for the opt-in internal-only bootloader migration bridge."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib

from bootloader_migration_resource import BOOT_BASE, BOOT_SIZE, emit_resource, load_bootloader, validate_candidate

ROOT = Path(__file__).resolve().parents[2]


def with_crc(image):
    image = bytearray(image)
    offset = BOOT_SIZE - 36
    struct.pack_into('<I', image, offset, 0)
    struct.pack_into('<I', image, offset, zlib.crc32(image))
    return bytes(image)


def candidate(version=0x02040CFF):
    image = bytearray(b'\xff' * BOOT_SIZE)
    struct.pack_into('<II', image, 0, 0x20030000, 0xF4101)
    offset = BOOT_SIZE - 76
    struct.pack_into('<IIHHIII16sI', image, offset, 0x464D4C42, 0x31435243,
                     1, 44, 0xF4000, BOOT_SIZE, 0x239A0029, b'4631_DFU', 0)
    struct.pack_into('<IIHHIHHIHHI', image, offset + 44, 0x324D4C42, 0x54464F53,
                     2, 32, version, 140, 0xB6, 0x26000, 1, 0, 0)
    return with_crc(image)


class HookEnvironment(dict):
    def __init__(self, build_directory, name='RAK_4631_repeater'):
        super().__init__(PIOENV=name, PROJECT_DIR=str(ROOT))
        self.build_directory = str(build_directory)
        self.append_calls = []
        self.source_calls = []

    def subst(self, value):
        if value != '$BUILD_DIR':
            raise AssertionError(value)
        return self.build_directory

    def Append(self, **values):
        self.append_calls.append(values)

    def BuildSources(self, *args, **values):
        self.source_calls.append((args, values))


def run_hook(environment, variables):
    script = ROOT / 'tools/mota/pio_bootloader_migration.py'
    with mock.patch.dict(os.environ, variables, clear=True), mock.patch.object(sys, 'path', sys.path.copy()):
        exec(compile(script.read_text(), str(script), 'exec'),
             {'Import': lambda name: None, 'env': environment})


class MigrationTests(unittest.TestCase):
    def test_candidate_requires_exact_normal_board_and_layout(self):
        good = candidate()
        self.assertEqual(validate_candidate(good), 0x02040CFF)
        self.assertEqual(validate_candidate(candidate(0x02040C90)), 0x02040C90)
        for version in (0, 0xffffffff, 0x02040C00):
            with self.assertRaises(ValueError):
                validate_candidate(candidate(version))
        for offset in (0, 4, BOOT_SIZE - 76, BOOT_SIZE - 60, BOOT_SIZE - 56,
                       BOOT_SIZE - 52, BOOT_SIZE - 32, BOOT_SIZE - 12):
            with self.subTest(offset=offset):
                bad = bytearray(good)
                bad[offset] ^= 0x40
                with self.assertRaises(ValueError):
                    validate_candidate(bad)
        for length in (0, 1, BOOT_SIZE - 4, BOOT_SIZE + 4):
            with self.assertRaises(ValueError):
                validate_candidate(bytes(length))

    def test_candidate_schema_checks_are_independent_of_crc(self):
        # A CRC failure must not hide a missing board/layout validation check.
        offset = BOOT_SIZE - 76
        fields = {
            'stack': 0,
            'reset': 4,
            'envelope magic': offset,
            'CRC algorithm': offset + 4,
            'envelope version': offset + 8,
            'envelope length': offset + 10,
            'bootloader base': offset + 12,
            'bootloader size': offset + 16,
            'board ID': offset + 20,
            'UF2 model': offset + 24,
            'extension magic': offset + 44,
            'extension type': offset + 48,
            'extension version': offset + 52,
            'extension length': offset + 54,
            'SoftDevice type': offset + 60,
            'SoftDevice FWID': offset + 62,
            'application base': offset + 64,
            'required layout': offset + 68,
            'reserved flags': offset + 70,
            'reserved word': offset + 72,
        }
        for name, position in fields.items():
            with self.subTest(field=name):
                bad = bytearray(candidate())
                bad[position] ^= 1 if position in (0, 4) else 0x40
                with self.assertRaises(ValueError):
                    validate_candidate(with_crc(bad))

    def test_hex_loader_extracts_only_bootloader_and_pads_sparse_records(self):
        from intelhex import IntelHex
        with tempfile.TemporaryDirectory(prefix='migration-hex-') as directory:
            directory = Path(directory)
            path = directory / 'release.hex'
            image = IntelHex()
            # Released packages may also contain SoftDevice and UICR records;
            # neither belongs in the persistent COPY_BL resource or its hash.
            image[0] = 0x12
            image[0x10001014] = 0x34
            image.puts(BOOT_BASE, candidate())
            image.write_hex_file(str(path))
            self.assertEqual(load_bootloader(path), candidate())
            sparse = IntelHex()
            sparse[BOOT_BASE] = 0x56
            sparse[BOOT_BASE + BOOT_SIZE - 1] = 0x78
            sparse.write_hex_file(str(path))
            self.assertEqual(load_bootloader(path), b'\x56' + b'\xff' * (BOOT_SIZE - 2) + b'\x78')

    def test_hook_is_noop_without_explicit_opt_in(self):
        # No environment access, output generation or migration define is
        # permitted on an ordinary build (including an unrelated nRF52 board).
        run_hook({}, {})

    def test_hook_requires_both_inputs_and_qualified_environment(self):
        for variables in ({'MC_MIGRATION_LEGACY_BOOTLOADER': 'missing.hex'},
                          {'MC_MIGRATION_CANDIDATE_BOOTLOADER': 'missing.hex'}):
            with self.subTest(variables=variables), self.assertRaisesRegex(RuntimeError, 'both exact'):
                run_hook({}, variables)
        for name in ('RAK_4631_repeater_slim', 'RAK_4631_companion_radio_usb', 'Heltec_t114'):
            environment = HookEnvironment('unused', name)
            with self.subTest(environment=name), self.assertRaisesRegex(RuntimeError, 'restricted'):
                run_hook(environment, {'MC_MIGRATION_LEGACY_BOOTLOADER': 'missing.hex',
                                       'MC_MIGRATION_CANDIDATE_BOOTLOADER': 'missing.hex'})
            self.assertEqual(environment.append_calls, [])
            self.assertEqual(environment.source_calls, [])

    def test_hook_generates_current_resources_and_only_explicit_sources(self):
        with tempfile.TemporaryDirectory(prefix='migration-hook-') as directory:
            directory = Path(directory)
            old = directory / 'legacy.bin'
            new = directory / 'candidate.bin'
            old.write_bytes(bytes(BOOT_SIZE))
            new.write_bytes(candidate())
            variables = {'MC_MIGRATION_LEGACY_BOOTLOADER': str(old),
                         'MC_MIGRATION_CANDIDATE_BOOTLOADER': str(new)}
            environment = HookEnvironment(directory / 'build')
            run_hook(environment, variables)
            generated = directory / 'build/bootloader-migration-resource'
            first_source = (generated / 'BootloaderMigrationResource.cpp').read_text()
            self.assertEqual(environment.append_calls, [
                {'CPPDEFINES': [('OTA_BOOTLOADER_MIGRATION', 1)]},
                {'CPPPATH': [str(generated)]}])
            self.assertEqual(environment.source_calls, [
                ((str(generated / 'objects'), str(generated)), {'src_filter': ['+<*.cpp>']})])
            # The hook must not reuse yesterday's bytes if a package is
            # replaced at the same path; SCons receives the refreshed source.
            new.write_bytes(candidate(0x02040DFF))
            run_hook(HookEnvironment(directory / 'build'), variables)
            second_source = (generated / 'BootloaderMigrationResource.cpp').read_text()
            self.assertNotEqual(first_source, second_source)
            old.write_bytes(bytes([0xAA]) * BOOT_SIZE)
            run_hook(HookEnvironment(directory / 'build'), variables)
            self.assertNotEqual(second_source,
                                (generated / 'BootloaderMigrationResource.cpp').read_text())

    def test_resource_is_internal_const_and_pins_both_hashes(self):
        with tempfile.TemporaryDirectory(prefix='migration-resource-') as directory:
            directory = Path(directory)
            old = directory / 'legacy.bin'
            new = directory / 'candidate.bin'
            old.write_bytes(bytes(BOOT_SIZE))
            new.write_bytes(candidate())
            emit_resource(old, new, directory / 'out')
            generated = (directory / 'out/BootloaderMigrationResource.cpp').read_text()
            self.assertIn('alignas(4) const uint8_t migration_candidate[40960]', generated)
            self.assertIn('migration_candidate_sha256[32]', generated)
            self.assertIn('migration_legacy_sha256[32]', generated)
            with self.assertRaises(ValueError):
                emit_resource(new, new, directory / 'same')

    def test_production_policy_rejects_unsafe_geometry_and_acl(self):
        code = r'''
#include <cassert>
#include "helpers/ota/BootloaderMigrationPolicy.h"
using namespace mesh::ota::migration;
static Snapshot good() {
  Snapshot s{};
  s.part=0x52840; s.flash_page_size=4096; s.flash_pages=256;
  s.softdevice_size=APP_BASE; s.softdevice_fwid=0xB6;
  s.mbr_boot_override=UINT32_MAX; s.mbr_params_override=UINT32_MAX;
  s.uicr_boot=BOOT_BASE; s.uicr_params=PARAMS_BASE;
  s.source=0xA0000; s.source_size=BOOT_SIZE; s.app_body_size=0x90000;
  return s;
}
int main() {
  static_assert(ACL_READ_DENIED == 4, "nRF52840 read deny is bit 2");
  static_assert(ACL_WRITE_DENIED == 2, "nRF52840 write deny is bit 1");
  assert(preflight(good())==Gate::Ok);
  Snapshot s=good(); s.part=0x52833; assert(preflight(s)==Gate::Chip);
  s=good(); s.flash_page_size=2048; assert(preflight(s)==Gate::Chip);
  s=good(); s.flash_pages=512; assert(preflight(s)==Gate::Chip);
  s=good(); s.softdevice_size=0x27000; assert(preflight(s)==Gate::Softdevice);
  s=good(); s.softdevice_fwid=0x123; assert(preflight(s)==Gate::Softdevice);
  s=good(); s.uicr_boot=0xF0000; assert(preflight(s)==Gate::BootAddress);
  s=good(); s.mbr_boot_override=0xF0000; assert(preflight(s)==Gate::BootAddress);
  s=good(); s.mbr_boot_override=BOOT_BASE; s.uicr_boot=0; assert(preflight(s)==Gate::Ok);
  s=good(); s.uicr_params=UINT32_MAX; assert(preflight(s)==Gate::ParamsAddress);
  s=good(); s.mbr_params_override=0xFF000; assert(preflight(s)==Gate::ParamsAddress);
  const uint32_t bad_sources[]={0,0x1000,APP_BASE-4,0xED000,BOOT_BASE,0xFE000,0x20000000,0x12000000,UINT32_MAX-3};
  for (auto address:bad_sources) { s=good(); s.source=address; assert(preflight(s)==Gate::Source); }
  s=good(); ++s.source; assert(preflight(s)==Gate::Source);
  s=good(); --s.source_size; assert(preflight(s)==Gate::Source);
  s=good(); s.app_body_size=0; assert(preflight(s)==Gate::Source);
  s=good(); s.app_body_size=UINT32_MAX; assert(preflight(s)==Gate::Source);
  s=good(); s.app_body_size=s.source-APP_BASE+BOOT_SIZE-1; assert(preflight(s)==Gate::Source);
  for (unsigned slot=0; slot<8; ++slot) {
    for (uint32_t address=BOOT_BASE; address<SETTINGS_BASE; address+=4096) {
      s=good(); s.acl[slot]={address,4096,2}; assert(preflight(s)==Gate::Protection);
      s.acl[slot].permissions=4; assert(preflight(s)==Gate::Protection);
      s.acl[slot].permissions=6; assert(preflight(s)==Gate::Protection);
      s.acl[slot].permissions=0; assert(preflight(s)==Gate::Ok);
    }
    s=good(); s.acl[slot]={s.source,4096,4}; assert(preflight(s)==Gate::Protection);
    s=good(); s.acl[slot]={APP_BASE,4096,2}; assert(preflight(s)==Gate::Ok);
    s=good(); s.acl[slot]={SETTINGS_BASE,4096,2}; assert(preflight(s)==Gate::Ok);
    s=good(); s.acl[slot]={0xFFFFF000,0x10000,3}; assert(preflight(s)==Gate::Ok);
  }
}
'''
        with tempfile.TemporaryDirectory(prefix='migration-policy-') as directory:
            binary = Path(directory) / ('policy.exe' if os.name == 'nt' else 'policy')
            subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-O2', '-Wall', '-Wextra',
                            '-Werror', '-I', str(ROOT / 'src'), '-x', 'c++', '-', '-o', str(binary)],
                           input=code, text=True, check=True)
            subprocess.run([str(binary)], check=True)

    def test_driver_never_erases_app_settings_or_writes_acl(self):
        driver = (ROOT / 'src/helpers/ota/BootloaderMigration.cpp').read_text()
        self.assertNotIn('nrfx_nvmc_', driver)
        self.assertNotIn('flash_nrf5x_', driver)
        self.assertNotIn('malloc(', driver)
        self.assertIn('command.command = SD_MBR_COMMAND_COPY_BL', driver)
        self.assertIn('command.params.copy_bl.bl_len = migration::BOOT_SIZE / sizeof(uint32_t)', driver)
        self.assertLess(driver.index('sd_softdevice_disable()'), driver.index('NRF_ACL->'))
        self.assertIn('__attribute__((naked, noipa))', driver)
        self.assertIn('"r0", "memory"', driver)
        self.assertLess(driver.index('migration::preflight(snapshot)'), driver.index('migration_mbr_command(&command)'))
        self.assertLess(driver.index('migration_legacy_sha256'), driver.index('migration_mbr_command(&command)'))


if __name__ == '__main__':
    unittest.main()
