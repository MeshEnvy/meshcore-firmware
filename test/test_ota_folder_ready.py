"""Exercise real folder framing, catalog readiness and production burst code."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

from test_replay_reset_integration import extract_braced

ROOT = Path(__file__).resolve().parents[1]


class OtaFolderReadyTest(unittest.TestCase):
    def run_fixture(self, missing_ready_trigger=False):
        production = (ROOT / "src/Mesh.cpp").read_text(encoding="ascii")
        defines = []
        for name in ("OTA_ANNOUNCE_BURST", "OTA_ANNOUNCE_BURST_MS", "OTA_ANNOUNCE_DISABLED_POLL_MS"):
            match = re.search(r"^#define " + name + r"\s+(\d+(?:UL)?)\b", production, re.M)
            self.assertIsNotNone(match)
            defines.append("#define " + name + " " + match[1])
        source = (ROOT / "test/fixtures/ota_folder_ready.cpp").read_text(encoding="ascii")
        # This folder-readiness fixture has no competing raw USB host session.
        # Keep the real Arduino declaration visible to MotaSourceSerial.cpp.
        source += "\nnamespace mesh { bool isUsbBinaryStreamOwned() { return false; } }\n"
        source = source.replace("// @PRODUCTION_DEFINES@", "\n".join(defines))
        ready = extract_braced(production, "if (ota::ota_ctx().manager.takeSourceReadyAnnouncement())")
        source = source.replace("// @PRODUCTION_READY@", "" if missing_ready_trigger else ready)
        source = source.replace("// @PRODUCTION_ANNOUNCE@", extract_braced(
            production, "if (_ota_announce_timer.ready(_ms->getMillis()))"))
        with tempfile.TemporaryDirectory(prefix="ota-folder-ready-") as directory:
            work = Path(directory)
            cpp, executable = work / "ready.cpp", work / "ready"
            cpp.write_text(source, encoding="ascii")
            sanitizers = [] if os.name == "nt" else ["-fsanitize=address,undefined", "-fno-pie", "-no-pie"]
            compiler = os.environ.get("CXX") or shutil.which("c++") or "g++"
            variants = (("NRF52_PLATFORM=1",),) if missing_ready_trigger else (
                ("ESP32_PLATFORM=1", "COMPANION_RADIO_FULL=1"), ("NRF52_PLATFORM=1",))
            for definitions in variants:
                with self.subTest(definitions=definitions):
                    sources = ("OtaManager.cpp", "OtaProtocol.cpp", "MotaContainer.cpp", "MerkleTree.cpp", "MotaSourceSerial.cpp")
                    result = subprocess.run([compiler, "-std=c++17", "-O1", *sanitizers,
                        "-DARDUINO=10819", "-DENABLE_OTA=1", "-DOTA_SEEDER_ONLY=1",
                        *["-D" + value for value in definitions],
                        "-I", str(ROOT / "src"), "-I", str(ROOT / "test/mocks"), str(cpp),
                        *[str(ROOT / "src/helpers/ota" / name) for name in sources],
                        str(ROOT / "src/Utils.cpp"), "-o", str(executable)], capture_output=True, text=True, timeout=90)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
                    if missing_ready_trigger:
                        self.assertNotEqual(result.returncode, 0, "missing readiness trigger escaped the oracle")
                        self.assertIn("ready catalog must discover without a control channel", result.stderr)
                    else:
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn("PASS: single-data-session folder readiness", result.stdout)

    def test_single_host_data_session_announces_only_ready_changed_catalogs(self):
        self.run_fixture()

    def test_missing_production_ready_trigger_is_detected(self):
        self.run_fixture(missing_ready_trigger=True)


if __name__ == "__main__":
    unittest.main()
