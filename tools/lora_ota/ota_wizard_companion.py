"""One-owner USB Companion transport for the interactive direct OTA updater.

The SDK's Binary connection carries both admin commands and legacy mOTA radio
frames. USB folder mode is deliberately never entered: it excludes Binary
Companion traffic. Imports of the SDK and mOTA implementation are lazy.
"""
from __future__ import annotations

import asyncio
from copy import deepcopy
import hashlib
import math
import re
import secrets
import string
import time
import zlib

import lora_ota as ota


class CompanionError(ota.OtaError):
    pass


def _key(value):
    value = value.hex() if isinstance(value, bytes) else str(value)
    if not re.fullmatch(r"[0-9a-fA-F]{64}", value):
        raise CompanionError("A complete 32-byte public key is required")
    return value.lower()


def _kind(event):
    value = getattr(event, "type", None)
    return getattr(value, "value", value)


def _payload(event):
    value = getattr(event, "payload", {})
    return value if isinstance(value, dict) else {}


def raw_rx_bytes(payload):
    """SDK RX_LOG_DATA.raw_hex includes SNR/RSSI; payload may be parser-modified."""
    try:
        if "raw_hex" in payload:
            raw = payload["raw_hex"]
            raw = bytes.fromhex(raw) if isinstance(raw, str) else bytes(raw)
            return raw[2:] if len(raw) >= 5 else b""
        raw = payload.get("payload", b"")
        return bytes.fromhex(raw) if isinstance(raw, str) else bytes(raw)
    except (TypeError, ValueError):
        return b""


def packet_airtime(length, radio, preamble=16):
    """Conservative explicit-header LoRa airtime, seconds, including CRC."""
    _, bw, sf, cr = radio[:4]
    symbol = 2 ** int(sf) / (float(bw) * 1000)
    de = int(symbol >= .016)
    payload_symbols = 8 + max(0, math.ceil(
        (8 * length - 4 * sf + 28 + 16) / (4 * (sf - 2 * de)))) * cr
    return (preamble + 4.25 + payload_symbols) * symbol


def _deflate(data):
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    return compressor.compress(data) + compressor.flush()


class CompanionTransport:
    def __init__(self, port, baud=115200, *, command_timeout=15,
                 remote_timeout=60, discovery_seconds=4, sdk_factory=None,
                 event_types=None, source_factory=None):
        self.port, self.baud = port, baud
        self.command_timeout = command_timeout
        self.remote_timeout = remote_timeout
        self.discovery_seconds = discovery_seconds
        self._sdk_factory, self._event_types = sdk_factory, event_types
        self._source_factory = source_factory
        self._sdk = None
        self._wire_lock = asyncio.Lock()
        self._admin_lock = asyncio.Lock()
        self._discovery_lock = asyncio.Lock()
        self._subscriptions = []
        self._saved_paths = {}
        self._authenticated = set()
        self._guarded_key = None
        self._discovered = {}
        self._discovery_tag = None
        self._login_waiter = self._reply_waiter = None
        self._used_correlations = set()
        self._timestamps = {}
        self._messages_ready = asyncio.Event()
        self._message_task = None
        self._raw = asyncio.Queue(maxsize=64)
        self._serve_task = None
        self._serve_stop = asyncio.Event()
        self._disconnected = False

    @property
    def self_info(self):
        return dict(getattr(self._sdk, "self_info", {}) or {})

    @property
    def source_key(self):
        value = self.self_info.get("public_key", "")
        try:
            return _key(value)
        except CompanionError:
            return None

    async def reset_remote_session(self, public_key):
        # The receiver loses login sessions on reboot. Preserve the used
        # command correlations and outbound timestamps so delayed replies
        # cannot satisfy a fresh request after authenticating again.
        async with self._admin_lock:
            key = _key(public_key)
            self._authenticated.discard(key)
            if self._guarded_key == key:
                await self._guard_mode(key, 3)
                self._guarded_key = None

    async def open(self):
        if self._sdk is not None:
            return self
        if self._sdk_factory is None:
            try:
                from meshcore import MeshCore, EventType
            except ImportError as exc:
                raise CompanionError("Install the meshcore Python package for USB Companion support") from exc
            self._sdk_factory, self._event_types = MeshCore.create_serial, EventType
        if self._event_types is None:
            raise CompanionError("Companion event definitions are unavailable")
        try:
            self._sdk = await self._sdk_factory(
                self.port, baudrate=self.baud, only_error=True,
                default_timeout=self.command_timeout, auto_reconnect=False)
            if self._sdk is None:
                raise CompanionError("No Binary Companion response on this USB port")
            for name in ("DISCOVER_RESPONSE", "RX_LOG_DATA", "CONTACT_MSG_RECV",
                         "LOGIN_SUCCESS", "LOGIN_FAILED", "MESSAGES_WAITING", "DISCONNECTED"):
                self._subscriptions.append(self._sdk.subscribe(
                    getattr(self._event_types, name), self._on_event))
            await self._call("get_contacts")
            self._disconnected = False
            self._message_task = asyncio.create_task(self._fetch_messages())
            self._messages_ready.set()
            return self
        except BaseException:
            await self.close()
            raise

    async def close(self):
        self._serve_stop.set()
        task = self._serve_task
        if task is not None and task is not asyncio.current_task():
            try:
                await asyncio.wait_for(asyncio.shield(task), 5)
            except (asyncio.TimeoutError, CompanionError, ConnectionError):
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
        if self._message_task is not None:
            self._message_task.cancel()
            await asyncio.gather(self._message_task, return_exceptions=True)
            self._message_task = None
        error = None
        if self._sdk is not None:
            try:
                await self.restore_contact_path()
            except (CompanionError, TimeoutError, ConnectionError) as exc:
                error = exc
            for subscription in self._subscriptions:
                self._sdk.unsubscribe(subscription)
            self._subscriptions.clear()
            await self._sdk.disconnect()
            self._sdk = None
        if error is not None:
            raise error

    async def _call(self, method, *args, **kwargs):
        if self._sdk is None or self._disconnected:
            raise ConnectionError("USB Companion is disconnected")
        function = getattr(self._sdk.commands, method, None)
        if function is None:
            raise CompanionError(f"Installed Companion SDK does not support {method}")
        async with self._wire_lock:
            try:
                result = await asyncio.wait_for(function(*args, **kwargs), self.command_timeout)
            except asyncio.TimeoutError as exc:
                raise TimeoutError(f"USB Companion timed out during {method}") from exc
            except OSError as exc:
                raise ConnectionError("USB Companion connection failed") from exc
        if result is None:
            raise TimeoutError(f"USB Companion did not answer {method}")
        if _kind(result) == "command_error":
            reason = str(_payload(result).get("reason", "")).lower()
            if reason in ("timeout", "no_event_received") or reason.startswith("timeout "):
                raise TimeoutError(f"USB Companion timed out during {method}")
            # Neither arbitrary SDK payloads nor command arguments enter errors.
            raise CompanionError(f"USB Companion rejected {method}")
        return result

    def _on_event(self, event):
        kind, payload = _kind(event), _payload(event)
        if kind == "disconnected":
            self._disconnected = True
            for waiting in (self._login_waiter, self._reply_waiter):
                if waiting is not None and not waiting[-1].done():
                    waiting[-1].set_exception(ConnectionError("USB Companion disconnected"))
        elif kind == "messages_waiting":
            self._messages_ready.set()
        elif kind == "discover_response":
            public_key = str(payload.get("pubkey", "")).lower()
            tag = payload.get("tag")
            if isinstance(tag, bytes):
                tag = tag.hex()
            if (tag != self._discovery_tag or not re.fullmatch(r"[0-9a-f]{64}", public_key)
                    or payload.get("node_type") not in (2, 3)
                    or payload.get("path_len") not in (0, 64, 128)):
                return
            self._discovered[public_key] = dict(
                public_key=public_key, type=payload["node_type"], path="",
                rssi=payload.get("RSSI"), snr=payload.get("SNR"))
        elif kind == "rx_log_data" and self._serve_task is not None:
            raw = raw_rx_bytes(payload)
            if 3 <= len(raw) <= 255 and not self._raw.full():
                self._raw.put_nowait(raw)
        elif kind in ("login_success", "login_failed") and self._login_waiter is not None:
            key, future = self._login_waiter
            if payload.get("pubkey_prefix", "").lower() != key[:12] or future.done():
                return
            if (kind == "login_failed" or not payload.get("is_admin", False)
                    or payload.get("acl_permissions", 0) & 7 != 3):
                future.set_exception(CompanionError("The selected repeater rejected the admin login"))
            else:
                future.set_result(payload)
        elif kind == "contact_message" and self._reply_waiter is not None:
            key, correlation, future = self._reply_waiter
            text = payload.get("text", "")
            if (not future.done() and payload.get("pubkey_prefix", "").lower() == key[:12]
                    and payload.get("txt_type") == 1 and payload.get("path_len") == 255
                    and isinstance(text, str) and text.startswith(correlation)):
                future.set_result(text[len(correlation):])

    async def _fetch_messages(self):
        while True:
            try:
                try:
                    await asyncio.wait_for(self._messages_ready.wait(), 1)
                except asyncio.TimeoutError:
                    if self._reply_waiter is None:
                        continue
                self._messages_ready.clear()
                for _ in range(32):
                    result = await self._call("get_msg", timeout=2)
                    if _kind(result) == "no_more_messages":
                        break
            except (CompanionError, TimeoutError, ConnectionError):
                if self._disconnected:
                    return
                await asyncio.sleep(.2)

    async def discover(self):
        async with self._discovery_lock:
            tag = secrets.randbits(32) or 1
            self._discovery_tag = tag.to_bytes(4, "little").hex()
            self._discovered = {}
            try:
                await self._call("send_node_discover_req", 0x0C, prefix_only=False, tag=tag)
                await asyncio.sleep(self.discovery_seconds)
                contacts = getattr(self._sdk, "contacts", {})
                results = []
                for key, item in self._discovered.items():
                    contact = contacts.get(key, {})
                    results.append(dict(item, name=contact.get("adv_name", key[:12])))
                return sorted(results, key=lambda item: (item["name"], item["public_key"]))
            finally:
                self._discovery_tag = None

    async def _direct_contact(self, public_key):
        key = _key(public_key)
        contacts = getattr(self._sdk, "contacts", {})
        if any(other.lower() != key and other.lower().startswith(key[:12]) for other in contacts):
            raise CompanionError("Contact key prefix is ambiguous; refusing remote administration")
        if key not in self._saved_paths:
            original = deepcopy(contacts.get(key))
            if original is None and key not in self._discovered:
                raise CompanionError("Discover the selected repeater on this direct link first")
            self._saved_paths[key] = original
        contact = deepcopy(contacts.get(key))
        if contact is None:
            discovered = self._discovered[key]
            contact = dict(public_key=key, type=discovered["type"], flags=0,
                           out_path="", out_path_len=0, out_path_hash_mode=0,
                           adv_name=discovered.get("name", key[:12]), last_advert=0,
                           adv_lat=0, adv_lon=0)
        # Reassert the direct path before every operation: background adverts or
        # another client must not silently restore a routed/flood path.
        contact.update(out_path="", out_path_len=0, out_path_hash_mode=0)
        await self._call("update_contact", contact)
        contacts[key] = contact
        return contact

    async def restore_contact_path(self, public_key=None):
        keys = [_key(public_key)] if public_key else list(self._saved_paths)
        if self._guarded_key in keys:
            await self._guard_mode(self._guarded_key, 3)
            self._guarded_key = None
        for key in keys:
            if key not in self._saved_paths:
                continue
            original = self._saved_paths[key]
            contacts = getattr(self._sdk, "contacts", {})
            if original is None:
                await self._call("remove_contact", key)
                contacts.pop(key, None)
            else:
                contact = deepcopy(contacts.get(key, original))
                for field in ("out_path", "out_path_len", "out_path_hash_mode"):
                    contact[field] = original[field]
                await self._call("update_contact", contact)
                contacts[key] = contact
            self._saved_paths.pop(key)
            self._authenticated.discard(key)

    async def local_command(self, text):
        if text.strip().lower() == "get radio":
            return "> " + ",".join(str(value) for value in await self.get_radio())
        result = await self._call("run_cli_command", text)
        reply = _payload(result).get("text")
        if not isinstance(reply, str):
            raise CompanionError("Companion did not return a local CLI reply")
        # Firmware CLI failures are data, just like remote CLI replies. The
        # session layer distinguishes unsupported capabilities from failures;
        # replacing every ERR with a generic message loses that information.
        return reply

    async def get_radio(self):
        result = await self._call("send_appstart")
        info = _payload(result)
        try:
            return (float(info["radio_freq"]), float(info["radio_bw"]),
                    int(info["radio_sf"]), int(info["radio_cr"]))
        except (KeyError, TypeError, ValueError) as exc:
            raise CompanionError("Companion did not report its radio settings") from exc

    async def set_radio(self, radio):
        if len(radio) != 4:
            raise CompanionError("Radio settings require frequency, bandwidth, SF and CR")
        await self._call("set_radio", *radio)
        actual = await self.get_radio()
        if any(abs(float(a) - float(b)) > .002 for a, b in zip(actual, radio)):
            raise CompanionError("Companion radio readback does not match the requested settings")

    async def remote_command(self, public_key, password, text):
        key = _key(public_key)
        if not isinstance(password, str) or len(password.encode("utf-8")) > 15:
            raise CompanionError("Admin passwords must fit in 15 UTF-8 bytes")
        if not isinstance(text, str) or not text or any(c in text for c in "\r\n\0"):
            raise CompanionError("Remote commands must be one nonempty line")
        if len(text.encode("utf-8")) > 155:
            raise CompanionError("Remote command is too long")
        async with self._admin_lock:
            return await self._remote_command(key, password, text)

    async def _remote_command(self, key, password, text, renewed=False):
        # Caller owns the admin lock; a verified explicit login-required reply
        # permits one fresh login. Silence never invalidates authentication.
        contact = await self._direct_contact(key)
        try:
            await self._guard_mode(key, 1)
        except CompanionError as exc:
            raise CompanionError("The USB radio needs current Full Companion firmware with the zero-hop OTA admin guard") from exc
        self._guarded_key = key
        if key not in self._authenticated:
            # The library's send_login_sync accepts any login-success
            # event. Register our own key-filtered future before sending.
            future = asyncio.get_running_loop().create_future()
            self._login_waiter = (key, future)
            try:
                await self._prepare_login_clock(key)
                await self._call("send_login", contact, password)
                await asyncio.wait_for(future, self.remote_timeout)
            except asyncio.TimeoutError as exc:
                raise TimeoutError("Timed out waiting for the selected repeater's admin login") from exc
            finally:
                self._login_waiter = None
                if not future.done():
                    future.cancel()
                elif not future.cancelled():
                    future.exception()
            # A zero-hop PATH teaches this server its empty return route.
            # PATH has lower RF priority than CLI; wait for actual TX drain
            # before a fresh full-key proof, and retry only this read-only proof.
            for attempt in range(3):
                try:
                    await self._teach_return_path(key)
                    proof = await self._command_reply(key, contact, "get public.key")
                    if proof.strip().removeprefix(">").strip().lower() != key:
                        raise CompanionError("The direct destination did not prove its complete public key")
                    break
                except TimeoutError:
                    if attempt == 2:
                        raise
            self._authenticated.add(key)
        reply = await self._command_reply(key, contact, text)
        if (not renewed and re.search(
                r"\b(?:login required|not logged in|authentication required|session expired)\b",
                reply, re.IGNORECASE)):
            self._authenticated.discard(key)
            await self._guard_mode(key, 3)
            self._guarded_key = None
            return await self._remote_command(key, password, text, renewed=True)
        return reply

    async def _prepare_login_clock(self, key):
        now = int(time.time())
        result = await self._call("get_time")
        current = _payload(result).get("time")
        if not isinstance(current, int) or isinstance(current, bool) or not 0 <= current <= 0xFFFFFFFF:
            raise CompanionError("The Companion did not report a valid RTC time before login")
        if current > now + 600:
            raise CompanionError("The Companion clock is over ten minutes ahead; correct it before OTA login")
        if current < now:
            requested = now + 2
            await self._call("set_time", requested)
            result = await self._call("get_time")
            current = _payload(result).get("time")
            if (not isinstance(current, int) or isinstance(current, bool)
                    or not requested <= current <= now + 600):
                raise CompanionError("Could not verify the Companion clock advance before OTA login")
        # sendLogin uses the source RTC, while subsequent CLI packets use host
        # timestamps. Their floor must follow the login, even when the source
        # is a little ahead of the computer or was just advanced above it.
        self._timestamps[key] = max(self._timestamps.get(key, 0), current)

    async def _guard_mode(self, key, mode):
        # Reserved modes of CMD52 are rejected by stock firmware without RF.
        # The public SDK send method preserves the connection's single owner.
        await self._call("send", b"\x34" + bytes([mode]) + bytes.fromhex(key),
                         [self._event_types.OK, self._event_types.ERROR])

    async def _teach_return_path(self, key):
        await self._guard_mode(key, 2)
        deadline = time.monotonic() + self.remote_timeout
        while time.monotonic() < deadline:
            status = await self.local_command("get ota.direct")
            match = re.fullmatch(r"\s*>?\s*ota\.direct v=1 active=1 busy=([01])\s*", status)
            if match is None:
                raise CompanionError("Companion did not confirm the active zero-hop admin guard")
            if match[1] == "0":
                return
            await asyncio.sleep(min(.1, max(.001, self.remote_timeout / 10)))
        raise TimeoutError("Timed out waiting for the direct return path to finish transmitting")

    async def _command_reply(self, key, contact, text):
        alphabet = string.ascii_letters + string.digits
        if len(self._used_correlations) >= len(alphabet) ** 2:
            raise CompanionError("Remote command correlation space exhausted; reconnect")
        while True:
            correlation = "".join(secrets.choice(alphabet) for _ in range(2)) + "|"
            if correlation not in self._used_correlations:
                self._used_correlations.add(correlation)
                break
        future = asyncio.get_running_loop().create_future()
        self._reply_waiter = (key, correlation, future)
        timestamp = max(int(time.time()), self._timestamps.get(key, 0) + 1)
        self._timestamps[key] = timestamp
        try:
            await self._call("send_cmd", contact, correlation + text, timestamp=timestamp)
            self._messages_ready.set()
            reply = await asyncio.wait_for(future, self.remote_timeout)
        except asyncio.TimeoutError as exc:
            raise TimeoutError("Timed out waiting for the selected repeater's command reply") from exc
        finally:
            self._reply_waiter = None
            if not future.done():
                future.cancel()
            elif not future.cancelled():
                future.exception()
        return reply

    async def serve(self, blob, stop_event):
        """Serve one validated package concurrently with Binary admin commands.

        The caller must first open the source's same-tuple TempRadio window.
        CMD65's OK is queue admission only, so send one frame at a time with a
        two-airtime gap rather than enqueueing an entire requested block batch.
        """
        if self._serve_task is not None:
            raise CompanionError("A Companion OTA source is already running")
        try:
            from .mota_kiss_direct import MotaSource, decode_radio
        except ImportError:
            from mota_kiss_direct import MotaSource, decode_radio
        radio = await self.get_radio()
        temporary = await self.local_command("get tempradio")
        if not re.search(r"\bactive\b", temporary, re.IGNORECASE):
            raise CompanionError("Open the source Companion's same-channel TempRadio window before serving")
        preamble_match = re.search(r"\bpreamble=(\d+)\b", temporary)
        preamble = int(preamble_match[1]) if preamble_match else 32
        identity = str(self.self_info.get("public_key", "")).encode()
        seeder_id = hashlib.sha256(identity + hashlib.sha256(blob).digest()).digest()[:4]
        source = (self._source_factory or MotaSource)(blob, seeder_id, compressor=_deflate)
        await asyncio.to_thread(source.prepare)
        self._serve_task = asyncio.current_task()
        self._serve_stop.clear()
        while not self._raw.empty():
            self._raw.get_nowait()
        next_advert = 0.0
        next_transmit = 0.0
        try:
            while not stop_event.is_set() and not self._serve_stop.is_set():
                if self._disconnected:
                    raise ConnectionError("USB Companion disconnected while serving")
                now = time.monotonic()
                if now >= next_advert:
                    replies = [source.advert()]
                    next_advert = now + 30
                else:
                    try:
                        raw = await asyncio.wait_for(self._raw.get(), .2)
                    except asyncio.TimeoutError:
                        continue
                    incoming = decode_radio(raw)
                    if incoming is None or incoming.wide or incoming.observed_path_bytes:
                        continue
                    replies = source.replies(raw)
                for frame in replies:
                    if stop_event.is_set() or self._serve_stop.is_set():
                        break
                    parsed = decode_radio(frame)
                    if parsed is None or parsed.wide or parsed.observed_path_bytes:
                        raise CompanionError("OTA source produced an invalid direct frame")
                    # Even discovery and HAVE are non-forwardable. The reused
                    # legacy source normally floods those frames.
                    direct = b"\x32\0" + parsed.message
                    delay = max(0.0, next_transmit - time.monotonic())
                    while delay > 0 and not (stop_event.is_set() or self._serve_stop.is_set()):
                        await asyncio.sleep(min(delay, .2))
                        delay = max(0.0, next_transmit - time.monotonic())
                    if stop_event.is_set() or self._serve_stop.is_set():
                        break
                    await self._call("send_raw_packet", direct, priority=1)
                    source.note_sent(direct)
                    next_transmit = time.monotonic() + max(.25, 2 * packet_airtime(len(direct), radio, preamble) + .1)
            return dict(source.stats)
        finally:
            self._serve_task = None
