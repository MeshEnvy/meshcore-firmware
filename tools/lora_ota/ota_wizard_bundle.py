"""Portable, public-only preparation records and PC-to-Pi update bundles."""
from __future__ import annotations

from dataclasses import asdict, fields
import hashlib
import json
from pathlib import Path
import zipfile
import re

import lora_ota as ota
from ota_wizard_session import Destination, Package, validate_package


class VerificationDependencyError(ota.OtaError):
    """The package needs the optional signature verifier."""


def load_ready_application(path: Path, image: ota.EndFInfo,
                           destination: Destination, *, validate_receiver: bool = True) -> Package | None:
    """Keep a supplied full mOTA intact, including its verified signing key."""
    if Path(path).suffix.lower() != ".mota":
        return None
    blob = ota.read_bounded_file(path, 64 * 1024 * 1024, "application mOTA")
    info = ota.parse_mota(blob)
    if info.is_bootloader or not info.is_full:
        raise ota.OtaError("Choose a full application mOTA, not a delta or bootloader package.")
    # The complete image, rather than caller-supplied EndF fields, determines
    # the expected post-install body hash and binds every manifest identity.
    expected = ota.parse_endf(image.image)
    if (info.payload != expected.image
            or info.image_hash != hashlib.sha256(expected.image).digest()
            or info.image_size != len(expected.image)
            or info.target_id != expected.target_id or info.hw_id != expected.hw_id
            or info.fw_version != expected.fw_version):
        raise ota.OtaError("The supplied mOTA does not match the selected complete firmware.")
    lib = ota.bootloader_library()
    try:
        parsed = lib.parse_container(blob)
        signer = parsed.manifest.signer_pubkey.hex().upper() if parsed.manifest.is_signed else None
        package = Package(blob, expected.body_hash, signer)
        if validate_receiver:
            validate_package(package, destination)
        else:
            # A full unsigned container can supply the complete new image for
            # a delta even when the receiver cannot install that full format.
            problems = lib.verify(parsed)
            if problems:
                raise ota.OtaError("Application mOTA integrity failed: " + "; ".join(problems))
    except ImportError as exc:
        raise VerificationDependencyError("Signature verification support is required for this application mOTA.") from exc
    except (ValueError, TypeError) as exc:
        raise ota.OtaError("Invalid supplied application mOTA: " + str(exc)) from exc
    return package


def save_request(path: Path, destination: Destination) -> None:
    value = asdict(destination)
    value["base_hash"] = destination.base_hash.hex()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps({"format": 1, "destination": value}, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def load_request(path: Path) -> Destination:
    try:
        value = json.loads(ota.read_bounded_file(path, 64 * 1024, "preparation request"))
        if not isinstance(value, dict) or value.get("format") != 1 or not isinstance(value.get("destination"), dict):
            raise ValueError("incompatible request format")
        data = dict(value["destination"])
        if set(data) != {field.name for field in fields(Destination)}:
            raise ValueError("missing or unknown destination fields")
        if (not isinstance(data["public_key"], str)
                or not re.fullmatch(r"[0-9A-Fa-f]{64}", data["public_key"])
                or int(data["public_key"], 16) == 0):
            raise ValueError("invalid destination public key")
        if not isinstance(data["hw_id"], str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,32}", data["hw_id"]):
            raise ValueError("invalid board identity")
        for name in ("target_id", "version"):
            if type(data[name]) is not int or not 0 < data[name] <= 0xFFFFFFFF:
                raise ValueError("invalid " + name)
        if data["platform"] not in ("esp32", "nrf52"):
            raise ValueError("unsupported platform")
        if data["max_block_size"] not in (1024, 2048) or type(data["max_block_size"]) is not int:
            raise ValueError("unsupported block size")
        if type(data["codecs"]) is not int or not 0 < data["codecs"] <= 0xFF:
            raise ValueError("invalid codec mask")
        if any(type(data[name]) is not bool for name in ("external_storage", "sd_storage")):
            raise ValueError("invalid storage flags")
        if not isinstance(data["version_text"], str) or len(data["version_text"]) > 512:
            raise ValueError("invalid version text")
        if not isinstance(data["base_hash"], str) or not re.fullmatch(r"[0-9a-fA-F]{16}", data["base_hash"]):
            raise ValueError("invalid running body hash")
        data["base_hash"] = bytes.fromhex(data["base_hash"])
        if data["boot_version"] is not None and (type(data["boot_version"]) is not int or not 0 < data["boot_version"] <= 0xFFFFFFFF):
            raise ValueError("invalid bootloader version")
        if data["bootloader"] is not None:
            boot = data["bootloader"]
            if not isinstance(boot, dict) or set(boot) != {field.name for field in fields(ota.BootloaderStatus)}:
                raise ValueError("invalid bootloader identity")
            for name in ("board_id", "target_id", "crc32", "apply_abi", "storage_flags"):
                if type(boot[name]) is not int or not 0 <= boot[name] <= 0xFFFFFFFF:
                    raise ValueError("invalid bootloader field")
            if not isinstance(boot["device_name"], str) or not re.fullmatch(r"[A-Za-z0-9_]{1,32}", boot["device_name"]):
                raise ValueError("invalid bootloader name")
            lib = ota.bootloader_library()
            if (lib.bootloader_target_id(boot["board_id"], boot["device_name"]) != boot["target_id"]
                    or lib.bootloader_hw_id(boot["board_id"], boot["device_name"]).rstrip(b"\0").decode() != boot["hw_id"]):
                raise ValueError("inconsistent bootloader identity")
            data["bootloader"] = ota.BootloaderStatus(**boot)
        return Destination(**data)
    except (ValueError, KeyError, TypeError, AttributeError) as exc:
        raise ota.OtaError("Invalid OTA preparation request: " + str(exc)) from exc


def save_bundle(path: Path, image: ota.EndFInfo, package: Package) -> None:
    info = package.info
    if (info.is_bootloader or info.image_hash != hashlib.sha256(image.image).digest()
            or info.target_id != image.target_id or info.hw_id != image.hw_id
            or info.fw_version != image.fw_version or info.image_size != len(image.image)):
        raise ota.OtaError("The prepared package and complete new firmware do not agree.")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_STORED) as archive:
        archive.writestr("ota-bundle.json", json.dumps({"format": 1,
            "package_sha256": hashlib.sha256(package.blob).hexdigest()}))
        archive.writestr("application.mota", package.blob)
        archive.writestr("firmware.bin", image.image)
    temporary.replace(path)


def load_bundle(path: Path, destination: Destination) -> tuple[ota.EndFInfo, Package] | None:
    try:
        return _load_bundle(path, destination)
    except (zipfile.BadZipFile, KeyError, ValueError, TypeError, UnicodeError) as exc:
        raise ota.OtaError("Invalid prepared OTA bundle: " + str(exc)) from exc


def _load_bundle(path: Path, destination: Destination) -> tuple[ota.EndFInfo, Package] | None:
    if path.suffix.lower() != ".zip":
        return None
    ota.read_bounded_file(path, 64 * 1024 * 1024, "OTA bundle")
    with zipfile.ZipFile(path) as archive:
        if "ota-bundle.json" not in archive.namelist():
            return None
        names = archive.namelist()
        if len(names) != 3 or set(names) != {"ota-bundle.json", "application.mota", "firmware.bin"}:
            raise ota.OtaError("The OTA preparation bundle has unexpected members.")
        metadata = json.loads(ota.read_zip_member(archive, archive.getinfo("ota-bundle.json")))
        blob = ota.read_zip_member(archive, archive.getinfo("application.mota"))
        image = ota.parse_endf(ota.read_zip_member(archive, archive.getinfo("firmware.bin")))
    if (not isinstance(metadata, dict) or metadata.get("format") != 1
            or metadata.get("package_sha256") != hashlib.sha256(blob).hexdigest()):
        raise ota.OtaError("The prepared OTA bundle's checksum does not match.")
    info = ota.parse_mota(blob)
    if (info.image_hash != hashlib.sha256(image.image).digest() or info.image_size != len(image.image)
            or info.target_id != image.target_id or info.hw_id != image.hw_id
            or info.fw_version != image.fw_version):
        raise ota.OtaError("The complete firmware does not match the prepared package.")
    lib = ota.bootloader_library()
    try:
        problems = lib.verify(lib.parse_container(blob))
    except ImportError as exc:
        raise VerificationDependencyError("Signature verification support is required for this OTA bundle.") from exc
    if problems:
        raise ota.OtaError("Prepared OTA package integrity failed: " + "; ".join(problems))
    parsed = lib.parse_container(blob)
    signer = parsed.manifest.signer_pubkey.hex().upper() if parsed.manifest.is_signed else None
    package = Package(blob, image.body_hash, signer)
    validate_package(package, destination)
    return image, package
