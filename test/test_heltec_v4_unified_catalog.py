"""Keep unified V4 releases distinct from historical display and OTA targets."""
from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import consolidated_profiles as consolidated

SPEC = next(spec for spec in consolidated.SPECS if spec["key"] == "unifiedV4Display")
SOURCE = "a" * 40
FLAGS = ["-DHELTEC_V4_DISPLAY_UNIFIED=1", "-DDISPLAY_CLASS=HeltecV4Display"]
PROVIDER_SPEC = importlib.util.spec_from_file_location(
    "unified_v4_provider", ROOT / "mesh-america/update-provider-release.py")
provider = importlib.util.module_from_spec(PROVIDER_SPEC)
PROVIDER_SPEC.loader.exec_module(provider)


def environment():
    return dict(board="heltec_v4", build_flags=list(FLAGS))


def manifest(target):
    return dict(target=target, artifact_target=target, verified=True,
                platform="ESP32_PLATFORM", build_profile="full", source_commit=SOURCE,
                capabilities=[SPEC["capability"]], verification=[dict(
                    capability=SPEC["capability"], present=True,
                    source="packaged application", evidence=SPEC["marker"])])


class UnifiedV4CatalogTests(unittest.TestCase):
    def test_all_display_variants_require_exact_new_driver_and_source_proof(self):
        for target in ("heltec_v4_repeater", "heltec_v4_room_server", "heltec_v4_sensor",
                       "heltec_v4_2_v4_3_companion_radio_full_femon",
                       "heltec_v4_tft_repeater", "heltec_v4_expansionkit_repeater",
                       "heltec_v4_expansionkit_tft_companion_radio_full_femon"):
            row = manifest(target)
            with self.subTest(target=target):
                self.assertEqual(consolidated.runtime_metadata(row, environment(), SOURCE),
                                 dict(unifiedV4Display=True, unifiedV4DisplaySource=SOURCE))
                self.assertEqual(consolidated.runtime_metadata(
                    {**row, "capabilities": []}, environment(), SOURCE), {})
                for changed in ({"verification": []}, {"platform": "NRF52_PLATFORM"},
                                {"source_commit": "b" * 40}, {"verified": False}):
                    with self.assertRaises(ValueError):
                        consolidated.runtime_metadata({**row, **changed}, environment(), SOURCE)
                with self.assertRaises(ValueError):
                    consolidated.runtime_metadata(row, {**environment(), "board": "heltec_v4_r8"}, SOURCE)
                for override in ("-UHELTEC_V4_DISPLAY_UNIFIED", "-DDISPLAY_CLASS=SSD1306Display"):
                    with self.assertRaises(ValueError):
                        consolidated.runtime_metadata(row, {**environment(), "build_flags": FLAGS + [override]}, SOURCE)

    def test_roles_and_r8_hardware_cannot_be_interchanged(self):
        for prefix in ("heltec_v4_tft", "heltec_v4_3_tft", "heltec_v4_expansionkit"):
            for role in ("repeater", "room_server", "sensor"):
                old, new = prefix + "_" + role, "heltec_v4_" + role
                with self.subTest(target=old):
                    self.assertIn((new, "unifiedV4Display"), consolidated.replacements(old))
                    envs = {"env:" + old: environment(), "env:" + new: environment()}
                    self.assertEqual(consolidated.configuration_replacement(old, envs), new)
                    envs["env:" + old]["board"] = "heltec_v4_r8"
                    self.assertIsNone(consolidated.configuration_replacement(old, envs))
        for target in ("heltec_v4_r8_tft_repeater", "heltec_v4_r8_sensor",
                       "heltec_v4_partition_expander", "heltec_v4_tft_repeater_custom"):
            with self.subTest(target=target):
                self.assertEqual(consolidated.unified_v4_replacements(target), [])
                with self.assertRaises(ValueError):
                    consolidated.runtime_metadata(manifest(target), environment(), SOURCE)

    def test_default_consolidation_does_not_change_deployed_target_names(self):
        old, new = "heltec_v4_tft_repeater", "heltec_v4_repeater"
        legacy = dict(manifest={**manifest(old), "capabilities": [], "verification": []})
        canonical = dict(manifest=manifest(new))
        inputs = [legacy, canonical]
        before = deepcopy(inputs)
        self.assertEqual(consolidated.omit_redundant_records(inputs), [canonical])
        self.assertEqual(inputs, before)
        canonical["manifest"]["verification"] = []
        self.assertEqual(consolidated.omit_redundant_records(inputs), inputs)

    def test_provider_requires_the_downloadable_application_not_only_sidecar_claims(self):
        old, new = "heltec_v4_tft_repeater", "heltec_v4_repeater-full-usb-wifi-ota"
        with tempfile.TemporaryDirectory() as folder:
            app = Path(folder) / (new + "-v1.18.0.2-aaaaaaaa.bin")
            proof = app.with_suffix(".capabilities.json")
            row = {**manifest("heltec_v4_repeater"), "artifact_target": new}
            proof.write_text(json.dumps(row))
            app.write_bytes(b"old application without unified display")
            files = {new: [app]}
            self.assertIsNone(provider.unified_v4_release_identity(old, files))
            app.write_bytes(b"prefix\0" + SPEC["marker"].encode("ascii") + b"\0suffix")
            self.assertEqual(provider.unified_v4_release_identity(old, files), new)
            self.assertEqual(provider.resolve_release_identity(old, files), (new, True))
            for changed in ({"verified": False}, {"verification": []},
                            {"artifact_target": "heltec_v4_r8_repeater-full-usb-wifi-ota"}):
                proof.write_text(json.dumps({**row, **changed}))
                self.assertIsNone(provider.unified_v4_release_identity(old, files))
            proof.unlink()
            self.assertIsNone(provider.unified_v4_release_identity(old, files))

    def test_historical_provider_artifacts_and_r8_keep_their_exact_identity(self):
        for old in ("heltec_v4_tft_repeater", "heltec_v4_r8_tft_repeater"):
            files = {old: [Path(old + "-v1.18.0.1-aaaaaaaa.bin")]}
            self.assertEqual(provider.resolve_release_identity(old, files), (old, False))


if __name__ == "__main__":
    unittest.main()
