#!/usr/bin/env python3
"""Interactive, direct LoRa updates through one USB repeater or Full Companion."""
from __future__ import annotations

import argparse
import asyncio
import getpass
import importlib.util
import importlib.metadata
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import venv

import lora_ota as ota
from ota_wizard_catalog import GitHubCatalog, ImageCache, download_asset, load_application
from ota_wizard_packages import (find_builder, find_running_image, install_builder,
                                 prepare_application, prepare_bootloader, small_host)
from ota_wizard_session import DirectSession
from ota_wizard_bundle import (load_bundle, load_ready_application, load_request,
                               save_bundle, save_request)
from ota_wizard_usb import open_serial_session

ROOT = Path(__file__).resolve().parents[2]


def cache_directory() -> Path:
    if os.name == "nt":
        return Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local")) / "MeshCore/ota"
    if sys.platform == "darwin":
        return Path.home() / "Library/Caches/MeshCore/ota"
    return Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "meshcore/ota"


def ask(prompt: str, default: str | None = None) -> str:
    if not sys.stdin.isatty():
        raise ota.OtaError("This choice needs an interactive terminal; supply the corresponding command-line option.")
    value = input(prompt + (f" [{default}]" if default is not None else "") + ": ").strip()
    return value or (default if default is not None else "")


def select(prompt: str, choices: list[str], default: int = 0) -> int:
    for index, value in enumerate(choices, 1):
        print(f"  {index}. {value}")
    while True:
        value = ask(prompt, str(default + 1))
        if value.isdigit() and 1 <= int(value) <= len(choices):
            return int(value) - 1
        print("Enter one of the numbers above.")


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     epilog="No passwords are saved. All updater transmissions use a direct, zero-hop route.")
    parser.add_argument("--port", help="USB port; selected automatically when only one radio is connected")
    parser.add_argument("--mode", choices=("auto", "repeater", "companion"), default="auto")
    parser.add_argument("--target", help="complete destination public key; otherwise discover nearby repeaters")
    parser.add_argument("--firmware", type=Path, help="custom application DFU ZIP, BIN, HEX, full mOTA, or prepared OTA bundle")
    parser.add_argument("--base", type=Path, help="original running firmware, checked against the destination hash")
    parser.add_argument("--full", action="store_true", help="use a full update when the destination supports it")
    parser.add_argument("--build", action="store_true", help="build custom Full firmware from this source checkout")
    parser.add_argument("--source-tree", type=Path, default=ROOT)
    parser.add_argument("--version", help="version for a custom build; default is newer than the running version")
    parser.add_argument("--motatool", help="existing differential builder executable")
    parser.add_argument("--cache", type=Path, default=cache_directory())
    parser.add_argument("--no-bootloader", action="store_true", help="update only application firmware")
    parser.add_argument("--no-setup", action="store_true", help="do not create a private Python dependency environment")
    parser.add_argument("--recover", action="store_true", help="restore settings from an interrupted update, then exit")
    parser.add_argument("--yes", action="store_true", help="accept the displayed update plan; does not guess missing credentials")
    parser.add_argument("--runtime-ready", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--check", action="store_true", help="inspect source and destination without preparing or installing an update")
    parser.add_argument("--prepare-only", action="store_true", help="prepare a portable update bundle on a computer without opening radios")
    parser.add_argument("--request", type=Path, help="destination preparation request saved by a previous live inspection")
    parser.add_argument("--output", type=Path, help="output .zip for --prepare-only")
    return parser.parse_args(argv)


def ensure_dependencies(args, argv) -> None:
    required = ("serial", "meshcore", "cryptography")
    missing = [name for name in required if importlib.util.find_spec(name) is None]
    if "meshcore" not in missing:
        try:
            if importlib.metadata.version("meshcore") != "2.3.9.1":
                missing.append("tested meshcore version")
        except importlib.metadata.PackageNotFoundError:
            missing.append("meshcore package metadata")
    if not missing:
        return
    if args.no_setup or args.runtime_ready:
        raise ota.OtaError("Install the USB updater dependencies: python -m pip install -r "
                           + str(Path(__file__).with_name("requirements-wizard.txt")))
    print("First run: preparing USB and update-verification support in a private Python environment.")
    environment = args.cache / "python" / f"{sys.version_info.major}.{sys.version_info.minor}"
    python = environment / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    if not python.is_file():
        try:
            venv.EnvBuilder(with_pip=True).create(environment)
        except Exception as exc:
            raise ota.OtaError("Python's venv support is required. On Raspberry Pi OS, install python3-venv.") from exc
    log = environment / "setup.log"
    with log.open("w", encoding="utf-8") as output:
        completed = subprocess.run([str(python), "-m", "pip", "install", "--disable-pip-version-check",
                                    "--only-binary=:all:", "-r",
                                    str(Path(__file__).with_name("requirements-wizard.txt"))],
                                   stdout=output, stderr=subprocess.STDOUT, timeout=600)
    if completed.returncode:
        raise ota.OtaError(f"USB support could not be installed without compiling dependencies. Details: {log}")
    os.execv(str(python), [str(python), str(Path(__file__).resolve()), "--runtime-ready", *argv])


def choose_port(explicit: str | None, remembered: dict | None = None) -> str:
    if explicit:
        return explicit
    from serial.tools import list_ports
    ports = [port for port in list_ports.comports() if port.vid is not None]
    if not ports:
        raise ota.OtaError("Connect the source repeater or Full Companion by USB, then run this command again.")
    if len(ports) == 1:
        return ports[0].device
    default_port = next((index for index, port in enumerate(ports)
                         if port.device == (remembered or {}).get("port")), 0)
    index = select("USB radio", [f"{port.description} — {port.device}" for port in ports], default_port)
    return ports[index].device


def remembered_choices(cache: Path) -> dict:
    try:
        value = json.loads(ota.read_bounded_file(cache / "choices.json", 8192, "remembered choices"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError, ota.OtaError):
        return {}


def detect_mode(port: str) -> str:
    """A harmless text probe avoids feeding Binary commands to a repeater CLI."""
    import serial
    connection = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=2)
    try:
        open_serial_session(connection, port)
        time.sleep(.75)
        connection.write(b"\rver\rota status\r")
        deadline = time.monotonic() + 3
        data = bytearray()
        while time.monotonic() < deadline and len(data) < 16384:
            data.extend(connection.read(1024))
        text = data.decode("utf-8", "replace")
        if "companion" in text.lower() or "ota seeder" in text.lower():
            return "companion"
        if re.search(r"(?m)(?:->\s*|^\s*>?\s*)v\d+\.\d+", text):
            return "repeater"
        return "companion"  # A Binary Companion does not answer text probes.
    finally:
        connection.close()


def save_preferences(path: Path, **values) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(values, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def user_progress(message: str) -> None:
    """Keep raw wire diagnostics in recovery records, not the normal UI."""
    if message.startswith("OTA "):
        count = re.search(r"\b(\d+)/(\d+)\b", message)
        if "ready to install" in message.lower():
            print("Download complete; checking the package before installation.")
        elif count:
            have, total = map(int, count.groups())
            print(f"Downloading: {100 * have // total if total else 0}% ({have}/{total} blocks)")
        else:
            print("Waiting for the destination's download status…")
    else:
        print(message)


def custom_build(args, destination, target_env: str) -> Path:
    source = args.source_tree.resolve()
    if not (source / "build.sh").is_file():
        raise ota.OtaError("Custom builds need a MeshCore source checkout; set --source-tree to that directory.")
    if not re.fullmatch(r"[A-Za-z0-9_]+", target_env):
        raise ota.OtaError("The detected firmware target cannot be used as a build environment.")
    version = args.version or ota.format_version(destination.version + 1) + "-custom"
    packed_version = ota.extract_reply_version(version)
    if packed_version is None or packed_version <= destination.version:
        raise ota.OtaError("The custom build version must be newer than the running version.")
    output = args.cache.resolve() / "custom" / (time.strftime("%Y%m%d-%H%M%S") + "-" + target_env)
    output.mkdir(parents=True, exist_ok=False)
    # build.sh holds the repository's build lock and runs one PlatformIO process.
    command = ["bash", "build.sh", "build-firmware", target_env,
               "--build-profile", "full", "--radio-preset", "target",
               "--profile", "cascade", "--firmware-version", version]
    if destination.platform == "esp32":
        command += ["--full-exact"]
    environment = {key: value for key, value in os.environ.items()
                   if key not in ("MESHCORE_ADMIN_PASSWORD", "MESHCORE_OTA_PASSWORD")}
    environment["OUTPUT_DIR"] = output.as_posix()
    if os.name == "nt" and shutil.which("wsl.exe"):
        def linux_path(path: Path) -> str:
            mapped = subprocess.run(["wsl.exe", "-e", "wslpath", "-a", str(path)],
                                    capture_output=True, text=True, timeout=15)
            if mapped.returncode or not mapped.stdout.strip().startswith("/"):
                raise ota.OtaError("Custom builds need a working WSL installation. Run this updater inside WSL, or choose a firmware file.")
            return mapped.stdout.strip()
        command = ["wsl.exe", "--cd", linux_path(source), "--exec", "env",
                   "-u", "MESHCORE_ADMIN_PASSWORD", "-u", "MESHCORE_OTA_PASSWORD",
                   "OUTPUT_DIR=" + linux_path(output), *command]
    elif not shutil.which("bash"):
        raise ota.OtaError("Custom builds need Bash and the firmware build tools. Use a published or local firmware file instead.")
    log = output / "build.log"
    print(f"Building custom Full firmware. This may download board tools; progress is saved to {log}.")
    with log.open("w", encoding="utf-8") as stream:
        completed = subprocess.run(command, cwd=source, env=environment,
                                   stdout=stream, stderr=subprocess.STDOUT, timeout=7200)
    if completed.returncode:
        raise ota.OtaError(f"The custom build failed; its full log is in {log}.")
    candidates = list(output.glob("*.zip")) + [path for path in output.glob("*.bin") if "merged" not in path.name]
    for candidate in candidates:
        try:
            load_application(candidate, target_id=destination.target_id, hw_id=destination.hw_id)
            return candidate
        except ota.OtaError:
            continue
    raise ota.OtaError("The custom build did not produce a matching application image.")


async def run(args) -> int:
    args.cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    if args.target and not re.fullmatch(r"[0-9A-Fa-f]{64}", args.target):
        raise ota.OtaError("--target needs the complete destination public key.")
    remembered = remembered_choices(args.cache)
    port = choose_port(args.port, remembered)
    mode = args.mode if args.mode != "auto" else await asyncio.to_thread(detect_mode, port)
    if mode == "companion":
        from ota_wizard_companion import CompanionTransport
        link = CompanionTransport(port)
    else:
        from ota_wizard_repeater import RepeaterTransport
        link = RepeaterTransport(port)
    print(f"Connecting to the USB {mode} on {port}…")
    await link.open()
    session = None
    try:
        key = args.target
        if not args.recover:
            print("Looking for directly reachable repeaters and rooms…")
            nearby = await link.discover()
            if not nearby:
                raise ota.OtaError("No direct destinations replied. Check both radios use the same channel, then retry.")
            if key:
                if not any(item["public_key"].lower() == key.lower() for item in nearby):
                    raise ota.OtaError("The selected public key did not answer direct discovery.")
            else:
                default_target = next((index for index, item in enumerate(nearby)
                                       if item["public_key"].lower() == remembered.get("target")), 0)
                selected = 0 if len(nearby) == 1 else select("Destination", [
                    f"{'Room' if item.get('type') == 3 else 'Repeater'} — {item['public_key'][:12].upper()}"
                    for item in nearby], default_target)
                key = nearby[selected]["public_key"]
        elif not key:
            key = remembered.get("target") or ask("Destination public key from the recovery record")
        print(f"Destination: {key[:12].upper()} (zero-hop direct)")
        password = os.environ.get("MESHCORE_ADMIN_PASSWORD")
        if password is None:
            if not sys.stdin.isatty():
                raise ota.OtaError("Set MESHCORE_ADMIN_PASSWORD for a noninteractive run.")
            password = getpass.getpass("Destination admin password [Enter for default]: ") or "password"
        journal = args.cache / "sessions" / (key.lower() + ".json")
        previous = None
        if journal.is_file():
            previous = json.loads(ota.read_bounded_file(journal, 64 * 1024, "recovery record"))
            if not isinstance(previous, dict) or previous.get("format") != 1:
                raise ota.OtaError("The interrupted update record has an unsupported format.")
            if previous.get("phase") not in ("finished", "prepared", "inspecting"):
                source_key = previous.get("source_key")
                if (previous.get("target_key") != key.lower() or not isinstance(source_key, str)
                        or not isinstance(link.source_key, str) or source_key.lower() != link.source_key.lower()):
                    raise ota.OtaError("The unfinished recovery record belongs to a different source or destination.")
                saved = previous.get("settings")
                if not isinstance(saved, dict) or set(saved) != {"source", "target"}:
                    raise ota.OtaError("The interrupted update record has incomplete saved settings.")
                for settings in saved.values():
                    if (not isinstance(settings, dict) or not all(
                            isinstance(settings.get(field), str) and len(settings[field]) <= 2048
                            for field in ("radio", "rxps", "ota"))):
                        raise ota.OtaError("The interrupted update record has invalid saved settings.")
        session = DirectSession(link, key, password, journal, progress=user_progress)
        if previous and previous.get("phase") not in ("finished", "prepared", "inspecting"):
            print("Restoring the settings saved by the interrupted update first…")
            session.saved = previous["settings"]
            session.active = True
            session._state = previous
            failures = await session.cleanup()
            if failures:
                raise ota.OtaError("Some recovery settings could not be restored; see " + str(journal))
            session = DirectSession(link, key, password, journal, progress=user_progress)
        if args.recover:
            print("Recovery is complete.")
            return 0
        destination = await session.inspect()
        request_file = args.cache / "requests" / (key.lower() + ".json")
        save_request(request_file, destination)
        print(f"{destination.hw_id}: {destination.version_text}")
        if args.check:
            print("Direct authentication and OTA capability checks passed. No update was installed.")
            print(f"Preparation request: {request_file}")
            return 0
        await session.snapshot()
        catalog = GitHubCatalog()
        print("Checking published firmware for this exact board and role…")
        try:
            asset = await asyncio.to_thread(catalog.choose_asset, target_id=destination.target_id)
        except ota.OtaError as exc:
            asset = None
            if not args.firmware and not args.build:
                print(str(exc))
        choice = 2 if args.build else 1 if args.firmware else 0
        if not args.yes and not args.firmware and not args.build:
            options = ["Published GitHub firmware (recommended)", "Custom firmware file"]
            if not small_host():
                options.append("Build custom Full firmware from source")
            default_choice = remembered.get("firmware_choice", 0 if asset else 1)
            if remembered.get("target") != key.lower() or default_choice not in range(len(options)):
                default_choice = 0 if asset else 1
            choice = select("Firmware", options, default_choice)
        if choice == 0:
            if not asset:
                raise ota.OtaError("No published firmware matched this board; supply --firmware or choose a custom build.")
            firmware = await asyncio.to_thread(download_asset, asset, args.cache / "downloads")
        elif choice == 1:
            default_file = remembered.get("firmware_file") if remembered.get("target") == key.lower() else None
            firmware = args.firmware or Path(ask("Custom application ZIP, BIN, HEX, full mOTA, or prepared bundle", default_file))
        else:
            if not asset:
                raise ota.OtaError("A custom build needs the exact target environment; use a local firmware file for this board.")
            if not args.version:
                args.version = ota.format_version(max(destination.version, asset.version) + 1) + "-custom"
            firmware = await asyncio.to_thread(custom_build, args, destination, asset.target_env)
        prepared = await asyncio.to_thread(load_bundle, firmware, destination)
        if prepared:
            image, application = prepared
            if args.full and not application.info.is_full:
                application = await asyncio.to_thread(prepare_application, image, destination, args.cache, full=True)
        else:
            image = await asyncio.to_thread(load_application, firmware,
                                            target_id=destination.target_id, hw_id=destination.hw_id)
            application = await asyncio.to_thread(load_ready_application, firmware, image, destination)
        ImageCache(args.cache).remember(image)
        if image.fw_version > destination.version and application is None:
            builder = find_builder(args.cache, args.motatool)
            if not args.full and not builder and not small_host():
                builder = await asyncio.to_thread(install_builder, args.cache)
            base = None
            if not args.full and builder:
                base = await asyncio.to_thread(find_running_image, destination, args.cache, catalog,
                                               supplied=args.base)
            if not builder:
                print("Using a full update: no differential compiler setup is needed on this host.")
            if not builder and destination.platform == "nrf52" and not destination.external_storage:
                raise ota.OtaError(
                    "This internal-flash radio requires a differential package. Prepare it on a computer with "
                    f"--prepare-only --request {request_file} --firmware NEW_FIRMWARE --base RUNNING_FIRMWARE "
                    "--output update.ota.zip, then select that ZIP on the Pi.")
            application = await asyncio.to_thread(prepare_application, image, destination, args.cache,
                                                  base=base, builder=builder, full=args.full)
        elif image.fw_version <= destination.version:
            application = None
            print("The selected published/application version is already installed or older; keeping the running firmware.")
        bootloader = None
        if not args.no_bootloader and destination.platform == "nrf52":
            print("Checking the exact-board bootloader update…")
            bootloader = await asyncio.to_thread(prepare_bootloader, destination, args.cache)
        if not application and not bootloader:
            print("No newer compatible update is needed.")
            return 0
        print("\nReady to update:")
        print(f"  Destination: {destination.hw_id} — {key[:12].upper()}")
        if bootloader:
            print(f"  Bootloader: {bootloader.info.version}, signed exact-board image")
        if application:
            print(f"  Firmware: {application.info.version}, {application.info.kind}, {len(application.blob):,} bytes")
        if bootloader or application and application.signer:
            signer = bootloader.signer if bootloader else application.signer
            print(f"  Trusted update signer: {signer[:16]}")
        print("  Direct link only. Original radio and power-saving settings will be restored.")
        if not args.yes and ask("Start", "Y").lower() not in ("y", "yes"):
            print("Update cancelled.")
            return 0
        for package in (bootloader, application):
            if package:
                destination = await session.install(package, destination)
                print(f"Verified {package.info.kind} installation.")
        save_preferences(args.cache / "choices.json", port=port, mode=mode,
                         target=key.lower(), hw=destination.hw_id, firmware_choice=choice,
                         firmware_file=str(firmware) if choice == 1 else None)
        print("Update verified successfully.")
        return 0
    finally:
        cleanup_failed = False
        if session:
            failures = await session.cleanup()
            if failures:
                cleanup_failed = True
                print(f"Some settings still need recovery. Record: {session.journal}", file=sys.stderr)
                for command in failures:
                    print("  " + command, file=sys.stderr)
        await link.close()
        if cleanup_failed and sys.exc_info()[0] is None:
            raise ota.OtaError("The update completed but settings recovery is still needed; see the saved record.")


def prepare_without_radios(args) -> int:
    if not args.request or not args.firmware or not args.output:
        raise ota.OtaError("Use --prepare-only --request DEVICE.json --firmware FILE --output update.ota.zip.")
    destination = load_request(args.request)
    image = load_application(args.firmware, target_id=destination.target_id, hw_id=destination.hw_id)
    package = load_ready_application(args.firmware, image, destination)
    if package is None:
        catalog = GitHubCatalog()
        builder = find_builder(args.cache, args.motatool)
        if not builder and not args.full:
            builder = install_builder(args.cache)
        base = None
        if builder and not args.full:
            base = find_running_image(destination, args.cache, catalog, supplied=args.base)
        package = prepare_application(image, destination, args.cache, base=base, builder=builder, full=args.full)
    save_bundle(args.output, image, package)
    print(f"Prepared {package.info.kind}: {args.output} ({len(package.blob):,} transfer bytes)")
    print("Copy this ZIP to the Pi and select it as the custom firmware file.")
    return 0


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    args = arguments(argv)
    try:
        if args.prepare_only:
            return prepare_without_radios(args)
        ensure_dependencies(args, argv)
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        print("\nStopped. Check the recovery record if the destination was unreachable during cleanup.", file=sys.stderr)
        return 130
    except (ota.OtaError, RuntimeError, OSError, ValueError, subprocess.TimeoutExpired) as exc:
        print(f"Update stopped: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
