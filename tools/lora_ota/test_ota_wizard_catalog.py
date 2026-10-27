"""Offline release selection, download, and exact-base checks for the OTA UI."""
from __future__ import annotations

import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
from urllib.parse import quote
import zipfile

import lora_ota as ota
import ota_wizard_catalog as catalog
from test_lora_ota import firmware, mota_blob


ENV = "ikoka_stick_nrf_33dbm_repeater"
HW = "ikoka_stick_nrf_33dbm"
TARGET = int.from_bytes(hashlib.sha256(ENV.encode()).digest()[:4], "little")
FAMILY = "v1.18.0.1-halo-keymind-cascade-dev-03214a16"
OLD_FAMILY = "v1.17.1.9-halo-keymind-cascade-dev-43f474a8"
VERSION = 0x01120001


def asset_row(name=None, tag=FAMILY, data=b"firmware", digest=True):
    name = name or f"{ENV}-full-ota-{FAMILY}.zip"
    return {"name": name, "size": len(data),
            "browser_download_url": f"https://github.com/mikecarper/MeshCore/releases/download/{quote(tag, safe='')}/{quote(name, safe='')}",
            "digest": "sha256:" + hashlib.sha256(data).hexdigest() if digest else None}


def release(tag, assets=(), **kwargs):
    return {"tag_name": tag, "draft": False, "prerelease": False,
            "assets": list(assets), "published_at": "2026-10-20T00:00:00Z", **kwargs}


class Response(io.BytesIO):
    def __init__(self, data, url):
        super().__init__(data)
        self.url = url

    def geturl(self):
        return self.url


class CatalogTest(unittest.TestCase):
    def test_current_release_family_includes_chunk_ten_but_not_board_previews(self):
        latest = asset_row()
        old = asset_row(f"{ENV}-full-ota-{OLD_FAMILY}.zip", OLD_FAMILY)
        reduced = asset_row(f"{ENV}-reduced-ota-{FAMILY}.zip", f"lora-ota-10-{FAMILY}")
        rows = [release("w12-v1.19.0.1", [asset_row()]),
                release("v1.19.0.1-preview.2", [asset_row()]),
                release(OLD_FAMILY, [old], published_at="2026-10-25T00:00:00Z"),
                release(FAMILY), release(f"lora-ota-2-{FAMILY}", [
                    {**latest, "browser_download_url": latest["browser_download_url"].replace(
                        f"/download/{FAMILY}/", f"/download/lora-ota-2-{FAMILY}/")}]),
                release(f"lora-ota-10-{FAMILY}", [reduced])]
        calls = []
        def opener(request, timeout):
            calls.append(request.full_url)
            return Response(json.dumps(rows).encode(), request.full_url)
        source = catalog.GitHubCatalog(opener=opener)
        current = source.fetch_assets()
        self.assertEqual(len(current), 2)
        self.assertEqual(source.choose_asset(target_id=TARGET).profile, "full")
        self.assertEqual(source.choose_asset(ENV, profile="reduced").profile, "reduced")
        bases = source.base_candidates(target_id=TARGET, fw_version=0x01110109)
        self.assertEqual([row.name for row in bases], [old["name"]])
        self.assertEqual(len(calls), 1)

    def test_exact_target_role_profile_and_embedded_variants(self):
        full = catalog.parse_release_asset(asset_row(), FAMILY)
        embedded = catalog.parse_release_asset(asset_row(
            f"{ENV}-with-bootloader-2.4.12-full-ota-{FAMILY}.zip"), FAMILY)
        room = catalog.parse_release_asset(asset_row(
            f"ikoka_stick_nrf_33dbm_room_server_lora_ota-full-ota-{FAMILY}.zip"), FAMILY)
        companion = catalog.parse_release_asset(asset_row(
            f"ikoka_stick_nrf_33dbm_companion_radio_full-{FAMILY}.zip"), FAMILY)
        self.assertEqual(full.target_id, TARGET)
        self.assertTrue(embedded.embedded_bootloader)
        self.assertIsNotNone(companion)
        source = catalog.GitHubCatalog()
        source._assets = [embedded, room, full]
        self.assertEqual(source.candidates(target_id=TARGET), [full])
        self.assertEqual(source.candidates(ENV, profile="reduced"), [])
        self.assertEqual(source.candidates(ENV, include_embedded=True), [full, embedded])
        with self.assertRaises(ota.OtaError):
            source.candidates()
        with self.assertRaises(ota.OtaError):
            source.choose_asset("another_repeater")

    def test_release_menu_groups_shards_and_offers_exact_board_previews(self):
        shard = f"lora-ota-10-{FAMILY}"
        preview = "ikoka-v1.18.0.2-alpha2"
        shard_asset = asset_row(tag=shard)
        preview_asset = asset_row(f"{ENV}-full-ota-v1.18.0.2-alpha2.zip", preview)
        old_asset = asset_row(f"{ENV}-full-ota-{OLD_FAMILY}.zip", OLD_FAMILY)
        rows = [release(preview, [preview_asset], prerelease=True),
                release(FAMILY, [asset_row(f"{ENV}-full-ota-{FAMILY}.bin")]),
                release(shard, [shard_asset]),
                release(f"full-profiles-{FAMILY}", [asset_row(
                    f"{ENV}-reduced-ota-{FAMILY}.zip", f"full-profiles-{FAMILY}")]),
                release(OLD_FAMILY, [old_asset]),
                release("v9.0.0-draft", [asset_row()], draft=True),
                release("another-v8.0.0", [asset_row(f"another_repeater-full-ota-v8.0.0.zip",
                                                     "another-v8.0.0")])]
        source = catalog.GitHubCatalog(opener=lambda req, timeout: Response(json.dumps(rows).encode(), req.full_url))
        choices = source.release_choices(target_id=TARGET)
        self.assertEqual([choice.release_tag for choice in choices], [FAMILY, OLD_FAMILY, preview])
        self.assertEqual(choices[0].asset.name, shard_asset["name"])
        self.assertEqual(choices[0].asset.release_tag, shard)
        self.assertFalse(choices[0].prerelease)
        self.assertTrue(choices[-1].prerelease)
        self.assertIn("prerelease", choices[-1].label)
        self.assertEqual(source.choose_asset(target_id=TARGET), choices[0].asset)
        self.assertEqual([choice.release_tag for choice in source.release_choices(
            target_id=TARGET, include_prerelease=False)], [FAMILY, OLD_FAMILY])
        self.assertFalse(source.has_more_releases)

    def test_menu_prefers_full_and_does_not_offer_embedded_or_wrong_role(self):
        rows = [release(FAMILY, [
            asset_row(f"{ENV}-with-bootloader-2.4.12-full-ota-{FAMILY}.zip"),
            asset_row(f"ikoka_stick_nrf_33dbm_room_server_lora_ota-full-ota-{FAMILY}.zip"),
            asset_row(f"{ENV}-reduced-ota-{FAMILY}.zip"),
            asset_row(f"{ENV}-ota-{FAMILY}.bin"), asset_row()])]
        source = catalog.GitHubCatalog(opener=lambda req, timeout: Response(json.dumps(rows).encode(), req.full_url))
        choices = source.release_choices(ENV)
        self.assertEqual(len(choices), 1)
        self.assertEqual(choices[0].asset.profile, "full")
        self.assertEqual(source.release_choices(ENV, profile="reduced")[0].asset.profile, "reduced")
        self.assertEqual(source.release_choices("no_matching_repeater"), [])

    def test_menu_loads_older_pages_once_and_adds_exact_base_candidates(self):
        def page_rows(number):
            if number == 1:
                head = release(FAMILY, [asset_row()])
            elif number == 2:
                head = release(OLD_FAMILY, [asset_row(f"{ENV}-full-ota-{OLD_FAMILY}.zip", OLD_FAMILY)])
            else:
                return []
            return [head] + [release(f"v0.0.0-empty-{number}-{index}") for index in range(9)]
        calls = []
        def opener(request, timeout):
            number = int(request.full_url.rsplit("=", 1)[1])
            calls.append(number)
            return Response(json.dumps(page_rows(number)).encode(), request.full_url)
        source = catalog.GitHubCatalog(opener=opener)
        self.assertEqual(len(source.release_choices(target_id=TARGET, max_pages=1)), 1)
        self.assertTrue(source.has_more_releases)
        self.assertEqual(len(source.release_choices(target_id=TARGET, max_pages=3)), 2)
        self.assertEqual(len(source.release_choices(target_id=TARGET, max_pages=6)), 2)
        self.assertEqual(calls, [1, 2, 3])
        self.assertFalse(source.has_more_releases)
        self.assertEqual(source.base_candidates(target_id=TARGET, fw_version=0x01110109)[0].release_tag, OLD_FAMILY)
        self.assertEqual(source.choose_asset(target_id=TARGET).release_tag, FAMILY)
        self.assertEqual(calls, [1, 2, 3])

    def test_preview_only_menu_still_supplies_an_exact_base_candidate(self):
        tag = "ikoka-v1.18.0.2-alpha1"
        rows = [release(tag, [asset_row(f"{ENV}-full-ota-v1.18.0.2-alpha1.zip", tag)], prerelease=True)]
        source = catalog.GitHubCatalog(opener=lambda req, timeout: Response(json.dumps(rows).encode(), req.full_url))
        choices = source.release_choices(target_id=TARGET)
        self.assertEqual(len(choices), 1)
        self.assertTrue(choices[0].prerelease)
        self.assertEqual(source.base_candidates(target_id=TARGET, fw_version=0x01120002), [choices[0].asset])
        with self.assertRaisesRegex(ota.OtaError, "main firmware release"):
            source.choose_asset(target_id=TARGET)

    def test_menu_page_errors_are_reported_without_caching_partial_results(self):
        calls = []
        def opener(request, timeout):
            page = int(request.full_url.rsplit("=", 1)[1])
            calls.append(page)
            if page == 1:
                rows = [release(FAMILY, [asset_row()])] + [release(f"v0.0.0-empty-{index}") for index in range(9)]
                return Response(json.dumps(rows).encode(), request.full_url)
            return Response(b"not a release catalog" if calls.count(2) == 1 else b"[]", request.full_url)
        source = catalog.GitHubCatalog(opener=opener)
        self.assertEqual(len(source.release_choices(target_id=TARGET, max_pages=1)), 1)
        with self.assertRaisesRegex(ota.OtaError, "invalid release catalog"):
            source.release_choices(target_id=TARGET, max_pages=3)
        self.assertEqual(len(source.release_choices(target_id=TARGET, max_pages=3)), 1)
        self.assertEqual(calls, [1, 2, 2])

    def test_menu_validates_target_page_limits_and_terminal_labels(self):
        source = catalog.GitHubCatalog(opener=lambda req, timeout: Response(b"[]", req.full_url))
        with self.assertRaisesRegex(ota.OtaError, "exact firmware target"):
            source.release_choices()
        for limit in (0, 11, True, "3"):
            with self.subTest(limit=limit), self.assertRaisesRegex(ota.OtaError, "page limit"):
                source.release_choices(target_id=TARGET, max_pages=limit)
        item = catalog.ReleaseChoice("v1.18.0.2\x1b[1m\n", catalog.parse_release_asset(asset_row(), FAMILY), True, "")
        self.assertNotIn("\x1b", item.label)
        self.assertNotIn("\n", item.label)

    def test_untrusted_asset_metadata_is_not_selectable(self):
        for mutation in ({"browser_download_url": "http://github.com/file.zip"},
                         {"browser_download_url": "https://example.com/file.zip"},
                         {"size": True}, {"size": 0},
                         {"size": catalog.MAX_DOWNLOAD_BYTES + 1},
                         {"digest": "sha1:1234"},
                         {"name": "../escape.zip"}):
            with self.subTest(mutation=mutation):
                self.assertIsNone(catalog.parse_release_asset({**asset_row(), **mutation}, FAMILY))
        self.assertIsNone(catalog.parse_release_asset(asset_row(
            f"{ENV}-full-ota-{FAMILY}-merged.bin"), FAMILY))

    def test_invalid_catalog_and_redirect_are_reported_before_selection(self):
        for data in (b"not JSON", b"{}", b"[null]"):
            with self.subTest(data=data), self.assertRaises(ota.OtaError):
                catalog.GitHubCatalog(opener=lambda req, timeout: Response(data, req.full_url)).fetch_assets()
        with self.assertRaisesRegex(ota.OtaError, "unexpected"):
            catalog.GitHubCatalog(opener=lambda req, timeout: Response(b"[]", "http://127.0.0.1/")).fetch_assets()
        with self.assertRaisesRegex(ota.OtaError, "unexpected"):
            handler = catalog._GithubRedirects(api=False)
            handler.redirect_request(None, None, 302, "redirect", {}, "http://localhost/file")

    def test_bounded_atomic_download_cache_and_sha256(self):
        data = b"published application" * 500
        row = catalog.parse_release_asset(asset_row(data=data), FAMILY)
        calls = []
        def opener(request, timeout):
            calls.append(request.full_url)
            return Response(data, "https://release-assets.githubusercontent.com/example?token=public")
        with tempfile.TemporaryDirectory() as folder:
            path = catalog.download_asset(row, Path(folder), opener=opener)
            self.assertEqual(path.read_bytes(), data)
            self.assertEqual(catalog.download_asset(row, Path(folder), opener=opener), path)
            self.assertEqual(len(calls), 1)
            path.write_bytes(b"x" * len(data))
            self.assertEqual(catalog.download_asset(row, Path(folder), opener=opener).read_bytes(), data)
            self.assertEqual(len(calls), 2)
            self.assertFalse(list(Path(folder).glob("*.tmp")))

    def test_incomplete_oversize_digest_or_host_failure_leaves_no_download(self):
        row = catalog.parse_release_asset(asset_row(data=b"abcd"), FAMILY)
        for data, url in ((b"abc", row.url), (b"abcde", row.url),
                          (b"xxxx", row.url), (b"abcd", "https://github.com.evil.test/file")):
            with self.subTest(data=data, url=url), tempfile.TemporaryDirectory() as folder:
                with self.assertRaises(ota.OtaError):
                    catalog.download_asset(row, Path(folder), opener=lambda req, timeout: Response(data, url))
                self.assertEqual(list(Path(folder).iterdir()), [])

    def test_older_github_assets_without_digest_have_checked_local_cache(self):
        row = catalog.parse_release_asset(asset_row(digest=False), FAMILY)
        with tempfile.TemporaryDirectory() as folder:
            opener = mock.Mock(side_effect=lambda req, timeout: Response(b"firmware", req.full_url))
            path = catalog.download_asset(row, Path(folder), opener=opener)
            catalog.download_asset(row, Path(folder), opener=opener)
            self.assertEqual(opener.call_count, 1)
            path.with_name(path.name + ".json").write_text("[]")
            catalog.download_asset(row, Path(folder), opener=opener)
            self.assertEqual(opener.call_count, 2)


class ImageCacheTest(unittest.TestCase):
    def setUp(self):
        self.image = firmware(b"exact running firmware" * 20, VERSION, TARGET, HW)

    def dfu_zip(self, path, image=None, member="application.bin"):
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("manifest.json", json.dumps({"manifest": {"application": {"bin_file": member}}}))
            archive.writestr(member, image or self.image)

    def test_dfu_application_identity_and_persistent_exact_base_cache(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "application.zip"
            self.dfu_zip(source)
            cache = catalog.ImageCache(root / "cache")
            identity = catalog.load_application(source, target_id=TARGET, hw_id=HW)
            self.assertEqual(identity.image, self.image)
            path = cache.store(source, target_id=TARGET, hw_id=HW)
            self.assertEqual(cache.find(identity.body_hash, TARGET, HW, fw_version=VERSION), path)
            self.assertEqual(catalog.ImageCache(root / "cache").find(identity.body_hash.hex(), TARGET, HW), path)
            self.assertIsNone(cache.find(identity.body_hash, TARGET + 1, HW))
            self.assertIsNone(cache.find(identity.body_hash, TARGET, "OtherBoard"))
            self.assertIsNone(cache.find(identity.body_hash, TARGET, HW, fw_version=VERSION + 1))
            path.write_bytes(self.image[:-1] + b"!")
            self.assertIsNone(cache.find(identity.body_hash, TARGET, HW))

    def test_wrong_target_hash_and_unsafe_dfu_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "application.zip"
            self.dfu_zip(source)
            with self.assertRaisesRegex(ota.OtaError, "target"):
                catalog.load_application(source, target_id=TARGET + 1)
            self.dfu_zip(source, self.image[:1] + b"!" + self.image[2:])
            with self.assertRaisesRegex(ota.OtaError, "hash"):
                catalog.load_application(source)
            self.dfu_zip(source, member="../application.bin")
            with self.assertRaisesRegex(ota.OtaError, "unsafe"):
                catalog.load_application(source)

    def test_generic_archive_requires_unambiguous_matching_target(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "application.zip"
            other = firmware(b"other board", VERSION, TARGET + 1, "OtherBoard")
            with zipfile.ZipFile(source, "w") as archive:
                archive.writestr("target.bin", self.image)
                archive.writestr("other.bin", other)
            self.assertEqual(catalog.load_application(source, target_id=TARGET).image, self.image)
            with self.assertRaisesRegex(ota.OtaError, "unambiguous"):
                catalog.load_application(source)

    def test_full_mota_is_a_base_but_delta_is_not(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "application.mota"
            source.write_bytes(mota_blob(self.image, version=VERSION, target=TARGET, hw=HW))
            self.assertEqual(catalog.load_application(source, target_id=TARGET, hw_id=HW).image, self.image)
            source.write_bytes(mota_blob(self.image, full=False, version=VERSION, target=TARGET, hw=HW))
            with self.assertRaisesRegex(ota.OtaError, "full application"):
                catalog.load_application(source)

    def test_body_hash_does_not_allow_replacing_a_different_trailer(self):
        with tempfile.TemporaryDirectory() as folder:
            cache = catalog.ImageCache(Path(folder))
            identity = ota.parse_endf(self.image)
            cache.remember(identity)
            changed = bytearray(self.image)
            struct.pack_into("<I", changed, len(changed) - 40, VERSION + 1)
            with self.assertRaisesRegex(ota.OtaError, "different complete image"):
                cache.remember(ota.parse_endf(bytes(changed)))
            with self.assertRaises(ota.OtaError):
                cache.find("too short", TARGET)


class UpdateStrategyTest(unittest.TestCase):
    def test_delta_preferred_and_full_fallback_is_only_used_when_installable(self):
        common = {"platform": "nrf52", "external_storage": True,
                  "exact_base": True, "delta_supported": True}
        self.assertEqual(catalog.choose_update_strategy(**common).kind, "delta")
        self.assertEqual(catalog.choose_update_strategy(**common, low_power=True).kind, "full")
        self.assertEqual(catalog.choose_update_strategy(**common, low_power=True, can_build_delta=True).kind, "delta")
        self.assertEqual(catalog.choose_update_strategy(**{**common, "exact_base": False}).kind, "full")
        self.assertEqual(catalog.choose_update_strategy(**{**common, "exact_base": False}, prebuilt_delta=True).kind, "delta")
        self.assertEqual(catalog.choose_update_strategy(**{**common, "external_storage": False}, can_build_delta=False).kind, "unavailable")
        self.assertEqual(catalog.choose_update_strategy(platform="rp2040", external_storage=True,
                                                       exact_base=True, delta_supported=True).kind, "unavailable")
        self.assertEqual(catalog.choose_update_strategy(platform="esp32", external_storage=False,
                                                       exact_base=False, delta_supported=False).kind, "full")


if __name__ == "__main__":
    unittest.main()
