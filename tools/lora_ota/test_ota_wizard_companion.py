"""Offline tests of the one-connection Companion OTA transport."""
import asyncio
from copy import deepcopy
from enum import Enum
from types import SimpleNamespace
import sys
import unittest
from unittest import mock
import time

import ota_wizard_companion as companion


KEY = "ab" * 32
OTHER = "cd" * 32


class Types(Enum):
    OK = "command_ok"
    ERROR = "command_error"
    DISCOVER_RESPONSE = "discover_response"
    RX_LOG_DATA = "rx_log_data"
    CONTACT_MSG_RECV = "contact_message"
    LOGIN_SUCCESS = "login_success"
    LOGIN_FAILED = "login_failed"
    MESSAGES_WAITING = "messages_waiting"
    DISCONNECTED = "disconnected"


def event(kind="command_ok", **payload):
    return SimpleNamespace(type=kind, payload=payload)


def contact(key=KEY):
    return dict(public_key=key, adv_name="Hilltop", type=2, flags=1,
                out_path="12345678", out_path_len=2, out_path_hash_mode=1,
                last_advert=100, adv_lat=1, adv_lon=2)


class Commands:
    def __init__(self, sdk):
        self.sdk = sdk
        self.calls = []
        self.active = self.maximum_active = 0
        self.raw = []
        self.login_allowed = True
        self.login_admin = True
        self.login_count = 0
        self.command_replies = True
        self.temporary = "TempRadio active: 910.525,62.50,7,5 100s left,preamble=16"
        self.next_reply = None
        self.guard_supported = True
        self.guard_version = 2
        self.mailbox_prefix = "--"
        self.mailbox = None
        self.inbox = []
        self.guard_modes = []
        self.guard_busy = 0
        self.proof_key = KEY
        self.login_acl = 3
        self.clock = int(time.time())
        self.time_sets = []
        self.command_times = []
        self.ignore_clock_advance = False

    async def response(self, method, result=None):
        self.calls.append(method)
        self.active += 1
        self.maximum_active = max(self.active, self.maximum_active)
        await asyncio.sleep(0)
        self.active -= 1
        return result or event()

    async def get_contacts(self):
        return await self.response("get_contacts", event("contacts"))

    async def get_msg(self, timeout=None):
        if self.inbox:
            return await self.response("get_msg", self.inbox.pop(0))
        return await self.response("get_msg", event("no_more_messages"))

    async def send_appstart(self):
        return await self.response("send_appstart", event("self_info", **self.sdk.self_info))

    async def set_radio(self, freq, bw, sf, cr):
        self.sdk.self_info.update(radio_freq=freq, radio_bw=bw, radio_sf=sf, radio_cr=cr)
        return await self.response("set_radio")

    async def run_cli_command(self, text):
        if text == "get ota.direct":
            busy = int(self.guard_busy > 0)
            self.guard_busy = max(0, self.guard_busy - 1)
            return await self.response("run_cli_command", event("cli_reply", text=f"> ota.direct v={self.guard_version} active=1 busy={busy}"))
        if text.startswith("get ota.direct.reply "):
            offset = int(text.rsplit(" ", 1)[1])
            body = self.mailbox
            raw = b"" if body is None else body[offset:offset+40]
            return await self.response("run_cli_command", event("cli_reply", text=(
                f"> ota.reply req={self.mailbox_prefix} state={'pending' if body is None else 'ready'} "
                f"bytes={0 if body is None else len(body)} off={offset} data={raw.hex() or '-'}")))
        return await self.response("run_cli_command", event("cli_reply", text=self.temporary))

    async def send(self, data, expected):
        assert data[:1] == b"\x34" and data[2:34].hex() == KEY
        assert len(data) == {1:35,2:34,3:34,4:37}[data[1]]
        self.guard_modes.append(data[1])
        if not self.guard_supported:
            return await self.response("send", event("command_error", reason="unsupported"))
        if data[1] == 1:
            assert data[34] in (2, 3)
        if data[1] == 2:
            self.guard_busy = 1
        if data[1] == 4:
            assert data[36] == ord('|')
            self.mailbox_prefix = data[34:36].decode()
            self.mailbox = None
        return await self.response("send")

    async def set_time(self, timestamp):
        assert timestamp >= self.clock, "The host must never turn the source clock back"
        self.time_sets.append(timestamp)
        if not self.ignore_clock_advance:
            self.clock = timestamp
        return await self.response("set_time")

    async def get_time(self):
        return await self.response("get_time", event("current_time", time=self.clock))

    async def update_contact(self, value):
        self.sdk.contacts[value["public_key"]] = deepcopy(value)
        return await self.response("update_contact")

    async def remove_contact(self, key):
        self.sdk.contacts.pop(key, None)
        return await self.response("remove_contact")

    async def send_node_discover_req(self, filter, prefix_only, tag):
        assert filter == 0x0C and prefix_only is False
        fields = dict(tag=tag.to_bytes(4, "little").hex(), pubkey=KEY,
                      node_type=2, path_len=0, RSSI=-70, SNR=7)
        self.sdk.emit(Types.DISCOVER_RESPONSE, **fields)
        for changed in (dict(tag="00000000"), dict(pubkey=OTHER, path_len=1),
                        dict(pubkey="ab" * 8), dict(pubkey=OTHER, node_type=1)):
            self.sdk.emit(Types.DISCOVER_RESPONSE, **(fields | changed))
        return await self.response("send_node_discover_req")

    async def send_login(self, destination, password):
        assert destination["out_path_len"] == 0 and destination["out_path"] == ""
        self.login_count += 1
        # A wrong radio's success must not authenticate this one; the listener
        # is registered before the USB acknowledgement can arrive.
        self.sdk.emit(Types.LOGIN_SUCCESS, pubkey_prefix=OTHER[:12], is_admin=True)
        self.sdk.emit(Types.LOGIN_SUCCESS if self.login_allowed else Types.LOGIN_FAILED,
                      pubkey_prefix=KEY[:12], is_admin=self.login_admin,
                      permissions=1 if self.login_admin else 0, acl_permissions=self.login_acl)
        return await self.response("send_login", event("message_sent"))

    async def send_cmd(self, destination, text, timestamp=None):
        assert destination["out_path_len"] == 0
        assert self.guard_busy == 0, "CLI overtook the lower-priority return PATH"
        self.command_times.append(timestamp)
        if self.command_replies:
            is_proof = text[3:] == "get public.key"
            result = self.proof_key if is_proof else self.next_reply or "> verified"
            reply = dict(pubkey_prefix=KEY[:12], txt_type=1, path_len=255,
                         text=text[:3] + result)
            if not is_proof:
                self.next_reply = None
            stale = ("00|" if text[:3] != "00|" else "11|") + "wrong request"
            for changed in (dict(pubkey_prefix=OTHER[:12]), dict(txt_type=0),
                            dict(path_len=2), dict(path_len=0), dict(text=stale)):
                self.sdk.emit(Types.CONTACT_MSG_RECV, **(reply | changed))
            self.sdk.emit(Types.CONTACT_MSG_RECV, **reply)
            assert self.mailbox_prefix == text[:2]
            self.mailbox = result.encode("utf-8")
        return await self.response("send_cmd", event("message_sent"))

    async def send_raw_packet(self, raw, priority):
        self.raw.append(raw)
        return await self.response("send_raw_packet")


class SDK:
    def __init__(self, contacts=None):
        self.contacts = {KEY: contact()} if contacts is None else contacts
        self.self_info = dict(public_key="ef" * 32, radio_freq=910.525,
                              radio_bw=62.5, radio_sf=7, radio_cr=5)
        self.commands = Commands(self)
        self.subscriptions = []
        self.closed = False

    def subscribe(self, kind, callback):
        subscription = (kind, callback)
        self.subscriptions.append(subscription)
        return subscription

    def unsubscribe(self, subscription):
        self.subscriptions.remove(subscription)

    def emit(self, kind, **payload):
        for subscribed, callback in list(self.subscriptions):
            if kind == subscribed:
                callback(event(kind, **payload))

    async def disconnect(self):
        self.closed = True


class FakeSource:
    last = None

    def __init__(self, blob, seeder_id, compressor):
        FakeSource.last = self
        self.stats = {"requests": 0}
        self.sent = []

    def prepare(self):
        pass

    def advert(self):
        return b"\x31\0\x01hostdata"

    def replies(self, raw):
        self.stats["requests"] += 1
        return [b"\x31\0\x05reply"]

    def note_sent(self, frame):
        self.sent.append(frame)


class CompanionTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.sdk = SDK()
        self.original = deepcopy(self.sdk.contacts[KEY])

        async def factory(port, **kwargs):
            self.assertEqual(port, "FAKE")
            self.assertTrue(kwargs["only_error"])
            self.assertFalse(kwargs["auto_reconnect"])
            return self.sdk

        self.transport = companion.CompanionTransport(
            "FAKE", sdk_factory=factory, event_types=Types,
            discovery_seconds=0, remote_timeout=.05,
            source_factory=FakeSource)
        await self.transport.open()

    async def asyncTearDown(self):
        await self.transport.close()

    async def test_open_check_and_admin_leave_private_and_channel_inbox_untouched(self):
        queued = [event("contact_message", text="private message"),
                  event("channel_message", text="channel message")]
        self.sdk.commands.inbox = list(queued)
        self.sdk.emit(Types.MESSAGES_WAITING)
        await asyncio.sleep(.01)
        self.assertEqual(self.sdk.commands.inbox, queued)
        self.assertEqual(await self.transport.remote_command(KEY, "secret", "ver"), "> verified")
        self.assertEqual(self.sdk.commands.inbox, queued)
        self.assertNotIn("get_msg", self.sdk.commands.calls)
        self.assertNotIn("update_contact", self.sdk.commands.calls)
        self.assertNotIn("remove_contact", self.sdk.commands.calls)

    async def test_v1_source_is_rejected_without_draining_or_changing_contacts(self):
        self.sdk.commands.guard_version = 1
        queued = [event("contact_message", text="unread")]
        self.sdk.commands.inbox = list(queued)
        with self.assertRaisesRegex(companion.CompanionError, "v2"):
            await self.transport.remote_command(KEY, "secret", "ver")
        self.assertEqual(self.sdk.commands.inbox, queued)
        self.assertEqual(self.sdk.commands.login_count, 0)
        self.assertEqual(self.sdk.commands.guard_modes, [])
        self.assertEqual(self.sdk.contacts[KEY], self.original)
        self.assertNotIn("get_msg", self.sdk.commands.calls)

    async def test_v1_source_cannot_emit_discovery_or_login(self):
        self.sdk.commands.guard_version = 1
        with self.assertRaisesRegex(companion.CompanionError, "v2"):
            await self.transport.discover()
        self.assertNotIn("send_node_discover_req", self.sdk.commands.calls)
        self.assertEqual(self.sdk.commands.login_count, 0)

    async def test_hard_restart_has_no_changed_route_to_restore(self):
        await self.transport.remote_command(KEY, "secret", "ver")
        self.assertEqual(self.sdk.contacts[KEY], self.original)
        async def factory(*args, **kwargs):
            return self.sdk
        restarted = companion.CompanionTransport("FAKE", sdk_factory=factory,
            event_types=Types, discovery_seconds=0, remote_timeout=.05)
        await restarted.open()
        try:
            await restarted.remote_command(KEY, "secret", "ver")
            await restarted.restore_contact_path()
            self.assertEqual(self.sdk.contacts[KEY], self.original)
            self.assertNotIn("update_contact", self.sdk.commands.calls)
            self.assertNotIn("remove_contact", self.sdk.commands.calls)
        finally:
            await restarted.close()

    async def test_mailbox_reassembles_full_reply_without_reading_normal_inbox(self):
        expected = "0123456789abcdef" * 10
        self.sdk.commands.next_reply = expected
        self.assertEqual(await self.transport.remote_command(KEY, "secret", "ver"), expected)
        self.assertNotIn("get_msg", self.sdk.commands.calls)

    async def test_printable_tags_support_long_transfers_without_reusing_old_tags(self):
        old = companion.string.ascii_letters + companion.string.digits
        self.transport._used_correlations = {a + b + "|" for a in old for b in old}
        with mock.patch.object(companion.secrets, "choice", side_effect=["]", "\\", "`", "~"]):
            self.assertEqual(await self.transport.remote_command(KEY, "secret", "ver"), "> verified")
        self.assertIn("]\\|", self.transport._used_correlations)
        self.assertIn("`~|", self.transport._used_correlations)
        self.assertEqual(len(self.transport._used_correlations), len(old) ** 2 + 2)

    async def test_discovery_uses_fresh_full_key_direct_responses_only(self):
        rows = await self.transport.discover()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["public_key"], KEY)
        self.assertEqual(rows[0]["rssi"], -70)
        self.assertEqual(rows[0]["name"], "Hilltop")
        self.assertEqual(self.sdk.contacts[KEY], self.original)

    async def test_native_settings_and_binary_local_cli_share_wire_lock(self):
        self.assertEqual(self.transport.source_key, "ef" * 32)
        self.assertEqual(await self.transport.get_radio(), (910.525, 62.5, 7, 5))
        await asyncio.gather(self.transport.get_radio(), self.transport.local_command("get tempradio"))
        self.assertEqual(self.sdk.commands.maximum_active, 1)
        await self.transport.set_radio((911, 125, 8, 5))
        self.assertEqual(await self.transport.get_radio(), (911, 125, 8, 5))

    async def test_binary_local_cli_preserves_unsupported_capability_reason(self):
        self.sdk.commands.temporary = "ERR RX power saving is unsupported on this radio"
        reply = await self.transport.local_command("get radio.rxps.config")
        self.assertIn("unsupported on this radio", reply)

    async def test_guest_success_cannot_authenticate_as_admin(self):
        self.sdk.commands.login_admin = False
        with self.assertRaisesRegex(companion.CompanionError, "admin login"):
            await self.transport.remote_command(KEY, "secret", "ver")
        self.assertNotIn(KEY, self.transport._authenticated)
        self.assertNotIn("send_cmd", self.sdk.commands.calls)

    async def test_old_source_guard_rejection_does_not_emit_a_radio_login(self):
        self.sdk.commands.guard_supported = False
        with self.assertRaisesRegex(companion.CompanionError, "current Full Companion"):
            await self.transport.remote_command(KEY, "secret", "ota pull DEADBEEF flash")
        self.assertEqual(self.sdk.commands.login_count, 0)
        self.assertNotIn("send_cmd", self.sdk.commands.calls)

    async def test_manager_role_cannot_authenticate_for_ota(self):
        self.sdk.commands.login_acl = 4
        with self.assertRaisesRegex(companion.CompanionError, "admin login"):
            await self.transport.remote_command(KEY, "secret", "ver")
        self.assertNotIn(KEY, self.transport._authenticated)

    async def test_clock_a_little_ahead_never_moves_back_and_cli_follows_login(self):
        now = 1800000000
        self.sdk.commands.clock = now + 300
        with mock.patch.object(companion.time, "time", return_value=now):
            await self.transport.remote_command(KEY, "secret", "ver")
        self.assertEqual(self.sdk.commands.time_sets, [])
        self.assertTrue(all(stamp > now + 300 for stamp in self.sdk.commands.command_times))
        self.assertLess(self.sdk.commands.calls.index("get_time"), self.sdk.commands.calls.index("send_login"))

    async def test_stale_clock_is_advanced_and_read_back_before_login(self):
        now = 1800000000
        self.sdk.commands.clock = now - 3600
        with mock.patch.object(companion.time, "time", return_value=now):
            await self.transport.remote_command(KEY, "secret", "ver")
        self.assertEqual(self.sdk.commands.time_sets, [now + 2])
        self.assertEqual(self.sdk.commands.calls.count("get_time"), 2)
        self.assertTrue(all(stamp > now + 2 for stamp in self.sdk.commands.command_times))

    async def test_bad_or_far_future_clock_stops_before_radio_login(self):
        now = 1800000000
        for value in (now + 601, None, "1800000000", True, -1):
            self.sdk.commands.clock = value
            with self.subTest(clock=value), mock.patch.object(companion.time, "time", return_value=now):
                with self.assertRaisesRegex(companion.CompanionError, "clock|RTC"):
                    await self.transport.remote_command(KEY, "secret", "ver")
        self.assertEqual(self.sdk.commands.login_count, 0)
        self.assertEqual(self.sdk.commands.time_sets, [])

    async def test_unverified_clock_advance_stops_before_radio_login(self):
        now = 1800000000
        self.sdk.commands.clock = now - 60
        self.sdk.commands.ignore_clock_advance = True
        with mock.patch.object(companion.time, "time", return_value=now):
            with self.assertRaisesRegex(companion.CompanionError, "verify.*clock advance"):
                await self.transport.remote_command(KEY, "secret", "ver")
        self.assertEqual(self.sdk.commands.login_count, 0)

    async def test_wrong_full_key_proof_cannot_run_the_requested_command(self):
        self.sdk.commands.proof_key = OTHER
        with self.assertRaisesRegex(companion.CompanionError, "complete public key"):
            await self.transport.remote_command(KEY, "secret", "ota install")
        self.assertNotIn(KEY, self.transport._authenticated)
        self.assertEqual(len(self.transport._used_correlations), 1)

    async def test_strict_login_reply_correlation_and_path_restore(self):
        for command in ("ver", "ota self"):
            self.assertEqual(await self.transport.remote_command(KEY, "secret", command), "> verified")
        self.assertEqual(self.sdk.commands.login_count, 1)
        self.assertEqual(len(self.transport._used_correlations), 3)
        self.assertEqual(self.sdk.contacts[KEY], self.original)
        await self.transport.restore_contact_path()
        self.assertEqual(self.sdk.contacts[KEY], self.original)
        self.assertEqual(self.sdk.commands.guard_modes, [1, 2, 4, 4, 1, 4, 3])

    async def test_reboot_reset_reauthenticates_without_reusing_old_correlations(self):
        await self.transport.remote_command(KEY, "secret", "ver")
        previous = set(self.transport._used_correlations)
        await self.transport.reset_remote_session(KEY)
        self.assertNotIn(KEY, self.transport._authenticated)
        self.assertEqual(self.transport._used_correlations, previous)
        self.assertEqual(self.sdk.contacts[KEY], self.original)
        await self.transport.remote_command(KEY, "secret", "ota self")
        self.assertEqual(self.sdk.commands.login_count, 2)
        self.assertEqual(len(self.transport._used_correlations), 4)
        self.assertTrue(previous < self.transport._used_correlations)

    async def test_rejected_login_restores_route_on_close_without_password_error(self):
        self.sdk.commands.login_allowed = False
        with self.assertRaises(companion.CompanionError) as caught:
            await self.transport.remote_command(KEY, "my-password", "ver")
        self.assertNotIn("my-password", str(caught.exception))
        await self.transport.close()
        self.assertEqual(self.sdk.contacts[KEY], self.original)

    async def test_new_direct_contact_removed_after_session(self):
        self.sdk.contacts.clear()
        await self.transport.discover()
        self.assertEqual(await self.transport.remote_command(KEY, "secret", "ver"), "> verified")
        await self.transport.restore_contact_path()
        self.assertNotIn(KEY, self.sdk.contacts)

    async def test_ambiguous_prefix_and_unseen_key_refused(self):
        with self.assertRaisesRegex(companion.CompanionError, "Discover"):
            await self.transport.remote_command(OTHER, "secret", "ver")
        collision = KEY[:12] + "01" * 26
        self.sdk.contacts[collision] = contact(collision)
        with self.assertRaisesRegex(companion.CompanionError, "ambiguous"):
            await self.transport.remote_command(KEY, "secret", "ver")

    async def test_remote_timeout_is_bounded_and_keeps_authenticated_session(self):
        await self.transport.remote_command(KEY, "secret", "ver")
        self.sdk.commands.command_replies = False
        with self.assertRaisesRegex(TimeoutError, "command reply"):
            await self.transport.remote_command(KEY, "secret", "ver")
        self.assertIn(KEY, self.transport._authenticated)
        self.sdk.commands.command_replies = True
        self.assertEqual(await self.transport.remote_command(KEY, "secret", "ver"), "> verified")

    async def test_source_requires_temporary_lease(self):
        for inactive in ("TempRadio inactive", "TempRadio pending: 910,62.5,7,5", "> off"):
            self.sdk.commands.temporary = inactive
            with self.assertRaisesRegex(companion.CompanionError, "TempRadio"):
                await self.transport.serve(b"package", asyncio.Event())
        self.assertEqual(self.sdk.commands.raw, [])

    async def test_only_explicit_expired_auth_renews_login(self):
        await self.transport.remote_command(KEY, "secret", "ver")
        self.sdk.commands.next_reply = "ERR login required"
        self.assertEqual(await self.transport.remote_command(KEY, "secret", "ver"), "> verified")
        self.assertEqual(self.sdk.commands.login_count, 2)
        self.sdk.commands.next_reply = "ERR not permitted"
        self.assertEqual(await self.transport.remote_command(KEY, "secret", "erase"), "ERR not permitted")
        self.assertEqual(self.sdk.commands.login_count, 2)

    async def test_disconnect_has_connection_error_contract(self):
        self.sdk.emit(Types.DISCONNECTED)
        with self.assertRaises(ConnectionError):
            await self.transport.get_radio()
        # Restore fixture connectivity so teardown can restore the saved path.
        self.transport._disconnected = False

    async def test_source_direct_only_with_simultaneous_remote_admin(self):
        stop = asyncio.Event()
        with mock.patch.object(companion, "packet_airtime", return_value=0):
            serving = asyncio.create_task(self.transport.serve(b"package", stop))
            for _ in range(200):
                if self.sdk.commands.raw:
                    break
                await asyncio.sleep(.005)
            self.assertTrue(self.sdk.commands.raw)
            # Actual SDK raw_hex = SNR/RSSI + original RF frame. Its packet
            # parser may have replaced payload, so that field is misleading.
            for raw in (b"\x31\x01\x44\x04request", b"\xf2\0\x04request",
                        b"\x32\0\x04request"):
                self.sdk.emit(Types.RX_LOG_DATA, raw_hex=(b"\x10\xba" + raw).hex(),
                              payload="001122", path_len=0)
            self.assertEqual(await self.transport.remote_command(KEY, "secret", "ota status"), "> verified")
            for _ in range(200):
                if len(self.sdk.commands.raw) >= 2:
                    break
                await asyncio.sleep(.005)
            stop.set()
            result = await asyncio.wait_for(serving, 2)
        self.assertEqual(result["requests"], 1)
        self.assertEqual(len(self.sdk.commands.raw), 2)
        self.assertTrue(all(frame[:2] == b"\x32\0" for frame in self.sdk.commands.raw))
        self.assertEqual(self.sdk.commands.maximum_active, 1)

    async def test_real_package_source_answers_manifest_using_direct_frames(self):
        # Exercise the reused real source, not only the adapter test double.
        from mota_kiss_direct import motalib
        import hashlib
        body = b"real firmware image" * 80
        manifest = motalib.build_manifest(
            target_id=0x12345678, fw_version=0x01120001,
            image_size=len(body), payload=body, block_size=1024,
            image_hash=hashlib.sha256(body).digest(), codec_id=0,
            is_full=True, hw_id="test-radio")
        blob = motalib.build_container(manifest, body)
        self.transport._source_factory = None
        stop = asyncio.Event()
        with mock.patch.object(companion, "packet_airtime", return_value=0):
            serving = asyncio.create_task(self.transport.serve(blob, stop))
            for _ in range(200):
                if self.sdk.commands.raw:
                    break
                await asyncio.sleep(.005)
            request = b"\x32\0\x04" + manifest.merkle_root + b"\xff\xff"
            self.sdk.emit(Types.RX_LOG_DATA, raw_hex=(b"\x10\xba" + request).hex())
            for _ in range(200):
                if len(self.sdk.commands.raw) >= 3:
                    break
                await asyncio.sleep(.005)
            stop.set()
            await asyncio.wait_for(serving, 2)
        self.assertEqual(len(self.sdk.commands.raw), 3)
        self.assertEqual([frame[2] for frame in self.sdk.commands.raw], [1, 5, 5])
        self.assertTrue(all(frame[:2] == b"\x32\0" for frame in self.sdk.commands.raw))


class PureHelpers(unittest.TestCase):
    def test_sdk_rx_shapes_and_malformed_logs(self):
        raw = b"\x32\0\x04request"
        self.assertEqual(companion.raw_rx_bytes({"raw_hex": "10ba" + raw.hex()}), raw)
        self.assertEqual(companion.raw_rx_bytes({"payload": raw.hex()}), raw)
        self.assertEqual(companion.raw_rx_bytes({"raw_hex": "badhex"}), b"")

    def test_airtime_increases_with_sf_and_payload(self):
        self.assertGreater(companion.packet_airtime(180, (910, 62.5, 8, 5)),
                           companion.packet_airtime(180, (910, 62.5, 7, 5)))
        self.assertGreater(companion.packet_airtime(180, (910, 62.5, 7, 5)),
                           companion.packet_airtime(20, (910, 62.5, 7, 5)))


class UsbConstructionTests(unittest.IsolatedAsyncioTestCase):
    async def create_sdk(self, dtr, *, handshake=True):
        import ota_wizard_usb as usb
        history = []

        class Wire:
            def __init__(self, port, baudrate, write_timeout):
                self.port, self.baudrate = port, baudrate
                self.is_open, self.dsrdtr = False, False
                self._dtr = self._rts = True
                history.append(("construct", port))

            @property
            def dtr(self):
                return self._dtr

            @dtr.setter
            def dtr(self, value):
                history.append(("dtr", value, self.is_open))
                self._dtr = value

            @property
            def rts(self):
                return self._rts

            @rts.setter
            def rts(self, value):
                history.append(("rts", value, self.is_open))
                self._rts = value

            def open(self):
                history.append(("open", self.dtr, self.rts))
                self.is_open = True

            def close(self):
                history.append(("close",))
                self.is_open = False

        class SerialConnection:
            def __init__(self, port, baudrate, cx_dly):
                self.port, self.baudrate = port, baudrate
                self.transport = None
                self._connected_event = asyncio.Event()

            class MCSerialClientProtocol:
                def __init__(self, connection):
                    self.connection = connection

                def connection_made(self, transport):
                    self.connection.transport = transport
                    transport.serial.rts = False
                    self.connection._connected_event.set()

        class MeshCore:
            last = None
            def __init__(self, connection, **settings):
                MeshCore.last = self
                self.connection, self.settings = connection, settings

            async def connect(self):
                await self.connection.connect()
                return event() if handshake else None

            async def disconnect(self):
                if self.connection.transport:
                    self.connection.transport.close()
                    self.connection.transport = None

        async def adopt(loop, protocol_factory, wire):
            self.assertTrue(wire.is_open)
            self.assertEqual((wire.dtr, wire.rts), (dtr, False))
            history.append(("adopt", wire.dtr, wire.rts))
            protocol = protocol_factory()
            transport = SimpleNamespace(serial=wire, close=wire.close)
            protocol.connection_made(transport)
            return transport, protocol

        modules = {"meshcore": SimpleNamespace(MeshCore=MeshCore, SerialConnection=SerialConnection),
                   "serial": SimpleNamespace(Serial=Wire),
                   "serial_asyncio_fast": SimpleNamespace(connection_for_serial=adopt)}
        with mock.patch.dict(sys.modules, modules), \
                mock.patch.object(usb, "serial_line_states", return_value=(dtr, False)):
            sdk = await companion._create_usb_sdk("USB-TEST", default_timeout=.1)
        return sdk, MeshCore.last, history

    async def test_native_usb_dtr_is_false_before_sdk_adopts_connection(self):
        sdk, instance, history = await self.create_sdk(False)
        self.assertIs(sdk, instance)
        self.assertLess(history.index(("dtr", False, False)), history.index(("open", False, False)))
        self.assertFalse(any(item[0] == "dtr" and item[1] for item in history))
        self.assertFalse(sdk.settings["auto_reconnect"])
        await sdk.disconnect()
        self.assertEqual(history[-1], ("close",))

    async def test_tinyusb_nrf_dtr_is_true_before_open(self):
        sdk, _, history = await self.create_sdk(True)
        self.assertLess(history.index(("dtr", True, False)), history.index(("open", True, False)))
        await sdk.disconnect()
        self.assertEqual(history[-1], ("close",))

    async def test_failed_binary_handshake_closes_the_configured_port(self):
        sdk, instance, history = await self.create_sdk(False, handshake=False)
        self.assertIsNone(sdk)
        self.assertIsNone(instance.connection.transport)
        self.assertEqual(history[-1], ("close",))


if __name__ == "__main__":
    unittest.main()
