#!/usr/bin/env python3
"""Require executable KISS transport and commands in uploadable Full images."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import host_kiss_capabilities as kiss
from test_meshtext_license import elf32

SYMBOLS = ("_ZN4mesh8hostkiss5Codec4feedEh",
           "_ZN4mesh8hostkiss5Codec11queueOutputEhPKhj",
           "_ZN4mesh10Dispatcher22queueForeignRadioFrameEPKhjhmNS_20RadioProfileProtocolE")
COMMANDS = b"\0".join(kiss.COMMAND_MARKERS)


class HostKissCapabilitiesTest(unittest.TestCase):
    def test_effective_recipe_matches_header_overrides_and_primary_radio(self):
        def required(*flags, platform="ESP32_PLATFORM", companion=True, overlay="", unflags="", config_unflags=()):
            config = [["env:base", [["build_flags", list(flags)], ["build_unflags", list(config_unflags)]]]]
            return kiss.required(config, "base", platform, companion, overlay, unflags)
        self.assertTrue(required())
        self.assertTrue(required(platform="NRF52_PLATFORM"))
        self.assertFalse(required(companion=False))
        self.assertTrue(required("-DMESH_ENABLE_HOST_KISS=1", companion=False))
        self.assertFalse(required("-DMESH_ENABLE_HOST_KISS=0"))
        self.assertFalse(required("-DPOLICY=0", "-DMESH_ENABLE_HOST_KISS=POLICY"))
        self.assertFalse(required("-DMESH_ENABLE_HOST_KISS=(0UL)"))
        self.assertTrue(required("-DMESH_ENABLE_HOST_KISS=0", overlay="-DMESH_ENABLE_HOST_KISS=1"))
        self.assertFalse(required("-DMESH_ENABLE_HOST_KISS=1", overlay="-DMESH_ENABLE_HOST_KISS=0"))
        self.assertTrue(required("-DMESH_ENABLE_HOST_KISS=0", overlay="-UMESH_ENABLE_HOST_KISS"))
        self.assertTrue(required("-DMESH_ENABLE_HOST_KISS=0", config_unflags=["-DMESH_ENABLE_HOST_KISS"]))
        self.assertTrue(required("-DMESH_ENABLE_HOST_KISS=0", unflags="-DMESH_ENABLE_HOST_KISS=0"))
        self.assertFalse(required("-DMESH_PRIMARY_ESPNOW=1", "-DMESH_ENABLE_HOST_KISS=1"))
        self.assertTrue(required("-DMESH_PRIMARY_ESPNOW=1", "-UMESH_PRIMARY_ESPNOW"))
        self.assertFalse(required("-DMESH_PRIMARY_ESPNOW=1", "-UMESH_PRIMARY_ESPNOW",
                                  config_unflags=["-UMESH_PRIMARY_ESPNOW"]))
        self.assertTrue(required("-DMESH_PRIMARY_ESPNOW=0"))
        self.assertFalse(required("-DCOMPANION_RADIO_FULL=0"))
        self.assertFalse(required("-DMESH_ENABLE_HOST_KISS=1", platform="STM32_PLATFORM"))
        self.assertFalse(required("-DMESH_ENABLE_HOST_KISS=1", platform="RP2040_PLATFORM"))

    def qualify(self, *, symbols=SYMBOLS, flags=6, undefined=(), zero=(),
                commands=COMMANDS, platform="ESP32_PLATFORM", tcp=True, require=True):
        with tempfile.TemporaryDirectory(prefix="mesh-kiss-capabilities-") as directory:
            root = Path(directory)
            elf32(root / "firmware.elf", symbols=symbols, flags=flags, undefined=undefined, zero=zero)
            app = commands + (b"\0" + kiss.TCP_MARKER if tcp else b"")
            args = []
            if platform == "NRF52_PLATFORM":
                with zipfile.ZipFile(root / "firmware.zip", "w") as package:
                    package.writestr("manifest.json", json.dumps({"manifest": {"application": {"bin_file": "app.bin"}}}))
                    package.writestr("app.bin", app)
                args += ["--dfu-package", str(root / "firmware.zip")]
            else:
                (root / "firmware.bin").write_bytes(app)
                args += ["--firmware-bin", str(root / "firmware.bin")]
            if require:
                args.append("--require-host-kiss")
            result = subprocess.run([sys.executable, str(ROOT / "scripts/check_firmware_capabilities.py"),
                "--image", str(root / "firmware.elf"), "--output", str(root / "caps.json"),
                "--target", "fixture", "--platform", platform, "--build-profile", "full",
                "--capability", kiss.CAPABILITY, "--capability", kiss.USB_CAPABILITY,
                "--capability", kiss.TCP_CAPABILITY, *args], capture_output=True, text=True)
            return result, json.loads((root / "caps.json").read_text())

    def test_packaged_usb_and_platform_specific_tcp_capabilities(self):
        for platform in ("ESP32_PLATFORM", "NRF52_PLATFORM"):
            with self.subTest(platform=platform):
                result, manifest = self.qualify(platform=platform, tcp=platform=="ESP32_PLATFORM")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(manifest["verified"])
                self.assertIn(kiss.CAPABILITY, manifest["capabilities"])
                self.assertIn(kiss.USB_CAPABILITY, manifest["capabilities"])
                # Requested labels alone must not advertise unsupported TCP.
                self.assertEqual(kiss.TCP_CAPABILITY in manifest["capabilities"], platform=="ESP32_PLATFORM")

    def test_rejects_each_missing_or_nonexecutable_method_and_command(self):
        cases = [dict(symbols=("unrelated_function",)), dict(flags=4), dict(flags=2), dict(tcp=False)]
        for symbol in SYMBOLS:
            cases.extend((dict(symbols=tuple(item for item in SYMBOLS if item!=symbol)),
                          dict(undefined=(symbol,)), dict(zero=(symbol,))))
        cases += [dict(commands=b"\0".join(marker for marker in kiss.COMMAND_MARKERS if marker!=missing))
                  for missing in kiss.COMMAND_MARKERS]
        for options in cases:
            with self.subTest(options=options):
                result, manifest = self.qualify(**options)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertFalse(manifest["verified"])
                self.assertFalse(set(manifest["capabilities"]) &
                                 {kiss.CAPABILITY,kiss.USB_CAPABILITY,kiss.TCP_CAPABILITY})
        result, manifest = self.qualify(require=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(set(manifest["capabilities"]) &
                         {kiss.CAPABILITY,kiss.USB_CAPABILITY,kiss.TCP_CAPABILITY})

    def test_actual_builder_checks_linked_code_and_packaged_application(self):
        script = r'''
set -euo pipefail
pio() { printf 'unexpected PlatformIO invocation\n' >&2; return 99; }
source build.sh
PIO_CONFIG_JSON=$MESH_TEST_CONFIG
OUTPUT_DIR=$MESH_TEST_DIRECTORY/out
PIO_BUILD_DIR_OVERRIDE=$MESH_TEST_DIRECTORY/build
BUILD_PROFILE_FOR_TARGET=full
BUILD_RECIPE_SHA256=test
BUILD_CAPABILITIES=(); BUILD_EXPECTATIONS=()
BUILD_APPLICATION_EXPECTATIONS=(); BUILD_REDUCTIONS=()
requires_wireless_self_update() { return 1; }
is_xiao_qspi_canonical_repeater_build() { return 1; }
is_nrf52_sensor_ota_pair_target() { return 1; }
embedded_bootloader_artifact_target() { printf '%s\n' "$1"; }
python3() {
  if [ "$1" = scripts/firmware_build_recipe.py ]; then return 0; fi
  command "$MESH_TEST_PYTHON" "$@"
}
write_build_capability_manifest fixture_repeater "$MESH_TEST_PLATFORM" base fixture
'''
        for platform in ("ESP32_PLATFORM", "NRF52_PLATFORM"):
            for complete in (True, False):
                with self.subTest(platform=platform, complete=complete), tempfile.TemporaryDirectory() as folder:
                    root=Path(folder);build=root/"build/base";build.mkdir(parents=True)
                    elf32(build/"firmware.elf", symbols=SYMBOLS if complete else ("unrelated_function",))
                    app=COMMANDS+b"\0"+kiss.TCP_MARKER
                    (build/"firmware.bin").write_bytes(app)
                    with zipfile.ZipFile(build/"firmware.zip", "w") as package:
                        package.writestr("manifest.json", json.dumps({"manifest":{"application":{"bin_file":"app.bin"}}}))
                        package.writestr("app.bin", app)
                    config=[["env:base", [["build_flags", ["-DMESH_ENABLE_HOST_KISS=1"]]]]]
                    result=subprocess.run(["bash","-c",script],cwd=ROOT,capture_output=True,text=True,
                        env={**os.environ,"MESH_TEST_CONFIG":json.dumps(config),"MESH_TEST_PLATFORM":platform,
                             "MESH_TEST_DIRECTORY":str(root),"MESH_TEST_PYTHON":sys.executable,
                             "PLATFORMIO_BUILD_FLAGS":"","PLATFORMIO_BUILD_UNFLAGS":""})
                    self.assertEqual(result.returncode,0 if complete else 1,result.stdout+result.stderr)
                    manifest=json.loads((root/"out/fixture.capabilities.json").read_text())
                    self.assertEqual(manifest["verified"],complete)
                    self.assertEqual(kiss.CAPABILITY in manifest["capabilities"],complete)

    def test_builder_requires_service_only_when_effectively_enabled(self):
        script = r'''
set -euo pipefail
pio() { printf 'unexpected PlatformIO invocation\n' >&2; return 99; }
source build.sh
PIO_CONFIG_JSON=$MESH_TEST_CONFIG
is_companion_radio_full_target() { [ "$MESH_TEST_COMPANION" = 1 ]; }
is_kiss_modem_target() { [ "$MESH_TEST_MODEM" = 1 ]; }
requires_host_kiss_code fixture base "$MESH_TEST_PLATFORM"
'''
        for platform, companion, modem, flags, expected in (
                ("ESP32_PLATFORM", True, False, [], 0),
                ("NRF52_PLATFORM", True, False, [], 0),
                ("ESP32_PLATFORM", False, False, ["-DMESH_ENABLE_HOST_KISS=1"], 0),
                ("NRF52_PLATFORM", False, False, ["-DMESH_ENABLE_HOST_KISS=1"], 0),
                ("ESP32_PLATFORM", True, False, ["-DMESH_ENABLE_HOST_KISS=0"], 1),
                ("ESP32_PLATFORM", True, False, ["-DMESH_ENABLE_HOST_KISS=1", "-DMESH_PRIMARY_ESPNOW=1"], 1),
                ("ESP32_PLATFORM", True, True, ["-DMESH_ENABLE_HOST_KISS=1"], 1),
                ("STM32_PLATFORM", True, False, ["-DMESH_ENABLE_HOST_KISS=1"], 1),
                ("ESP32_PLATFORM", False, False, [], 1)):
            with self.subTest(platform=platform,companion=companion,modem=modem,flags=flags):
                config = [["env:base", [["build_flags",flags]]]]
                result = subprocess.run(["bash","-c",script],cwd=ROOT,capture_output=True,text=True,
                    env={**os.environ,"MESH_TEST_CONFIG":json.dumps(config),"MESH_TEST_PLATFORM":platform,
                         "MESH_TEST_COMPANION":str(int(companion)),"MESH_TEST_MODEM":str(int(modem)),
                         "PLATFORMIO_BUILD_FLAGS":"","PLATFORMIO_BUILD_UNFLAGS":""})
                self.assertEqual(result.returncode,expected,result.stdout+result.stderr)


if __name__ == "__main__":
    unittest.main()
