"""Direct OTA transactions shared by the interactive USB transports."""
from __future__ import annotations

import asyncio
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re
import time
from typing import Callable

import lora_ota as ota


@dataclass(frozen=True)
class Destination:
    public_key: str
    target_id: int
    hw_id: str
    base_hash: bytes
    version: int
    version_text: str
    platform: str
    external_storage: bool
    max_block_size: int
    codecs: int
    bootloader: ota.BootloaderStatus | None = None
    boot_version: int | None = None
    sd_storage: bool = False


@dataclass(frozen=True)
class Package:
    blob: bytes
    body_hash: bytes | None = None
    signer: str | None = None

    @property
    def info(self) -> ota.MotaInfo:
        return ota.parse_mota(self.blob)


def cli_value(reply: str) -> str:
    return reply.strip().removeprefix(">").strip()


def temp_state(reply: str) -> str:
    value = cli_value(reply)
    if re.fullmatch(r"(?i)(?:TempRadio\s+)?(?:inactive|off)", value):
        return "off"
    if re.match(r"(?i)^(?:TempRadio\s+)?pending\b", value):
        return "pending"
    if re.match(r"(?i)^TempRadio active:", value):
        return "active"
    radio_tuple(value)
    return "active"


def checked(reply: str) -> str:
    value = cli_value(reply)
    if not value or re.match(r"(?i)^(?:err(?:or)?\b|unknown\b|unsupported\b)", value):
        raise ota.OtaError(value or "The radio returned no reply.")
    return value


def radio_tuple(reply: str) -> tuple[float, float, int, int]:
    values = cli_value(reply).split(",")
    try:
        result = (float(values[0]), float(values[1]), int(values[2]), int(values[3]))
    except (ValueError, IndexError) as exc:
        raise ota.OtaError("Could not read the radio frequency and modulation.") from exc
    ota.parse_temp_radio(",".join(map(str, result)) + ",240")
    return result


def full_package(image: ota.EndFInfo, block_size: int) -> Package:
    """A Pi can package a full image without a Rust or firmware compiler."""
    lib = ota.bootloader_library()
    manifest = lib.build_manifest(
        target_id=image.target_id, fw_version=image.fw_version,
        image_size=len(image.image), payload=image.image,
        block_size=block_size, image_hash=hashlib.sha256(image.image).digest(),
        codec_id=lib.CODEC_FULL, is_full=True, hw_id=image.hw_id)
    blob = lib.build_container(manifest, image.image)
    problems = lib.verify(lib.parse_container(blob))
    if problems:
        raise ota.OtaError("Invalid full package: " + "; ".join(problems))
    return Package(blob, image.body_hash)


def validate_package(package: Package, destination: Destination) -> None:
    info = package.info
    library = ota.bootloader_library()
    try:
        expected_signer = bytes.fromhex(package.signer) if package.signer else None
        parsed = library.parse_container(package.blob)
        if expected_signer is not None and (len(expected_signer) != 32 or not parsed.manifest.is_signed):
            raise ValueError("The claimed signing key requires a signed container")
        problems = library.verify(parsed, expect_pub=expected_signer)
    except (ValueError, TypeError) as exc:
        raise ota.OtaError("Invalid package signing key or container") from exc
    if problems:
        raise ota.OtaError("Package verification failed: " + "; ".join(problems))
    if info.block_size > destination.max_block_size:
        raise ota.OtaError("The package's block size exceeds this receiver's limit.")
    if info.is_bootloader:
        boot = destination.bootloader
        if destination.platform != "nrf52" or not boot or boot.apply_abi < 3:
            raise ota.OtaError("This bootloader cannot update itself over LoRa.")
        if destination.codecs != 0x5:
            raise ota.OtaError("Bootloader self-update requires FULL|INPLACE codecs 0x5.")
        if (info.target_id != boot.target_id or info.hw_id != boot.hw_id
                or info.bootloader_storage != boot.storage_flags):
            raise ota.OtaError("Bootloader package does not match the installed identity and storage.")
        if not package.signer:
            raise ota.OtaError("Bootloader installation needs a pinned trusted signing key.")
        if destination.boot_version is None or info.fw_version <= destination.boot_version:
            raise ota.OtaError("Choose a bootloader newer than the installed version.")
    else:
        if not isinstance(package.body_hash, bytes) or len(package.body_hash) != 8:
            raise ota.OtaError("The application package needs its expected post-install body hash.")
        if info.target_id != destination.target_id or info.hw_id != destination.hw_id:
            raise ota.OtaError("The firmware package is for a different board or role.")
        if info.fw_version <= destination.version:
            raise ota.OtaError("Choose firmware newer than the running version.")
        if not info.is_full and info.base_hash != destination.base_hash:
            raise ota.OtaError("The delta does not match the exact running firmware.")
        if destination.platform == "nrf52":
            if not destination.codecs & (1 << info.codec_id):
                raise ota.OtaError("The installed bootloader does not support this package codec.")
            if info.is_full and not destination.external_storage:
                raise ota.OtaError("Internal-flash nRF52 needs an exact-base in-place delta.")
            if not info.is_full and info.codec_id != ota.MOTA_CODEC_IN_PLACE:
                raise ota.OtaError("nRF52 needs an in-place differential package.")
        elif not info.is_full and info.codec_id != ota.MOTA_CODEC_SEQUENTIAL:
            raise ota.OtaError("ESP32 needs a sequential differential package.")
        if destination.sd_storage and not package.signer:
            raise ota.OtaError("This SD installer requires a trusted signed application package.")


class DirectSession:
    """One target, zero-hop routing, bounded operations, and durable recovery state."""

    def __init__(self, transport, public_key: str, password: str, journal: Path,
                 *, progress: Callable[[str], None] = print, poll_seconds: float = 10,
                 transfer_timeout: float = 4 * 60 * 60, reboot_timeout: float = 300):
        if not re.fullmatch(r"[0-9a-fA-F]{64}", public_key):
            raise ota.OtaError("Choose a destination with a complete public key.")
        self.link, self.key, self.password = transport, public_key.lower(), password
        self.journal, self.progress = Path(journal), progress
        self.poll_seconds, self.transfer_timeout = poll_seconds, transfer_timeout
        self.reboot_timeout = reboot_timeout
        # A run can install a bootloader and then an application. Keep one
        # source lease valid for both complete transfer/reboot budgets, with
        # setup and recovery slack, rather than granting each package four
        # hours against a source window that started only once.
        self.window_minutes = max(15, math.ceil(
            (2 * transfer_timeout + 2 * reboot_timeout + 15 * 60) / 60))
        if self.window_minutes > 10080:
            raise ota.OtaError("The combined update exceeds the supported temporary radio window.")
        self.saved: dict[str, dict[str, str]] = {}
        self.active = False
        self._serve_stop = None
        self._serve_task = None
        self._state: dict = {"format": 1, "target_key": self.key,
                            "source_key": getattr(transport, "source_key", None),
                            "phase": "inspecting"}

    async def remote(self, command: str, *, retries: int = 2) -> str:
        # Only callers of read-only/idempotent commands choose retries. Install
        # and pull are sent once and reconciled from device state if uncertain.
        for attempt in range(retries + 1):
            try:
                return checked(await self.link.remote_command(self.key, self.password, command))
            except (TimeoutError, asyncio.TimeoutError, ConnectionError, ota.TransmissionError) as exc:
                if attempt == retries:
                    raise ota.TransmissionError("No reply from the selected direct destination.") from exc
                await asyncio.sleep(min(1 + attempt, 3))
        raise AssertionError("unreachable")

    async def local(self, command: str) -> str:
        return checked(await self.link.local_command(command))

    def save(self, phase: str, **fields) -> None:
        self._state.update(phase=phase, settings=self.saved, **fields)
        self.journal.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.journal.with_suffix(".tmp")
        temporary.write_text(json.dumps(self._state, indent=2) + "\n", encoding="utf-8")
        temporary.replace(self.journal)

    async def inspect(self) -> Destination:
        identity = re.search(r"\b[0-9A-Fa-f]{64}\b", await self.remote("get public.key"))
        if not identity or identity[0].lower() != self.key:
            raise ota.OtaError("The responding radio did not prove the selected public key.")
        status = await self.remote("ota status")
        self_status = await self.remote("ota self")
        version_text = await self.remote("ver")
        version = ota.extract_reply_version(version_text)
        target = re.search(r"\btarget:([0-9a-fA-F]{8})\b", status)
        hardware = re.search(r"\bhw=([^ |]+)", status)
        body = re.search(r"\bbase_hash=([0-9a-fA-F]{16})\b", self_status)
        if not target or not hardware or not body or version is None:
            raise ota.OtaError("The destination must report valid firmware, hardware, target, and body hash.")
        combined = status + " " + self_status
        nrf = "bootloader:" in combined or bool(re.search(r"\bbl:", combined))
        external = bool(re.search(r"(?:QSPI|SD) apply OK|\bbl:(?:QSPI|SD)\b", combined))
        if re.search(r"(?:QSPI|SD) store:ERR|storage unsafe:|NO .*apply|bl:NONE", combined):
            raise ota.OtaError("The destination's storage or bootloader is not ready for LoRa updates.")
        boot, boot_version, codecs = None, None, 0x7 if not nrf else 0
        if nrf:
            try:
                boot = ota.parse_bootloader_status(await self.remote("ota bootloader status", retries=0))
            except ota.OtaError:
                pass  # Application-only bootloaders can still support firmware OTA.
            try:
                boot_reply = await self.remote("get bootloader.ver", retries=0)
                match = re.search(r"OTAFIX(\d+)\.(\d+)\.(\d+)(?:-preview\.(\d+))?", boot_reply)
                if match:
                    major, minor, patch = map(int, match.groups()[:3])
                    preview = int(match[4]) if match[4] else 255
                    boot_version = (major << 24) | (minor << 16) | (patch << 8) | preview
            except ota.OtaError:
                pass
            caps = re.search(r"abi=(\d+)\s+codecs=0x([0-9a-fA-F]+)", combined)
            if caps and int(caps[1]) >= 3:
                codecs = int(caps[2], 16)
            elif boot and boot.apply_abi >= 3:
                codecs = 0x5
            if not codecs:
                raise ota.OtaError("The installed nRF52 bootloader did not confirm supported OTA codecs.")
        return Destination(self.key, int(target[1], 16), hardware[1], bytes.fromhex(body[1]),
                           version, version_text, "nrf52" if nrf else "esp32", external,
                           ota.parse_target_max_block_size(status, self_status), codecs,
                           boot, boot_version, "SD apply OK" in combined or "bl:SD" in combined)

    async def snapshot(self) -> None:
        source_key = getattr(self.link, "source_key", None)
        if not isinstance(source_key, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", source_key):
            identity = re.search(r"\b[0-9A-Fa-f]{64}\b", await self.local("get public.key"))
            if identity is None:
                raise ota.OtaError("The USB source did not prove its complete public key.")
            source_key = identity[0]
        self._state["source_key"] = source_key.lower()
        for label, command in (("source", self.local), ("target", self.remote)):
            await self._check_temporary_windows(command, label)
            if label == "target":
                await self._require_idle_download()
            config = await command("ota config")
            radio = await command("get radio")
            try:
                rxps = await command("get radio.rxps.config")
            except RuntimeError as exc:
                if "unsupported" in str(exc).lower():
                    rxps = "unsupported"
                else:
                    rxps = await command("get radio.rxps")
            if rxps != "unsupported":
                ota.parse_rxps_settings(rxps, label)
            self.saved[label] = {"radio": radio, "rxps": rxps, "ota": config}
        source, target = (radio_tuple(self.saved[label]["radio"]) for label in ("source", "target"))
        if any(abs(a - b) > .001 for a, b in zip(source, target)):
            raise ota.OtaError("The source and destination must start on the same saved radio settings.")
        self.save("prepared")

    async def _check_temporary_windows(self, command, label: str) -> None:
        if temp_state(await command("get tempradio")) != "off":
            raise ota.OtaError(f"The {label} already has a temporary radio window. Finish it before updating.")
        # Older supported repeaters report TempRadio off until a scheduled
        # tuple starts. A new lease cancels even their future temp schedules.
        try:
            schedules = cli_value(await command("get tempradioat"))
        except ota.TransmissionError:
            raise
        except ota.OtaError as exc:
            if re.match(r"(?i)^(?:(?:err(?:or)?)[ :.-]*)?(?:unknown(?: command| config)?|unsupported|not supported)\b", str(exc)):
                return  # Companion and Room roles have no primary scheduler.
            raise
        if schedules.lower() not in ("-none-", "none", "off") and not re.fullmatch(r"(?i)slots:\s*", schedules):
            raise ota.OtaError(f"The {label} has scheduled temporary radio settings. Finish them before updating.")

    async def _wait_window(self, command) -> None:
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            reply = temp_state(await command("get tempradio"))
            if reply.lower() not in ("pending", "off"):
                return
            await asyncio.sleep(1)
        raise ota.OtaError("The temporary OTA window did not become active.")

    async def begin(self) -> None:
        if not self.saved:
            await self.snapshot()
        else:
            # Package preparation can take time. Recheck both radios before
            # the first mutation rather than replacing a newly queued lease.
            for label, command in (("source", self.local), ("target", self.remote)):
                await self._check_temporary_windows(command, label)
            await self._require_idle_download()
        self.active = True
        self.save("opening OTA windows")
        for label, command in (("target", self.remote), ("source", self.local)):
            commands = ["ota config hops 0"]
            if "mode=seeder-only" not in self.saved[label]["ota"]:
                commands[:0] = ["ota config autofetch off", "ota config autoinstall off"]
            if self.saved[label]["rxps"] != "unsupported":
                commands.append("set radio.rxps off")
            for text in commands:
                await command(text)
            await self._verify_paused_policy(command, label)
            if self.saved[label]["rxps"] != "unsupported":
                current = await self._read_rxps(command, label)
                if current.enabled:
                    raise ota.OtaError(f"The {label} did not disable RX power saving.")
            # Keep the existing RF tuple so both admin and file traffic stay
            # reachable even if the process is interrupted before cleanup.
            await self._open_window(label)
        self.save("OTA windows active")

    async def _open_window(self, label: str) -> None:
        command = self.remote if label == "target" else self.local
        values = radio_tuple(self.saved[label]["radio"])
        text = "tempradio " + ",".join(map(str, values)) + f",{self.window_minutes}"
        try:
            if label == "target":
                await self.remote(text, retries=0)
            else:
                await self.local(text)
        except (TimeoutError, ConnectionError, ota.TransmissionError):
            # Relative leases must not be replayed after a lost acknowledgement.
            # Both entry points have already proved the old window inactive.
            pass
        await self._wait_window(command)

    async def _stop_serving(self) -> None:
        if self._serve_task:
            self._serve_stop.set()
            try:
                await asyncio.wait_for(self._serve_task, timeout=15)
            except (asyncio.TimeoutError, asyncio.CancelledError):
                self._serve_task.cancel()
                await asyncio.gather(self._serve_task, return_exceptions=True)
            finally:
                self._serve_task = self._serve_stop = None

    async def _start_serving(self, package: Package) -> None:
        await self._stop_serving()
        self._serve_stop = asyncio.Event()
        self._serve_task = asyncio.create_task(self.link.serve(package.blob, self._serve_stop))
        await asyncio.sleep(.2)
        if self._serve_task.done():
            await self._serve_task
            raise ota.OtaError("The file server stopped before starting the transfer.")

    async def _ready(self, info: ota.MotaInfo) -> None:
        deadline = time.monotonic() + self.transfer_timeout
        last = None
        while time.monotonic() < deadline:
            if self._serve_task.done():
                await self._serve_task
                raise ota.OtaError("The USB file server stopped during the transfer.")
            status, complete = await self._download_state(info)
            if status != last:
                self.progress(status)
                last = status
            if complete:
                return
            await asyncio.sleep(self.poll_seconds)
        raise ota.OtaError("The transfer timed out. The recovery record retains the package ID.")

    async def _download_state(self, info: ota.MotaInfo) -> tuple[str, bool]:
        state, mid, status = await self._read_download()
        if mid is None:
            raise ota.OtaError("The destination did not confirm a live download and manifest ID.")
        if mid != info.manifest_id.lower():
            raise ota.OtaError("The destination's download is not the selected package.")
        if state == "failed":
            raise ota.OtaError("The destination reported a failed download: " + status)
        return status, state == "complete"

    async def _read_download(self) -> tuple[str, str | None, str]:
        status = await self.remote("ota status")
        if re.search(r"\bno download\b", status, re.I):
            return "idle", None, status
        download = re.search(r"\bdownload:\s*([^|]+)", status, re.I)
        section = download[1] if download else ""
        mid = re.search(r"\bid=([0-9A-Fa-f]{8})\b", section)
        if mid is None:
            # Long hardware names can truncate the status reply before id=.
            # ota stats also has a self id=; only inspect its FETCH section.
            stats = await self.remote("ota stats")
            fetch = re.search(r"\bfetch\s+([^|]+)", stats, re.I)
            section = fetch[1] if fetch else ""
            if re.match(r"idle\b", section, re.I):
                return "idle", None, status + " | " + stats
            mid = re.search(r"\bid=([0-9A-Fa-f]{8})\b", section)
            status += " | " + stats
        if mid is None:
            raise ota.OtaError("The destination did not confirm a live download and manifest ID.")
        state = ("failed" if re.match(r"failed\b", section, re.I) else
                 "complete" if re.match(r"(?:ready to install|done)\b", section, re.I) else "active")
        return state, mid[1].lower(), status

    async def _require_idle_download(self) -> None:
        state, mid, _ = await self._read_download()
        if state != "idle":
            raise ota.OtaError(
                f"The destination already has OTA download {mid or 'unknown'}. "
                "Finish or cancel that update before starting another one.")

    async def _installed(self, package: Package, before: Destination) -> Destination:
        info, deadline = package.info, time.monotonic() + self.reboot_timeout
        while time.monotonic() < deadline:
            try:
                reset_session = getattr(self.link, "reset_remote_session", None)
                if reset_session:
                    await reset_session(self.key)
                current = await self.inspect()
                if info.is_bootloader:
                    if current.boot_version == info.fw_version:
                        if (current.target_id != before.target_id or current.base_hash != before.base_hash
                                or current.hw_id != before.hw_id
                                or not current.bootloader or not before.bootloader
                                or (current.bootloader.board_id, current.bootloader.device_name,
                                    current.bootloader.storage_flags) !=
                                   (before.bootloader.board_id, before.bootloader.device_name,
                                    before.bootloader.storage_flags)):
                            raise ota.OtaError("The application changed during the bootloader-only update.")
                        return current
                elif (current.version == info.fw_version and package.body_hash == current.base_hash
                      and current.target_id == info.target_id and current.hw_id == info.hw_id):
                    return current
            except (ota.OtaError, TimeoutError, ConnectionError):
                pass
            await asyncio.sleep(self.poll_seconds)
        raise ota.OtaError("Could not confirm the installed version and body hash after reboot.")

    async def install(self, package: Package, before: Destination) -> Destination:
        validate_package(package, before)
        info = package.info
        if not self.active:
            await self.begin()
        # A bootloader reboot clears the destination's RAM-only lease.
        if temp_state(await self.remote("get tempradio")) == "off":
            await self._open_window("target")
        await self._require_idle_download()
        self.save("preparing package", package_mid=info.manifest_id, package_hash=info.image_hash.hex(),
                  package_kind=info.kind, package_bootloader=info.is_bootloader,
                  package_version=info.fw_version, package_target=info.target_id, package_hw=info.hw_id,
                  package_storage=info.bootloader_storage,
                  expected_body=package.body_hash.hex() if package.body_hash else None,
                  previous_app=dict(target=before.target_id, hw=before.hw_id,
                                    version=before.version, body=before.base_hash.hex()),
                  pull_attempted=False, install_attempted=False,
                  package_finalized=False, cancel_sent=False, cancel_confirmed=False)
        if package.signer:
            await self.remote("ota key add " + package.signer)
        if not info.is_bootloader and not info.is_full:
            live = await self.inspect()
            if live.base_hash != info.base_hash:
                raise ota.OtaError("The destination's firmware changed after delta preparation.")
        await self._start_serving(package)
        self.save("serving package")
        # Discovery may need more than one advertisement. It is read-only and
        # always bound to the exact selected MID before a pull is issued.
        for _ in range(12):
            if info.manifest_id.lower() in (await self.remote("ota ls")).lower():
                break
            await asyncio.sleep(self.poll_seconds)
        else:
            raise ota.OtaError("The selected package was not discovered over the direct link.")
        await self._require_idle_download()
        self.save("downloading", pull_attempted=True)
        try:
            await self.remote(f"ota pull {info.manifest_id} flash", retries=0)
        except ota.TransmissionError:
            await self._download_state(info)
        except ota.OtaError:
            # An explicit rejection did not grant us ownership of somebody
            # else's slot; cleanup must not cancel that download.
            self.save("pull rejected", pull_attempted=False)
            raise
        await self._ready(info)
        if info.is_bootloader:
            staged = await self.remote("ota bootloader status")
            expected = f"staged:ready mid={info.manifest_id} hash={info.image_hash[:8].hex()}"
            if expected.lower() not in staged.lower():
                raise ota.OtaError("The staged bootloader MID/hash does not match the verified package.")
            command = f"ota bootloader install {info.manifest_id} {info.image_hash[:8].hex().upper()}"
        else:
            command = "ota install"
        self.save("installing", install_attempted=True)
        try:
            await self.remote(command, retries=0)
        except ota.TransmissionError:
            self.progress("The install reply was lost; checking the running image before taking further action.")
        current = await self._installed(package, before)
        await self._stop_serving()
        self.save("installed", package_finalized=True, install_verified=True)
        return current

    async def _verify_paused_policy(self, command, label: str) -> None:
        policy = await command("ota config")
        fields = ("hops",) if "mode=seeder-only" in policy else ("hops", "autofetch", "autoinstall")
        for field in fields:
            match = re.search(rf"\b{field}=([A-Za-z0-9]+)", policy)
            expected = "0" if field == "hops" else "off"
            if match is None or match[1] != expected:
                raise ota.OtaError(f"The {label} did not confirm isolated OTA policy {field}={expected}.")

    def _expected_install_matches(self, current: Destination) -> bool:
        state = self._state
        if state.get("package_bootloader"):
            previous = state.get("previous_app", {})
            boot = current.bootloader
            return (current.boot_version == state.get("package_version") and boot is not None
                    and boot.target_id == state.get("package_target")
                    and boot.hw_id == state.get("package_hw")
                    and boot.storage_flags == state.get("package_storage")
                    and current.target_id == previous.get("target")
                    and current.hw_id == previous.get("hw")
                    and current.version == previous.get("version")
                    and current.base_hash.hex() == previous.get("body"))
        return (current.target_id == state.get("package_target")
                and current.hw_id == state.get("package_hw")
                and current.version == state.get("package_version")
                and current.base_hash.hex() == state.get("expected_body"))

    async def _finalize_download(self) -> bool:
        """Free only this run's receive slot, before restoring automatic policy."""
        state = self._state
        if state.get("package_finalized") is True:
            return True
        if type(state.get("pull_attempted")) is not bool:
            # Old journals recorded only a phase. Normalize it durably before
            # cleanup can replace that phase with "needs cleanup"; otherwise
            # the next recovery could forget an unresolved pull/install.
            phase = state.get("phase")
            owned = phase in ("downloading", "installing", "installed") or (
                bool(state.get("package_mid")) and phase not in (
                    "inspecting", "prepared", "opening OTA windows", "OTA windows active",
                    "preparing package", "serving package", "pull rejected"))
            self.save(phase or "needs cleanup", pull_attempted=owned,
                      install_attempted=owned and phase != "downloading",
                      cancel_sent=(state.get("cancel_sent") is True or
                                   "cancel_sent" in state and type(state["cancel_sent"]) is not bool),
                      cancel_confirmed=state.get("cancel_confirmed") is True,
                      package_finalized=False)
        attempted = state.get("pull_attempted") is True
        if not attempted:
            return True
        if type(state.get("install_attempted")) is not bool:
            # A partial or malformed journal must not turn a possibly armed
            # install into a cancelable download when cleanup changes phase.
            self.save(state.get("phase", "needs cleanup"), install_attempted=True)
        if type(state.get("cancel_sent")) is not bool:
            self.save(state.get("phase", "needs cleanup"), cancel_sent=True)
        if type(state.get("cancel_confirmed")) is not bool:
            self.save(state.get("phase", "needs cleanup"), cancel_confirmed=False)
        mid = state.get("package_mid")
        reset_session = getattr(self.link, "reset_remote_session", None)
        if reset_session:
            await reset_session(self.key)
        # A canceled COMPLETE signed fetch must never become eligible merely
        # because we restored its saved autoinstall=trusted setting.
        await self.remote("ota config autoinstall off")
        await self.remote("ota config autofetch off")
        policy = await self.remote("ota config")
        if not all(re.search(rf"\b{field}=off\b", policy) for field in ("autoinstall", "autofetch")):
            raise ota.OtaError("The destination did not confirm automatic OTA policies are off.")
        if not isinstance(mid, str) or not re.fullmatch(r"[0-9a-fA-F]{8}", mid):
            raise ota.OtaError("The unfinished update has no valid package ID; automatic OTA policies remain off.")
        if state.get("install_attempted") is True or state.get("phase") == "installing":
            # An install may already be armed even if its acknowledgement was
            # lost. Never erase that slot; first prove the resulting image.
            current = await self.inspect()
            if self._expected_install_matches(current):
                self.save("installed", package_finalized=True, install_verified=True)
                return True
            raise ota.OtaError("Installation outcome is unresolved; automatic OTA policies remain off.")
        live_state, live_mid, _ = await self._read_download()
        if live_state == "idle":
            if state.get("cancel_confirmed") is True:
                return True
            # IDLE alone says nothing about persistent media invalidation.
            # Do not replay cancel against an unowned/unknown staged slot.
            raise ota.OtaError("The live download is idle, but persistent slot clearance is unconfirmed; automatic OTA policies remain off.")
        if live_mid != mid.lower():
            raise ota.OtaError("The destination now has a different download; it was not canceled and automatic OTA policies remain off.")
        if state.get("cancel_sent") is True:
            raise ota.OtaError("The previous cancellation is unconfirmed; it was not replayed and automatic OTA policies remain off.")
        self.save(state.get("phase", "downloading"), cancel_sent=True)
        reply = None
        try:
            reply = await self.remote("ota cancel", retries=0)
        except (ota.OtaError, TimeoutError, ConnectionError):
            pass  # Read back once; never blindly repeat a destructive command.
        live_state, _, _ = await self._read_download()
        confirmed = (reply is not None and live_state == "idle"
                     and re.match(r"(?i)^OK dropped session\b", reply)
                     and re.search(rf"(?i)\bmid={re.escape(mid)}\b", reply)
                     and "OTA receive slot confirmed clear" in reply)
        if not confirmed:
            raise ota.OtaError("Cancellation or persistent slot clearance is unconfirmed; automatic OTA policies remain off.")
        self.save(state.get("phase", "downloading"), cancel_confirmed=True, package_finalized=True)
        return True

    async def cleanup(self) -> list[str]:
        errors = []
        server_stopped = True
        try:
            await self._stop_serving()
        except Exception:
            server_stopped = False
            errors.append("Stop the USB file server before restoring settings.")
        target_policy_safe = True
        if self.active:
            try:
                target_policy_safe = await self._finalize_download()
            except Exception as exc:
                target_policy_safe = False
                errors.append(f"Resolve the recorded target update before restoring automatic OTA policy: {exc}")
            if not server_stopped:
                target_policy_safe = False
                try:
                    await self.remote("ota config autoinstall off")
                    await self.remote("ota config autofetch off")
                except Exception:
                    errors.append("Confirm both target automatic OTA policies are off while its source remains unconfirmed.")
        if self.active:
            for label, command in (("target", self.remote), ("source", self.local)):
                if label not in self.saved:
                    continue
                settings = self.saved[label]
                commands = ["normalradio"]
                if settings["rxps"] != "unsupported":
                    try:
                        rxps = ota.parse_rxps_settings(settings["rxps"], label)
                        current = await self._read_rxps(command, label)
                        if current != rxps:
                            commands.append(ota.rxps_restore_command(rxps))
                            if not rxps.enabled:
                                commands.append("set radio.rxps off")
                    except Exception:
                        errors.append(f"Restore the recorded {label} RXPS settings from {self.journal}.")
                for key in ("hops", "autofetch", "autoinstall"):
                    if label == "target" and key != "hops" and not target_policy_safe:
                        continue
                    if key != "hops" and "mode=seeder-only" in settings["ota"]:
                        continue
                    match = re.search(rf"\b{key}=([A-Za-z0-9]+)", settings["ota"])
                    if match:
                        commands.append(f"ota config {key} {match[1]}")
                for text in commands:
                    try:
                        await command(text)
                    except Exception:
                        errors.append(f"{label}: {text}")
                try:
                    await self._verify_restored(command, label, settings,
                                                restore_auto=label != "target" or target_policy_safe)
                except Exception:
                    errors.append(f"Verify and restore the recorded {label} radio/RXPS/OTA settings from {self.journal}.")
        restore_path = getattr(self.link, "restore_contact_path", None)
        if restore_path:
            try:
                await restore_path()
            except Exception:
                errors.append("Restore the Companion's recorded destination contact route.")
        self.save("needs cleanup" if errors else "finished", recovery_commands=errors)
        self.active = bool(errors)
        return errors

    async def _read_rxps(self, command, label: str) -> ota.RxpsSettings:
        try:
            reply = await command("get radio.rxps.config")
        except ota.OtaError as exc:
            if not re.search(r"unknown|unrecognized|not supported", str(exc), re.I):
                raise
            reply = await command("get radio.rxps")
        return ota.parse_rxps_settings(reply, label)

    async def _verify_restored(self, command, label: str, settings: dict,
                               *, restore_auto: bool = True) -> None:
        deadline = time.monotonic() + 45
        while temp_state(await command("get tempradio")) != "off":
            if time.monotonic() >= deadline:
                raise ota.OtaError(f"The {label} did not return to normal radio.")
            await asyncio.sleep(1)
        current_radio = radio_tuple(await command("get radio"))
        if any(abs(a - b) > .001 for a, b in zip(current_radio, radio_tuple(settings["radio"]))):
            raise ota.OtaError(f"The {label} radio settings changed.")
        if settings["rxps"] != "unsupported":
            if await self._read_rxps(command, label) != ota.parse_rxps_settings(settings["rxps"], label):
                raise ota.OtaError(f"The {label} RXPS settings did not restore exactly.")
        actual = await command("ota config")
        for key in ("hops", "autofetch", "autoinstall"):
            if not restore_auto and key != "hops":
                if not re.search(rf"\b{key}=off\b", actual):
                    raise ota.OtaError(f"The {label} unsafe automatic OTA policy was not kept off.")
                continue
            previous = re.search(rf"\b{key}=([A-Za-z0-9]+)", settings["ota"])
            current = re.search(rf"\b{key}=([A-Za-z0-9]+)", actual)
            if previous is None or current is None or previous[1] != current[1]:
                raise ota.OtaError(f"The {label} OTA policy did not restore exactly.")
