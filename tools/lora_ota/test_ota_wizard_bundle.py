"""Portable request and bundle checks without serial devices or networking."""
from dataclasses import replace
import io
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import warnings
import zipfile

import lora_ota as ota
import ota_wizard_bundle as bundles
from ota_wizard_session import Destination, Package, full_package

KEY = '11' * 32
HW = 'ikoka_stick_nrf_33dbm'
TARGET = 0xE5331F02


def image(version=0x01120002, target=TARGET, hw=HW, body=b'new firmware' * 32):
    lib = ota.bootloader_library()
    return ota.parse_endf(body + lib.build_endf(body, lib.FwIdent(version, target, hw)))


def destination():
    old = image(0x01120001, body=b'original application' * 16)
    return Destination(KEY, TARGET, HW, old.body_hash, old.fw_version,
                       'v1.18.0.1', 'nrf52', True, 2048, 5)


def signed_package(new_image=None):
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    except ImportError as exc:
        raise unittest.SkipTest('cryptography is required for signature fixtures') from exc
    new_image = image() if new_image is None else new_image
    lib = ota.bootloader_library()
    manifest = lib.build_manifest(target_id=new_image.target_id, fw_version=new_image.fw_version,
        image_size=len(new_image.image), payload=new_image.image, block_size=2048,
        image_hash=hashlib.sha256(new_image.image).digest(), codec_id=lib.CODEC_FULL,
        is_full=True, hw_id=new_image.hw_id,
        sign_priv=Ed25519PrivateKey.from_private_bytes(bytes(range(32))))
    return Package(lib.build_container(manifest, new_image.image), new_image.body_hash,
                   manifest.signer_pubkey.hex().upper())


class WizardBundleTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='meshcore-wizard-bundle-')
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)
        self.destination = destination()
        self.image = image()
        self.package = full_package(self.image, 2048)

    def bundle(self):
        file = self.path/'update.ota.zip'
        bundles.save_bundle(file, self.image, self.package)
        return file

    def rewrite(self, path, transform):
        with zipfile.ZipFile(path) as archive:
            members = {name: archive.read(name) for name in archive.namelist()}
        transform(members)
        with zipfile.ZipFile(path, 'w') as archive:
            for name, data in members.items():
                archive.writestr(name, data)

    def test_request_round_trip_preserves_only_public_destination_data(self):
        file = self.path/'device.json'
        bundles.save_request(file, self.destination)
        self.assertEqual(bundles.load_request(file), self.destination)
        raw = json.loads(file.read_text())
        self.assertEqual(raw['destination']['public_key'], KEY)
        self.assertEqual(raw['destination']['base_hash'], self.destination.base_hash.hex())
        self.assertNotIn('password', file.read_text().lower())
        self.assertNotIn('private_key', file.read_text().lower())

    def test_full_bundle_round_trip_validates_firmware_and_package(self):
        loaded_image, loaded_package = bundles.load_bundle(self.bundle(), self.destination)
        self.assertEqual(loaded_image.image, self.image.image)
        self.assertEqual(loaded_package.blob, self.package.blob)
        self.assertEqual(loaded_package.body_hash, self.image.body_hash)
        with zipfile.ZipFile(self.path/'update.ota.zip') as archive:
            self.assertEqual(set(archive.namelist()),
                             {'ota-bundle.json', 'application.mota', 'firmware.bin'})

    def test_request_round_trip_binds_exact_bootloader_identity(self):
        lib = ota.bootloader_library()
        board, name = 0x28860044, 'XIAO_DFU'
        boot = ota.BootloaderStatus(board, lib.bootloader_target_id(board, name), name,
                                   lib.bootloader_hw_id(board, name).rstrip(b'\0').decode(),
                                   0x6F9BE280, 3, 14)
        expected = replace(self.destination, bootloader=boot, boot_version=0x0204000B)
        file = self.path/'request.json'
        bundles.save_request(file, expected)
        self.assertEqual(bundles.load_request(file), expected)
        value = json.loads(file.read_text())
        value['destination']['bootloader']['target_id'] ^= 1
        file.write_text(json.dumps(value))
        with self.assertRaisesRegex(ota.OtaError, 'identity'):
            bundles.load_request(file)

    def test_normal_firmware_zip_is_not_mistaken_for_prepared_bundle(self):
        file = self.path/'application.zip'
        with zipfile.ZipFile(file, 'w') as archive:
            archive.writestr('manifest.json', '{}')
        self.assertIsNone(bundles.load_bundle(file, self.destination))

    def test_changed_package_checksum_is_rejected(self):
        file = self.bundle()
        self.rewrite(file, lambda members: members.__setitem__(
            'application.mota', members['application.mota'][:-1] + b'X'))
        with self.assertRaisesRegex(ota.OtaError, 'checksum'):
            bundles.load_bundle(file, self.destination)

    def test_changed_complete_firmware_identity_is_rejected(self):
        file = self.bundle()
        self.rewrite(file, lambda members: members.__setitem__(
            'firmware.bin', image(body=b'different firmware' * 32).image))
        with self.assertRaisesRegex(ota.OtaError, 'does not match'):
            bundles.load_bundle(file, self.destination)

    def test_recomputed_outer_checksum_does_not_hide_corrupted_container(self):
        file = self.bundle()
        def corrupt(members):
            blob = bytearray(members['application.mota'])
            blob[-1] ^= 1
            members['application.mota'] = bytes(blob)
            metadata = json.loads(members['ota-bundle.json'])
            metadata['package_sha256'] = hashlib.sha256(blob).hexdigest()
            members['ota-bundle.json'] = json.dumps(metadata)
        self.rewrite(file, corrupt)
        with self.assertRaises(ota.OtaError):
            bundles.load_bundle(file, self.destination)

    def test_malformed_bundle_metadata_is_reported_as_a_user_error(self):
        for metadata in (b'[]', b'null', b'"text"', b'{broken'):
            file = self.bundle()
            self.rewrite(file, lambda members: members.__setitem__('ota-bundle.json', metadata))
            with self.subTest(metadata=metadata), self.assertRaises(ota.OtaError):
                bundles.load_bundle(file, self.destination)

    def test_wrong_live_board_and_non_upgrade_are_rejected(self):
        file = self.bundle()
        for candidate in (replace(self.destination, target_id=TARGET ^ 1),
                          replace(self.destination, hw_id='another_board'),
                          replace(self.destination, version=self.image.fw_version)):
            with self.subTest(candidate=candidate), self.assertRaises(ota.OtaError):
                bundles.load_bundle(file, candidate)

    def test_unexpected_and_duplicate_members_are_rejected(self):
        for member in ('extra.json', 'firmware.bin'):
            file = self.bundle()
            with warnings.catch_warnings(), zipfile.ZipFile(file, 'a') as archive:
                warnings.simplefilter('ignore', UserWarning)
                archive.writestr(member, b'not another input')
            with self.subTest(member=member), self.assertRaisesRegex(ota.OtaError, 'members'):
                bundles.load_bundle(file, self.destination)

    def test_save_refuses_a_package_for_a_different_complete_image(self):
        with self.assertRaises(ota.OtaError):
            bundles.save_bundle(self.path/'wrong.zip', image(body=b'other'), self.package)

    def test_invalid_request_fields_fail_as_user_errors(self):
        file = self.path/'request.json'
        for field, value in (('public_key', 'short'), ('target_id', -1),
                             ('platform', 'unknown'), ('base_hash', 'abcd'),
                             ('max_block_size', 4096)):
            bundles.save_request(file, self.destination)
            contents = json.loads(file.read_text())
            contents['destination'][field] = value
            file.write_text(json.dumps(contents))
            with self.subTest(field=field), self.assertRaises(ota.OtaError):
                bundles.load_request(file)

    def test_signed_full_input_preserves_signature_and_is_usable_by_sd_installer(self):
        original = signed_package(self.image)
        path = self.path/'signed-full.mota'
        path.write_bytes(original.blob)
        package = bundles.load_ready_application(path, self.image,
                                                replace(self.destination, sd_storage=True))
        self.assertEqual(package, original)
        self.assertEqual(package.blob, path.read_bytes())
        self.assertEqual(package.body_hash, self.image.body_hash)

    def test_unsigned_full_input_remains_rejected_for_sd(self):
        path = self.path/'unsigned-full.mota'
        path.write_bytes(self.package.blob)
        self.assertEqual(bundles.load_ready_application(path, self.image, self.destination), self.package)
        with self.assertRaisesRegex(ota.OtaError, 'trusted signed'):
            bundles.load_ready_application(path, self.image, replace(self.destination, sd_storage=True))

    def test_supplied_mota_is_bound_to_exact_image_target_hardware_version_and_codec(self):
        original = signed_package(self.image)
        path = self.path/'signed-full.mota'
        path.write_bytes(original.blob)
        with self.assertRaisesRegex(ota.OtaError, 'selected complete firmware'):
            bundles.load_ready_application(path, image(body=b'another complete image'), self.destination)
        for live in (replace(self.destination, target_id=TARGET ^ 1),
                     replace(self.destination, hw_id='different_board'),
                     replace(self.destination, version=self.image.fw_version),
                     replace(self.destination, codecs=4)):
            with self.subTest(live=live), self.assertRaises(ota.OtaError):
                bundles.load_ready_application(path, self.image, live)

    def test_corrupted_signature_and_non_full_input_are_rejected(self):
        original = signed_package(self.image)
        path = self.path/'signed-full.mota'
        broken = bytearray(original.blob)
        broken[8 + 129] ^= 1  # Fixed-layout manifest signature; payload remains intact.
        path.write_bytes(broken)
        with self.assertRaisesRegex(ota.OtaError, 'verification failed'):
            bundles.load_ready_application(path, self.image, self.destination)
        lib = ota.bootloader_library()
        manifest = lib.build_manifest(target_id=TARGET, fw_version=self.image.fw_version,
            image_size=len(self.image.image), payload=b'patch', block_size=2048,
            image_hash=hashlib.sha256(self.image.image).digest(), codec_id=lib.CODEC_DETOOLS_INPLACE,
            is_full=False, hw_id=HW, base_hash=self.destination.base_hash)
        path.write_bytes(lib.build_container(manifest, b'patch'))
        with self.assertRaisesRegex(ota.OtaError, 'full application'):
            bundles.load_ready_application(path, self.image, self.destination)

    def test_raw_input_does_not_require_a_ready_package(self):
        self.assertIsNone(bundles.load_ready_application(self.path/'not-opened.bin',
                                                       self.image, self.destination))


if __name__ == '__main__':
    unittest.main()
