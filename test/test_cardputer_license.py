"""Qualify and carry Cardputer UI/font notices without changing firmware or DFU ZIPs."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from test_pocket_gui_license import elf32 as synthetic_elf32

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import package_cardputer_license as notice
import firmware_memory_manifest as memory
import firmware_build_recipe as recipe
import package_cascade_release as release

GUI = "_ZN15CardputerUITask4loopEv"
FONT = "_ZL13DejaVuSans_11"


def elf32(path, symbols=(GUI, FONT), **kwargs):
    synthetic_elf32(path, symbols=symbols, **kwargs)


class CardputerLicenseTest(unittest.TestCase):
    def qualify(self, directory, stem, target="test_companion_radio_usb"):
        output = stem.parent / (stem.name + ".capabilities.json")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/check_firmware_capabilities.py"),
            "--image", str(directory / "firmware.elf"), "--output", str(output),
            "--target", target, "--platform", "ESP32_PLATFORM", "--build-profile", "standard"],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(output.read_text())

    @unittest.skipUnless(sys.platform.startswith("linux"), "requires a Linux ELF compiler")
    def test_compiler_produced_gui_and_font_symbols_qualify(self):
        compiler = shutil.which("g++")
        if compiler is None:
            self.skipTest("host g++ required")
        source = """
class CardputerUITask { public: void loop(); bool isAwake() const; };
void CardputerUITask::loop() { asm volatile(\"\"); }
bool CardputerUITask::isAwake() const { return false; }
static const unsigned char DejaVuSans_11[] = {1, 2, 3, 4};
static const unsigned char DejaVuSans_16[] = {5, 6, 7, 8};
static const unsigned char DejaVuSans_19[] = {9, 10, 11, 12};
extern \"C\" void _start() { asm volatile(\"\" :: \"r\"(DejaVuSans_11),
    \"r\"(DejaVuSans_16), \"r\"(DejaVuSans_19)); }
"""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            input_path = root / "linked.cpp"
            input_path.write_text(source)
            result = subprocess.run([compiler, "-m32", "-O0", "-fno-pie", "-no-pie",
                "-fno-exceptions", "-fno-rtti", "-fno-stack-protector", "-nostdlib",
                "-Wl,-e,_start", "-Wl,--build-id=none", str(input_path),
                "-o", str(root / "firmware.elf")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            evidence = notice.cardputer_evidence(root / "firmware.elf")
            self.assertEqual(len(evidence), 5)
            self.assertIn(FONT, evidence)
            self.qualify(root, root / "compiled")
            self.assertEqual(notice.collect_build_notice(root, root / "compiled").read_bytes(),
                             notice.notice_bytes())

    def test_notice_requires_allocated_gui_or_font_definitions(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "firmware.elf"
            elf32(path)
            self.assertEqual(notice.cardputer_evidence(path), sorted((GUI, FONT)))
            # A partially linked font still needs its font license.
            elf32(path, symbols=(FONT,), flags=2)
            self.assertEqual(notice.cardputer_evidence(path), [FONT])
            for font in ("DejaVuSans_16", "_ZL13DejaVuSans_19", "_ZL13DejaVuSans_99.lto_priv.0"):
                elf32(path, symbols=(font,), flags=2)
                self.assertEqual(notice.cardputer_evidence(path), [font])
            for kwargs in ({"flags": 0}, {"flags": 4}, {"section_type": 8},
                           {"symbols": (GUI,), "flags": 2},
                           {"symbols": (FONT, "other"), "undefined": (FONT,)},
                           {"symbols": (GUI, "other"), "zero": (GUI,)}):
                with self.subTest(kwargs=kwargs):
                    elf32(path, **kwargs)
                    self.assertEqual(notice.cardputer_evidence(path), [])
            path.write_bytes((GUI + " " + FONT).encode())
            self.assertEqual(notice.cardputer_evidence(path), [])

    def test_target_name_alone_never_creates_notice_and_stale_notice_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stem = root / "plain"
            elf32(root / "firmware.elf", symbols=("unrelated",))
            manifest = self.qualify(root, stem, "M5Stack_Cardputer_Adv_companion_radio_usb")
            self.assertNotIn("third_party_notices", manifest)
            self.assertIsNone(notice.collect_build_notice(root, stem))
            notice.sidecar(stem).write_bytes(notice.notice_bytes())
            with self.assertRaisesRegex(ValueError, "stale"):
                notice.collect_build_notice(root, stem)
            self.assertTrue(recipe.package_occupied(stem))

    def test_notice_is_hashed_with_memory_proof_and_staged_for_release(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build, output = root / "build", root / "out"
            build.mkdir(); output.mkdir()
            elf32(build / "firmware.elf")
            version = "v1.18.0.1-test-deadbeef"
            stem = output / ("test_companion_radio_usb-" + version)
            manifest = self.qualify(build, stem)
            data = notice.notice_bytes()
            self.assertEqual(manifest["third_party_notices"], [notice.notice_record(stem, data)])
            # No artifact bytes are rewritten to carry notices.
            app = output / (stem.name + ".bin")
            merged = output / (stem.name + "-merged.bin")
            app.write_bytes(b"unchanged app"); merged.write_bytes(b"unchanged merged")
            sidecar = notice.collect_build_notice(build, stem)
            self.assertEqual(sidecar.read_bytes(), data)
            self.assertEqual(notice.collect_build_notice(build, stem), sidecar)
            report = dict(schema_version=1, passed=True, available_internal_bytes=80000,
                          required_heap_bytes=50000, largest_internal_region_bytes=80000,
                          required_contiguous_bytes=5120, target="build_env",
                          elf_sha256=hashlib.sha256((build / "firmware.elf").read_bytes()).hexdigest())
            (build / "firmware.memory.json").write_text(json.dumps(report))
            memory.package_report(build, stem)
            proof = memory.validate_package(stem)
            self.assertEqual(proof["files"][sidecar.name], hashlib.sha256(data).hexdigest())
            records = release.collect_artifacts(output, version)
            self.assertEqual(sum(path == sidecar for path in records[0]["files"]), 1)
            self.assertEqual(app.read_bytes(), b"unchanged app")
            self.assertEqual(merged.read_bytes(), b"unchanged merged")
            sidecar.write_bytes(data + b"tampered\n")
            with self.assertRaisesRegex(ValueError, "mismatched"):
                memory.validate_package(stem)
            sidecar.write_bytes(b"truncated")
            with self.assertRaisesRegex(ValueError, "mismatched"):
                memory.validate_package(stem)
            sidecar.unlink()
            with self.assertRaisesRegex(ValueError, "missing"):
                memory.validate_package(stem)
            with self.assertRaisesRegex(ValueError, "missing"):
                release.collect_artifacts(output, version)

    def test_metadata_cannot_omit_or_replace_linked_notice(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stem = root / "gui"
            elf32(root / "firmware.elf")
            manifest = self.qualify(root, stem)
            manifest_path = root / "gui.capabilities.json"
            manifest["third_party_notices"] = []
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "linked ELF"):
                notice.collect_build_notice(root, stem)
            manifest["third_party_notices"] = [notice.notice_record(stem, b"wrong")]
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "source notice"):
                notice.collect_build_notice(root, stem)
            self.assertFalse(notice.valid_notice(b"MIT License"))

    def test_both_source_notices_are_required_and_carried_in_full(self):
        data = notice.notice_bytes()
        self.assertTrue(notice.valid_notice(data))
        self.assertIn(notice.UI_NOTICE_PATH.read_bytes().rstrip(), data)
        self.assertIn(notice.FONT_NOTICE_PATH.read_bytes().rstrip(), data)
        with tempfile.TemporaryDirectory() as directory:
            absent = Path(directory) / "absent"
            incomplete = Path(directory) / "incomplete"
            incomplete.write_bytes(b"MIT License\n")
            for attribute in ("UI_NOTICE_PATH", "FONT_NOTICE_PATH"):
                with self.subTest(source=attribute):
                    with patch.object(notice, attribute, absent), self.assertRaises(OSError):
                        notice.notice_bytes()
                    with patch.object(notice, attribute, incomplete), self.assertRaisesRegex(ValueError, "incomplete"):
                        notice.notice_bytes()

    def test_malformed_manifest_declarations_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stem = root / "cardputer"
            elf32(root / "firmware.elf")
            manifest = self.qualify(root, stem)
            manifest_path = root / "cardputer.capabilities.json"
            record = notice.notice_record(stem, notice.notice_bytes())
            for records, message in (([record, record], "duplicate"),
                                     ([dict(record, file="elsewhere.txt")], "invalid"),
                                     ([dict(record, license="MIT")], "invalid"),
                                     ([dict(record, sha256="invalid")], "invalid")):
                with self.subTest(records=records):
                    manifest["third_party_notices"] = records
                    manifest_path.write_text(json.dumps(manifest))
                    with self.assertRaisesRegex(ValueError, message):
                        notice.collect_build_notice(root, stem)


if __name__ == "__main__":
    unittest.main()
