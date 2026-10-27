"""Exercise container conversion and lazy verification through the real planner."""
from __future__ import annotations

import asyncio
import builtins
from contextlib import redirect_stdout
from dataclasses import replace
import io
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import lora_ota as ota
import ota_wizard as wizard
import ota_wizard_bundle as bundles
import ota_wizard_packages as packages
import ota_wizard_session as session
from test_lora_ota import mota_blob
from test_ota_wizard_bundle import destination, image, signed_package


class PreparationRegressionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="meshcore-prepare-regression-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.live = destination()
        self.current = image(self.live.version, body=b"original application" * 16)
        self.new = image(body=b"new complete application" * 320)
        self.base = self.root / "running.bin"
        self.base.write_bytes(self.current.image)
        self.full = session.full_package(self.new, 1024)
        self.firmware = self.root / "new-full.mota"
        self.firmware.write_bytes(self.full.blob)

    def options(self, *extra, cache="cache"):
        return wizard.arguments(["--cache", str(self.root / cache), "--base", str(self.base), *extra])

    def test_automatic_full_container_can_become_a_smaller_delta_for_external_and_internal_nrf(self):
        for external in (True, False):
            with self.subTest(external_storage=external):
                live = replace(self.live, external_storage=external)
                options = self.options(cache="external" if external else "internal")
                commands = []

                def build(command, **_kwargs):
                    commands.append(command)
                    old = ota.parse_endf(Path(command[command.index("--base") + 1]).read_bytes())
                    new = ota.parse_endf(Path(command[command.index("--fw") + 1]).read_bytes())
                    self.assertEqual(old.image, self.current.image)
                    self.assertEqual(new.image, self.new.image)
                    output = Path(command[command.index("--out") + 1])
                    # The builder boundary is fake; the actual manifest, Merkle
                    # proof, base, target, codec and new-image validation run.
                    output.write_bytes(mota_blob(new.image, full=False,
                        version=new.fw_version, target=new.target_id, hw=new.hw_id,
                        base_hash=old.body_hash, codec=ota.MOTA_CODEC_IN_PLACE,
                        payload=b"small differential container"))
                    return SimpleNamespace(returncode=0)

                if not external:
                    with self.assertRaisesRegex(ota.OtaError, "Internal-flash"):
                        session.validate_package(self.full, live)
                with mock.patch.object(wizard, "find_builder", return_value="fake-builder") as builder, \
                        mock.patch.object(wizard, "install_builder", side_effect=AssertionError("unneeded setup")), \
                        mock.patch.object(packages.subprocess, "run", side_effect=build), \
                        redirect_stdout(io.StringIO()):
                    selected, package = asyncio.run(wizard.prepare_selected_firmware(
                        options, live, self.firmware, None))
                builder.assert_called_once()
                self.assertEqual(len(commands), 1)
                self.assertEqual(commands[0][commands[0].index("--patch-type") + 1], "in-place")
                self.assertEqual(selected.image, self.new.image)
                self.assertFalse(package.info.is_full)
                self.assertEqual(package.info.base_hash, self.live.base_hash)
                self.assertEqual(package.body_hash, self.new.body_hash)
                self.assertLess(len(package.blob), len(self.full.blob))
                session.validate_package(package, live)

    def test_explicit_full_keeps_the_original_unsigned_container_bytes(self):
        with mock.patch.object(wizard, "find_builder", side_effect=AssertionError("must stay full")), \
                mock.patch.object(packages.subprocess, "run", side_effect=AssertionError("must not build")), \
                redirect_stdout(io.StringIO()):
            selected, package = asyncio.run(wizard.prepare_selected_firmware(
                self.options("--full"), self.live, self.firmware, None))
        self.assertEqual(selected.image, self.new.image)
        self.assertEqual(package.blob, self.full.blob)
        self.assertEqual(package.info.block_size, 1024)

    def test_automatic_preserves_a_signed_full_sd_package_without_a_builder(self):
        signed = signed_package(self.new)
        self.firmware.write_bytes(signed.blob)
        with mock.patch.object(wizard, "find_builder", side_effect=AssertionError("signature must stay intact")), \
                mock.patch.object(wizard, "find_running_image", side_effect=AssertionError("no delta base needed")), \
                mock.patch.object(packages.subprocess, "run", side_effect=AssertionError("must not repackage")), \
                redirect_stdout(io.StringIO()):
            selected, package = asyncio.run(wizard.prepare_selected_firmware(
                self.options(), replace(self.live, sd_storage=True), self.firmware, None))
        self.assertEqual(selected.image, self.new.image)
        self.assertEqual(package.blob, signed.blob)
        self.assertEqual(package.signer, signed.signer)
        session.validate_package(package, replace(self.live, sd_storage=True))

    def test_missing_verifier_in_real_signed_mota_and_bundle_paths_is_installed_then_retried(self):
        # Build/sign fixtures before simulating a fresh Python installation.
        signed = signed_package(self.new)
        signed_file = self.root / "signed-full.mota"
        signed_file.write_bytes(signed.blob)
        prepared_file = self.root / "signed-prepared.ota.zip"
        bundles.save_bundle(prepared_file, self.new, signed)
        original_import = builtins.__import__
        live = replace(self.live, sd_storage=True)

        for firmware in (signed_file, prepared_file):
            with self.subTest(input=firmware.name):
                blocked = [True]
                missing_imports = []

                def import_without_crypto(name, *args, **kwargs):
                    if blocked[0] and (name == "cryptography" or name.startswith("cryptography.")):
                        missing_imports.append(name)
                        raise ModuleNotFoundError("No module named cryptography", name=name)
                    return original_import(name, *args, **kwargs)

                def install_verifier():
                    self.assertTrue(missing_imports, "the real signature verifier must have failed first")
                    blocked[0] = False

                options = self.options(cache=firmware.stem)
                with mock.patch.object(wizard, "choose_request", return_value=live), \
                        mock.patch.object(wizard, "GitHubCatalog", return_value=object()), \
                        mock.patch.object(wizard, "choose_firmware", new_callable=mock.AsyncMock,
                                          return_value=(firmware, "local")) as choose, \
                        mock.patch.object(ota, "cryptography_repair_root", return_value=self.root / "fresh-python"), \
                        mock.patch.object(builtins, "__import__", side_effect=import_without_crypto), \
                        mock.patch.object(wizard, "ensure_signature_support", side_effect=install_verifier) as setup, \
                        mock.patch.object(wizard, "ensure_dependencies", side_effect=AssertionError("no USB setup")), \
                        mock.patch.object(wizard, "choose_port", side_effect=AssertionError("no radio access")), \
                        mock.patch.object(wizard, "find_builder", side_effect=AssertionError("no native builder")), \
                        mock.patch.object(wizard, "ask", side_effect=lambda _prompt, default=None: default), \
                        redirect_stdout(io.StringIO()):
                    self.assertEqual(asyncio.run(wizard.prepare_interactively(options)), 0)
                setup.assert_called_once_with()
                choose.assert_awaited_once()
                outputs = list((options.cache / "bundles").glob("*.ota.zip"))
                self.assertEqual(len(outputs), 1)
                restored_image, restored_package = bundles.load_bundle(outputs[0], live)
                self.assertEqual(restored_image.image, self.new.image)
                self.assertEqual(restored_package.blob, signed.blob)
                self.assertEqual(restored_package.signer, signed.signer)


if __name__ == "__main__":
    unittest.main()
