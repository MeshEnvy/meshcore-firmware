"""Offline direct-update transactions with stateful radios and real containers."""
from __future__ import annotations

import asyncio
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import lora_ota as ota
import ota_wizard_session as wizard
from test_lora_ota import firmware
from test_lora_ota_bootloader import boot_blob


KEY = "12" * 32
SOURCE = "34" * 32
TARGET = 0xE5331F02
HW = "ikoka_stick_nrf_33dbm"
OLD = 0x01110109
NEW = 0x01120001
BOOT_OLD = 0x02040BFF
BOOT_NEW = 0x02040CFF
TEST_SIGNER = "8A88E3DD7409F195FD52DB2D3CBA5D72CA6709BF1D94121BF3748801B40F6F5C"


def application(version=NEW, body=b"new application" * 600):
    return ota.parse_endf(firmware(body, version, TARGET, HW))


def boot_status(staged="none mid=- hash=-"):
    return ("BL board=28860044 target=28860044 name=XIAO_DFU "
            "crc=6F9BE280 abi=3 caps=0E | staged:" + staged)


class FakeTransport:
    """Apply mutations before optionally losing their replies, as real USB can."""
    source_key = SOURCE

    def __init__(self):
        self.image = application(OLD, b"old application" * 600)
        self.boot_version = BOOT_OLD
        self.calls = []
        self.windows = {"source": False, "target": False}
        self.temp_schedules = {"source": "-none-", "target": "-none-"}
        self.rxps = {"source": "off,level=8,preamble=16,rx=10000,sleep=10000",
                     "target": "on,level=8,preamble=16,rx=10000,sleep=10000"}
        self.policies = {"source": dict(hops="3", autofetch="off", autoinstall="off"),
                         "target": dict(hops="2", autofetch="on", autoinstall="on")}
        self.seeder = True
        self.unsupported_source = False
        self.serving = None
        self.serves = []
        self.mid = None
        self.lost_pull = self.lost_install = False
        self.bad_mid = self.bad_hash = self.bad_body = False
        self.truncate_status = False
        self.hold_download = False
        self.download_started = asyncio.Event()
        self.ignore_restore = False
        self.restore_path_called = False
        self.reset_calls = []
        self.login_stale = False

    async def local_command(self, text):
        return await self.command("source", text)

    async def remote_command(self, key, password, text):
        if key != KEY or password != "secret-for-test":
            raise AssertionError("incorrect target or credentials")
        if self.login_stale:
            raise AssertionError("post-reboot probe needs a fresh authenticated session")
        return await self.command("target", text)

    async def reset_remote_session(self, key):
        self.reset_calls.append(key)
        self.login_stale = False

    async def serve(self, blob, stop):
        self.serving = ota.parse_mota(blob)
        self.serves.append(self.serving)
        await stop.wait()

    async def restore_contact_path(self):
        self.restore_path_called = True

    async def command(self, label, text):
        self.calls.append((label, text))
        if text == "get public.key":
            return KEY if label == "target" else SOURCE
        if text == "ver":
            return ota.format_version(self.image.fw_version) + "-dev-test"
        if text == "get bootloader.ver":
            return f"OTAFIX{self.boot_version >> 24}.{self.boot_version >> 16 & 255}.{self.boot_version >> 8 & 255}"
        if text == "ota status":
            result = f"OTA | this fw 0000 hw={HW} | target:{TARGET:08X} | maxblk:2048 | bl:QSPI"
            if self.mid:
                mid = "DEADBEEF" if self.bad_mid else self.mid
                state = "downloading" if self.hold_download else "ready to install"
                result += f" | download: {state} 1/1 (100%)" + ("" if self.truncate_status else f" id={mid}")
            else:
                result += " | no download"
            return result
        if text == "ota stats":
            mid = "DEADBEEF" if self.bad_mid else self.mid
            state = "dl" if self.hold_download else "done"
            return f"OTA | fw v1.17.1.9 id=DEADCAFE body=0000 | fetch {state} 1/1 100% id={mid} 0s"
        if text == "ota self":
            return f"self body=9000 image=9056 base_hash={self.image.body_hash.hex()} | bootloader: QSPI apply OK (abi=3 codecs=0x5)"
        if text == "ota bootloader status":
            if self.mid:
                image_hash = "00" * 8 if self.bad_hash else self.serving.image_hash[:8].hex()
                return boot_status(f"ready mid={self.mid} hash={image_hash}")
            return boot_status()
        if text == "ota config":
            mode = "mode=seeder-only " if label == "source" and self.seeder else ""
            return "ota config: " + mode + " ".join(f"{key}={value}" for key, value in self.policies[label].items())
        if text.startswith("ota config "):
            _, _, field, value = text.split()
            if label == "source" and self.seeder and field != "hops":
                return "ERR seeder-only policy must stay off"
            if not self.ignore_restore or value == "0":
                self.policies[label][field] = value
            return "OK"
        if text == "get radio":
            return "910.525024,62.5,7,5"
        if text in ("get radio.rxps.config", "get radio.rxps"):
            if label == "source" and self.unsupported_source:
                return "ERR RX power saving is unsupported on this radio"
            return self.rxps[label]
        if text == "set radio.rxps off":
            self.rxps[label] = self.rxps[label].replace("on,", "off,")
            return "OK off"
        if text == "set radio.rxps level 8 preamble 16":
            self.rxps[label] = "on,level=8,preamble=16,rx=10000,sleep=10000"
            return "OK on"
        if text == "get tempradio":
            if label == "source":
                return "TempRadio active: 910.525024,62.5,7,5 14000s left,preamble=16" if self.windows[label] else "TempRadio inactive"
            return "910.525024,62.5,7,5,240" if self.windows[label] else "off"
        if text == "get tempradioat":
            return self.temp_schedules[label]
        if text.startswith("tempradio "):
            self.windows[label] = True
            return "OK applying in 2s"
        if text == "normalradio":
            self.windows[label] = False
            return "OK"
        if text.startswith("ota key add "):
            return "OK trusted key added"
        if text == "ota ls":
            return f"OTA | {self.serving.manifest_id}"
        if text.startswith("ota pull "):
            if not all(self.windows.values()):
                raise AssertionError("pull without overlapping leases")
            self.mid = text.split()[2]
            self.download_started.set()
            if self.lost_pull:
                raise TimeoutError("lost pull acknowledgment")
            return "OK pulling"
        if text.startswith("ota bootloader install ") or text == "ota install":
            if self.serving.is_bootloader:
                self.boot_version = self.serving.fw_version
            else:
                self.image = ota.parse_endf(self.serving.payload)
                if self.bad_body:
                    self.image = application(self.image.fw_version, b"different body" * 600)
            self.windows["target"] = False  # reboot clears the RAM lease
            self.login_stale = True
            self.mid = None
            if self.lost_install:
                raise TimeoutError("lost install acknowledgment")
            return "OK rebooting"
        raise AssertionError((label, text))


class SessionTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.link = FakeTransport()
        self.path = Path(self.temporary.name) / "session.json"
        self.session = wizard.DirectSession(self.link, KEY, "secret-for-test", self.path,
                                            progress=lambda _: None, poll_seconds=.001,
                                            reboot_timeout=.03, transfer_timeout=1)

    async def test_inspect_proves_board_storage_and_boot_contract(self):
        destination = await self.session.inspect()
        self.assertEqual((destination.public_key, destination.target_id, destination.hw_id), (KEY, TARGET, HW))
        self.assertEqual((destination.platform, destination.external_storage, destination.codecs), ("nrf52", True, 5))
        self.assertEqual((destination.bootloader.storage_flags, destination.boot_version), (14, BOOT_OLD))

    async def test_rejects_wrong_target_identity_before_mutations(self):
        original = self.link.remote_command
        async def wrong(key, password, text):
            return "56" * 32 if text == "get public.key" else await original(key, password, text)
        self.link.remote_command = wrong
        with self.assertRaisesRegex(ota.OtaError, "public key"):
            await self.session.inspect()
        self.assertEqual(self.link.calls, [])

    async def test_lost_pull_and_install_replies_are_reconciled_once(self):
        self.link.lost_pull = self.link.lost_install = self.link.truncate_status = True
        before = await self.session.inspect()
        package = wizard.full_package(application(), 1024)
        after = await self.session.install(package, before)
        self.assertEqual((after.version, after.base_hash), (NEW, application().body_hash))
        commands = [text for label, text in self.link.calls if label == "target"]
        self.assertEqual(sum(text.startswith("ota pull ") for text in commands), 1)
        self.assertEqual(commands.count("ota install"), 1)
        self.assertIn("ota stats", commands)
        self.assertEqual(await self.session.cleanup(), [])
        self.assertEqual(self.link.rxps["source"].split(",")[0], "off")
        self.assertEqual(self.link.rxps["target"].split(",")[0], "on")
        self.assertEqual(self.link.policies["target"], dict(hops="2", autofetch="on", autoinstall="on"))
        self.assertFalse(any(self.link.windows.values()))
        self.assertTrue(self.link.restore_path_called)
        journal = json.loads(self.path.read_text())
        self.assertEqual((journal["target_key"], journal["source_key"], journal["phase"]), (KEY, SOURCE, "finished"))
        self.assertNotIn("secret-for-test", self.path.read_text())

    async def test_bootloader_then_application_reopens_target_window(self):
        before = await self.session.inspect()
        boot = wizard.Package(boot_blob(storage=14, version=BOOT_NEW), signer=TEST_SIGNER)
        after_boot = await self.session.install(boot, before)
        self.assertEqual(after_boot.base_hash, before.base_hash)
        self.assertEqual(after_boot.boot_version, BOOT_NEW)
        after = await self.session.install(wizard.full_package(application(), 1024), after_boot)
        self.assertEqual(after.version, NEW)
        target = [text for label, text in self.link.calls if label == "target"]
        self.assertEqual(sum(text.startswith("tempradio ") for text in target), 2)
        self.assertEqual(len(self.link.serves), 2)
        self.assertEqual(self.link.reset_calls, [KEY, KEY])
        self.assertEqual(await self.session.cleanup(), [])

    async def test_wrong_mid_fails_without_install(self):
        before = await self.session.inspect()
        self.link.bad_mid = True
        try:
            with self.assertRaisesRegex(ota.OtaError, "not the selected package"):
                await self.session.install(wizard.full_package(application(), 1024), before)
        finally:
            self.assertEqual(await self.session.cleanup(), [])
        self.assertNotIn(("target", "ota install"), self.link.calls)

    async def test_staged_bootloader_hash_must_match_exactly(self):
        before = await self.session.inspect()
        self.link.bad_hash = True
        try:
            with self.assertRaisesRegex(ota.OtaError, "MID/hash"):
                await self.session.install(wizard.Package(boot_blob(storage=14, version=BOOT_NEW), signer=TEST_SIGNER), before)
        finally:
            await self.session.cleanup()
        self.assertFalse(any(text.startswith("ota bootloader install ") for _, text in self.link.calls))

    async def test_running_version_alone_is_not_install_success(self):
        before = await self.session.inspect()
        self.link.bad_body = True
        try:
            with self.assertRaisesRegex(ota.OtaError, "version and body hash"):
                await self.session.install(wizard.full_package(application(), 1024), before)
        finally:
            await self.session.cleanup()

    async def test_destination_temporarily_missing_after_reboot_is_probed_again(self):
        import itertools
        from types import SimpleNamespace
        from unittest import mock
        from ota_wizard_companion import CompanionError
        before = await self.session.inspect()
        original = self.link.remote_command
        missing = 2
        async def reboot_discovery(key, password, text):
            nonlocal missing
            if self.link.image.fw_version == NEW and text == "get public.key" and missing:
                missing -= 1
                raise CompanionError("No directly reachable destination replied yet")
            return await original(key, password, text)
        self.link.remote_command = reboot_discovery
        # Advance only this module's deadline clock; compiler load must not
        # exhaust the tiny test reboot budget between three fake probes.
        clock = SimpleNamespace(monotonic=itertools.count(0, .001).__next__)
        with mock.patch.object(wizard, "time", clock):
            after = await self.session.install(wizard.full_package(application(), 1024), before)
        self.assertEqual(after.version, NEW)
        self.assertEqual(missing, 0)
        self.assertEqual(len(self.link.reset_calls), 3)
        self.assertEqual([text for _, text in self.link.calls].count("ota install"), 1)
        self.assertEqual(await self.session.cleanup(), [])

    async def test_cancelled_download_can_restore_every_setting(self):
        before = await self.session.inspect()
        self.link.hold_download = True
        task = asyncio.create_task(self.session.install(wizard.full_package(application(), 1024), before))
        await asyncio.wait_for(self.link.download_started.wait(), 2)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertEqual(await self.session.cleanup(), [])
        self.assertFalse(self.session.active)
        self.assertFalse(any(self.link.windows.values()))
        self.assertNotIn(("target", "ota install"), self.link.calls)

    async def test_cleanup_requires_policy_readback_and_can_be_retried(self):
        await self.session.begin()
        self.link.ignore_restore = True
        self.assertTrue(await self.session.cleanup())
        self.assertTrue(self.session.active)
        self.assertEqual(json.loads(self.path.read_text())["phase"], "needs cleanup")
        self.link.ignore_restore = False
        self.assertEqual(await self.session.cleanup(), [])
        self.assertFalse(self.session.active)

    async def test_unsupported_source_rxps_is_explicitly_skipped(self):
        self.link.unsupported_source = True
        await self.session.begin()
        self.assertEqual(self.session.saved["source"]["rxps"], "unsupported")
        self.assertNotIn(("source", "set radio.rxps off"), self.link.calls)
        self.assertEqual(await self.session.cleanup(), [])

    async def test_existing_window_prevents_any_mutation(self):
        self.link.windows["source"] = True
        with self.assertRaisesRegex(ota.OtaError, "already has a temporary"):
            await self.session.begin()
        self.assertFalse(self.session.active)
        self.assertFalse(any(text.startswith("set ") or text.startswith("ota config ") for _, text in self.link.calls))

    async def test_legacy_future_temp_schedule_is_preserved_before_any_mutation(self):
        future = "> 1:910.540,62.5,7,5,32@1900000000-1900000900"
        for label in ("source", "target"):
            self.link.calls.clear()
            self.link.temp_schedules = {"source": "-none-", "target": "-none-"}
            self.link.temp_schedules[label] = future
            with self.subTest(label=label), self.assertRaisesRegex(ota.OtaError, "scheduled temporary"):
                await self.session.begin()
            self.assertFalse(self.session.active)
            self.assertEqual(self.link.temp_schedules[label], future)
            self.assertFalse(any(text.startswith(("set ", "ota config ", "tempradio ", "normalradio"))
                                 for _, text in self.link.calls))

    async def test_schedule_queued_during_preparation_is_checked_again(self):
        await self.session.snapshot()
        self.link.calls.clear()
        self.link.temp_schedules["target"] = "> 1:910.540,62.5,7,5@1900000000-1900000900"
        with self.assertRaisesRegex(ota.OtaError, "scheduled temporary"):
            await self.session.begin()
        self.assertFalse(self.session.active)
        self.assertFalse(any(text.startswith(("set ", "ota config ", "tempradio ", "normalradio"))
                             for _, text in self.link.calls))

    async def test_roles_without_primary_schedule_getter_can_still_update(self):
        self.link.temp_schedules = {"source": "Unknown command", "target": "Error: unsupported"}
        await self.session.begin()
        self.assertTrue(self.session.active)
        self.assertEqual(await self.session.cleanup(), [])

    async def test_schedule_read_timeout_cannot_be_treated_as_unsupported(self):
        original = self.link.local_command
        async def uncertain(text):
            if text == "get tempradioat":
                raise ota.TransmissionError("unknown command result after timeout")
            return await original(text)
        self.link.local_command = uncertain
        with self.assertRaises(ota.TransmissionError):
            await self.session.begin()
        self.assertFalse(self.session.active)
        self.assertFalse(any(text.startswith(("set ", "ota config ", "tempradio ", "normalradio"))
                             for _, text in self.link.calls))


if __name__ == "__main__":
    unittest.main()
