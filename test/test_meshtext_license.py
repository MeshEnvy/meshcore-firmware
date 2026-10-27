"""Qualify linked chat code and ship its MIT notice without touching firmware."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import package_meshtext_license as license
import firmware_memory_manifest as memory
import firmware_build_recipe as recipe
import package_cascade_release as release

SYMBOLS = ("_ZN4mesh2mt10encodeTextEj", "_ZN4mesh2mt10decodeTextEj",
           "_ZN6MyMesh16sendMeshtextTextEj", "_ZN6MyMesh19onForeignRadioFrameEj",
           "_ZN4mesh10Dispatcher22queueForeignRadioFrameEj")


def elf32(path, symbols=SYMBOLS, flags=6, undefined=(), zero=()):
    names, table = bytearray(b"\0"), bytearray(16)
    data = b"CODE" * max(1, len(symbols))
    for i, name in enumerate(symbols):
        offset = len(names)
        names += name.encode() + b"\0"
        table += struct.pack("<IIIBBH", offset, 0x40000000 + i * 4,
                             0 if name in zero else 4, 0x12, 0, 0 if name in undefined else 1)
    payload = 52 + 4 * 40
    strings = payload + len(data)
    symtab = strings + len(names)
    header = b"\x7fELF\x01\x01\x01" + bytes(9)
    header += struct.pack("<HH5I6H", 2, 40, 1, 0, 0, 52, 0, 52, 0, 0, 40, 4, 0)
    sections = bytes(40)
    sections += struct.pack("<10I", 0, 1, flags, 0x40000000, payload, len(data), 0, 0, 4, 0)
    sections += struct.pack("<10I", 0, 3, 0, 0, strings, len(names), 0, 0, 1, 0)
    sections += struct.pack("<10I", 0, 2, 0, 0, symtab, len(table), 2, 1, 4, 16)
    path.write_bytes(header + sections + data + names + table)


class MeshtextLicenseTest(unittest.TestCase):
    def builder_contract(self, target, platform, *, flags=(), overlay="", unflags="",
                         config_unflags=(), symbols=("unrelated_defined_function",), invalid_config=False):
        with tempfile.TemporaryDirectory(prefix="meshtext-build-contract-") as folder:
            root = Path(folder)
            build = root / "build/base"
            build.mkdir(parents=True)
            elf32(build / "firmware.elf", symbols=symbols)
            config = [["env:base", [["build_flags", ["-DCOMPANION_RADIO_FULL=1", "-DMESH_ENABLE_HOST_KISS=0", *flags]],
                                    ["build_unflags", list(config_unflags)]]]]
            script = r'''
set -euo pipefail
pio() { printf 'unexpected PlatformIO invocation\n' >&2; return 99; }
source build.sh
PIO_ENV_PLATFORM_BY_NAME[$1]=$2
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
  # Only the unrelated recipe attachment is substituted. The real builder
  # decision and ELF capability checker must reject omitted chat code.
  if [ "$1" = scripts/firmware_build_recipe.py ]; then return 0; fi
  command "$MESH_TEST_PYTHON" "$@"
}
write_build_capability_manifest "$1" "$2" base fixture
'''
            result = subprocess.run(["bash", "-c", script, "meshtext-contract", target, platform],
                cwd=ROOT, text=True, capture_output=True, env={**os.environ,
                    "MESH_TEST_CONFIG": "invalid JSON" if invalid_config else json.dumps(config),
                    "MESH_TEST_DIRECTORY": str(root), "MESH_TEST_PYTHON": sys.executable,
                    "PLATFORMIO_BUILD_FLAGS": overlay, "PLATFORMIO_BUILD_UNFLAGS": unflags})
            manifest = root / "out/fixture.capabilities.json"
            return result, json.loads(manifest.read_text()) if manifest.exists() else None

    def test_builder_requires_real_chat_on_every_full_companion_family(self):
        for target, platform in (
            ("RAK_4631_companion_radio_full", "NRF52_PLATFORM"),
            ("RAK_3401_companion_radio_full", "NRF52_PLATFORM"),
            ("t1000e_companion_radio_full", "NRF52_PLATFORM"),
            ("Xiao_nrf52_companion_radio_full", "NRF52_PLATFORM"),
            ("Heltec_v2_companion_radio_full", "ESP32_PLATFORM"),
            ("heltec_v4_companion_radio_full_femon", "ESP32_PLATFORM"),
            ("Xiao_C3_companion_radio_full", "ESP32_PLATFORM"),
            ("Xiao_C6_companion_radio_full_", "ESP32_PLATFORM"),
        ):
            with self.subTest(target=target):
                result, manifest = self.builder_contract(target, platform)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertFalse(manifest["verified"])
                self.assertIn("promised", result.stderr)
        result, manifest = self.builder_contract("t1000e_companion_radio_full", "NRF52_PLATFORM",
                                                 symbols=SYMBOLS)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(manifest["verified"])
        self.assertIn(license.CAPABILITY, manifest["capabilities"])

    def test_builder_preserves_effective_feature_overrides_and_espnow_exclusion(self):
        cases = (
            (dict(flags=["-DMESH_ENABLE_MESHTEXT_CHAT=0"]), False),
            (dict(overlay="-D MESH_ENABLE_MESHTEXT_CHAT=0"), False),
            (dict(flags=["-DMESH_ENABLE_MESHTEXT_CHAT=0"], overlay="-DMESH_ENABLE_MESHTEXT_CHAT=1"), True),
            (dict(flags=["-DMESH_ENABLE_MESHTEXT_CHAT=1"], overlay="-DMESH_ENABLE_MESHTEXT_CHAT=0"), False),
            (dict(flags=["-DMESH_PRIMARY_ESPNOW=1"]), False),
            (dict(flags=["-DMESH_PRIMARY_ESPNOW=1", "-UMESH_PRIMARY_ESPNOW"]), True),
            (dict(flags=["-DMESH_PRIMARY_ESPNOW=1"], overlay="-DMESH_PRIMARY_ESPNOW=0"), True),
            (dict(flags=["-DMESH_PRIMARY_ESPNOW=1", "-DMESH_ENABLE_MESHTEXT_CHAT=1"]), True),
            (dict(flags=["-DMESH_ENABLE_MESHTEXT_CHAT=0"], overlay="-UMESH_ENABLE_MESHTEXT_CHAT"), True),
            (dict(flags=["-DMESH_ENABLE_MESHTEXT_CHAT=0"], unflags="-D MESH_ENABLE_MESHTEXT_CHAT=0"), True),
            (dict(flags=["-DMESH_ENABLE_MESHTEXT_CHAT=0"], config_unflags=["-DMESH_ENABLE_MESHTEXT_CHAT"]), True),
            (dict(flags=["-DMESH_PRIMARY_ESPNOW=1", "-UMESH_PRIMARY_ESPNOW"],
                  config_unflags=["-UMESH_PRIMARY_ESPNOW"]), False),
            (dict(flags=["-DCHAT_POLICY=0", "-DMESH_ENABLE_MESHTEXT_CHAT=CHAT_POLICY"]), False),
            (dict(flags=["-DMESH_ENABLE_MESHTEXT_CHAT=(0UL)"]), False),
            (dict(flags=["-DCOMPANION_RADIO_FULL=0"]), False),
        )
        for options, required in cases:
            with self.subTest(options=options):
                result, manifest = self.builder_contract("Heltec_v2_companion_radio_full",
                                                         "ESP32_PLATFORM", **options)
                self.assertEqual(result.returncode, int(required), result.stdout + result.stderr)
                self.assertEqual(manifest["verified"], not required)
        for target, platform in (("t1000e_companion_radio_usb", "NRF52_PLATFORM"),
                                 ("PicoW_companion_radio_full", "RP2040_PLATFORM"),
                                 ("RAK_3x72_companion_radio_full", "STM32_PLATFORM")):
            with self.subTest(target=target):
                result, manifest = self.builder_contract(target, platform)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue(manifest["verified"])
        result, manifest = self.builder_contract("Heltec_v2_companion_radio_full", "ESP32_PLATFORM",
                                                 invalid_config=True)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIsNone(manifest)
        self.assertIn("Cannot determine", result.stderr)

    def qualify(self, build, stem):
        output = stem.parent / (stem.name + ".capabilities.json")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/check_firmware_capabilities.py"),
            "--image", str(build / "firmware.elf"), "--output", str(output),
            "--target", "test_companion_radio_usb", "--platform", "ESP32_PLATFORM",
            "--build-profile", "standard"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(output.read_text())

    def require_chat(self, build, *extra_args):
        output = build / "required.capabilities.json"
        result = subprocess.run([sys.executable, str(ROOT / "scripts/check_firmware_capabilities.py"),
            "--image", str(build / "firmware.elf"), "--output", str(output),
            "--target", "RAK_4631_companion_radio_full", "--platform", "NRF52_PLATFORM",
            "--build-profile", "full", "--require-meshtext", *extra_args],
            capture_output=True, text=True)
        return result, json.loads(output.read_text())

    def test_required_chat_accepts_complete_linked_codec_and_transport(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            elf32(root / "firmware.elf")
            result, manifest = self.require_chat(root)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(manifest["verified"])
            self.assertIn(license.CAPABILITY, manifest["capabilities"])
            check = next(item for item in manifest["verification"]
                         if item["capability"] == license.CAPABILITY)
            self.assertTrue(check["present"])
            self.assertEqual(check["source"], "linked ELF symbols")
            self.assertEqual(len(check["evidence"].split("; ")), len(SYMBOLS))

    def test_required_chat_rejects_every_missing_or_nonrunning_implementation(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            cases = [("unrelated code", {"symbols": ("_ZN4mesh9unrelatedEv",)}),
                     ("codec only", {"symbols": SYMBOLS[:2]}),
                     ("transport only", {"symbols": SYMBOLS[2:]}),
                     ("unallocated", {"flags": 4}),
                     ("data section", {"flags": 2})]
            for symbol in SYMBOLS:
                cases += [("missing " + symbol, {"symbols": tuple(name for name in SYMBOLS if name != symbol)}),
                          ("undefined " + symbol, {"undefined": (symbol,)}),
                          ("zero size " + symbol, {"zero": (symbol,)})]
            for label, options in cases:
                with self.subTest(label=label):
                    elf32(root / "firmware.elf", **options)
                    result, manifest = self.require_chat(root)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertFalse(manifest["verified"])
                    self.assertNotIn(license.CAPABILITY, manifest["capabilities"])
                    self.assertIn("promised", result.stderr)
                    self.assertIn("linked ELF symbols", result.stderr)

    def test_required_chat_cannot_be_qualified_by_cli_strings_or_requested_capabilities(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            marker = "get mt.status"
            images = [b"CLI " + marker.encode() + b"\0" + b"\0".join(s.encode() for s in SYMBOLS)]
            elf32(root / "firmware.elf", symbols=SYMBOLS[:2])
            images.append((root / "firmware.elf").read_bytes() + marker.encode() +
                          b"\0".join(s.encode() for s in SYMBOLS))
            for image in images:
                with self.subTest(elf=image.startswith(b"\x7fELF")):
                    (root / "firmware.elf").write_bytes(image)
                    result, manifest = self.require_chat(root, "--capability", license.CAPABILITY,
                                                         "--expect", license.CAPABILITY + "=" + marker)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertFalse(manifest["verified"])
                    self.assertNotIn(license.CAPABILITY, manifest["capabilities"])
                    marker_check = next(item for item in manifest["verification"]
                                        if item["source"] == "linked image")
                    self.assertTrue(marker_check["present"])

    @unittest.skipUnless(sys.platform.startswith("linux"), "requires a Linux ELF compiler")
    def test_compiler_produced_symbols_qualify_chat_and_notice(self):
        compiler = shutil.which("g++")
        if compiler is None:
            self.skipTest("g++ is not installed")
        # Do not build symbol spellings from SYMBOLS: the compiler must independently
        # produce the Itanium name lengths that the production matcher recognizes.
        source = """
namespace mesh {
namespace mt {
unsigned encodeText(unsigned value) { return value + 1; }
unsigned decodeText(unsigned value) { return value + 2; }
}
class Dispatcher {
public: unsigned queueForeignRadioFrame(unsigned);
};
unsigned Dispatcher::queueForeignRadioFrame(unsigned value) { return value + 3; }
}
class MyMesh {
public:
    unsigned sendMeshtextText(unsigned);
    unsigned onForeignRadioFrame(unsigned);
};
unsigned MyMesh::sendMeshtextText(unsigned value) { return value + 4; }
unsigned MyMesh::onForeignRadioFrame(unsigned value) { return value + 5; }
extern "C" void _start() {}
"""
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            input_path = root / "linked.cpp"
            input_path.write_text(source)
            result = subprocess.run([compiler, "-m32", "-O0", "-fno-pie", "-no-pie",
                "-fno-exceptions", "-fno-rtti", "-fno-stack-protector", "-nostdlib",
                "-Wl,-e,_start", "-Wl,--build-id=none", str(input_path),
                "-o", str(root / "firmware.elf")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            codec, chat = license.meshtext_evidence(root / "firmware.elf")
            self.assertEqual(len(codec), 2)
            self.assertEqual(len(chat), 5)
            manifest = self.qualify(root, root / "compiled")
            self.assertIn(license.CAPABILITY, manifest["capabilities"])
            self.assertEqual(manifest["verification"][0]["source"], "linked ELF symbols")
            self.assertEqual(license.collect_build_notice(root, root / "compiled").read_bytes(),
                             license.notice_bytes())

    def test_only_allocated_defined_executable_symbols_qualify(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "firmware.elf"
            elf32(path)
            codec, chat = license.meshtext_evidence(path)
            self.assertEqual(len(codec), 2)
            self.assertEqual(len(chat), 5)
            for flags in (0, 2, 4):
                elf32(path, flags=flags)
                self.assertEqual(license.meshtext_evidence(path), ([], []))
            for missing in SYMBOLS:
                elf32(path, undefined=(missing,))
                self.assertFalse(license.meshtext_evidence(path)[1])
                elf32(path, zero=(missing,))
                self.assertFalse(license.meshtext_evidence(path)[1])
            path.write_bytes(b"debug strings " + b"\0".join(s.encode() for s in SYMBOLS))
            self.assertEqual(license.meshtext_evidence(path), ([], []))

    def test_partial_codec_still_requires_license_but_never_advertises_chat(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            elf32(root / "firmware.elf", symbols=SYMBOLS[:1])
            manifest = self.qualify(root, root / "partial")
            self.assertNotIn(license.CAPABILITY, manifest["capabilities"])
            self.assertEqual(manifest["third_party_notices"][0]["license"], "MIT")
            self.assertEqual(license.collect_build_notice(root, root / "partial").read_bytes(), license.notice_bytes())

    def test_unaffected_builds_reject_stale_output_notice(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            elf32(root / "firmware.elf", symbols=("unrelated_defined_function",))
            manifest = self.qualify(root, root / "plain")
            self.assertNotIn("third_party_notices", manifest)
            self.assertIsNone(license.collect_build_notice(root, root / "plain"))
            notice = license.sidecar(root / "plain")
            notice.write_bytes(license.notice_bytes())
            with self.assertRaisesRegex(ValueError, "stale"):
                license.collect_build_notice(root, root / "plain")
            self.assertEqual(notice.read_bytes(), license.notice_bytes())

    def test_notice_is_hashed_staged_and_recipe_bound_without_binary_changes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            build, output = root / "build", root / "out"
            build.mkdir(); output.mkdir()
            elf32(build / "firmware.elf")
            label = "v1.17.1.9-halo-keymind-cascade-dev"
            version = label + "-deadbeef"
            stem = output / ("test_companion_radio_usb-" + version)
            manifest = self.qualify(build, stem)
            self.assertIn(license.CAPABILITY, manifest["capabilities"])
            self.assertEqual(manifest["verification"][0]["source"], "linked ELF symbols")
            app, merged = b"qualified-application", b"qualified-merged"
            (output / (stem.name + ".bin")).write_bytes(app)
            (output / (stem.name + "-merged.bin")).write_bytes(merged)
            notice = license.collect_build_notice(build, stem)
            self.assertEqual(notice.read_bytes(), license.notice_bytes())
            self.assertEqual(license.collect_build_notice(build, stem), notice)
            report = dict(schema_version=1, passed=True, available_internal_bytes=80000,
                          required_heap_bytes=50000, largest_internal_region_bytes=80000,
                          required_contiguous_bytes=5120,
                          elf_sha256=hashlib.sha256((build / "firmware.elf").read_bytes()).hexdigest(), target="build_env")
            (build / "firmware.memory.json").write_text(json.dumps(report))
            memory.package_report(build, stem)
            proof = memory.validate_package(stem)
            self.assertEqual(proof["files"][notice.name], hashlib.sha256(notice.read_bytes()).hexdigest())
            records = release.collect_artifacts(output, version)
            self.assertEqual(sum(path == notice for path in records[0]["files"]), 1)
            self.assertTrue(recipe.package_occupied(stem))
            commit = "deadbeef" + "0" * 32
            status = root / "status"
            status.write_text("state=completed\nexit_code=0\n"
                f"working_directory={root}\noutput_directory=out\nsource_commit={commit}\n"
                f"firmware_version={label}\nfirmware_profile=cascade\n"
                "radio_frequency=909.5\nradio_bandwidth=500\nradio_sf=5\nradio_cr=5\n")
            result = subprocess.run([sys.executable, str(ROOT / "scripts/package_cascade_release.py"),
                "--input", str(output), "--output", str(root / "stage"), "--build-status", str(status),
                "--version", "1.17.1.9", "--commit", commit], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            staged = root / "stage/companion"
            self.assertEqual((staged / notice.name).read_bytes(), license.notice_bytes())
            self.assertIn(notice.name, (staged / "TARGET-MANIFEST.json").read_text())
            self.assertIn(notice.name, (staged / "SHA256SUMS.txt").read_text())
            self.assertEqual((output / (stem.name + ".bin")).read_bytes(), app)
            self.assertEqual((output / (stem.name + "-merged.bin")).read_bytes(), merged)
            notice.write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "mismatched"):
                memory.validate_package(stem)
            notice.unlink()
            with self.assertRaisesRegex(ValueError, "missing"):
                memory.validate_package(stem)

    def test_license_only_package_blocks_resume_and_missing_metadata_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            stem = root / "occupied"
            license.sidecar(stem).write_bytes(license.notice_bytes())
            self.assertTrue(recipe.package_occupied(stem))
            (root / "occupied.capabilities.json").write_text(json.dumps({"capabilities": [license.CAPABILITY]}))
            with self.assertRaisesRegex(ValueError, "lacks"):
                license.validate_artifact_notice(stem)


if __name__ == "__main__":
    unittest.main()
