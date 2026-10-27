"""Published firmware, bounded downloads, and exact-image cache for the OTA UI.

This module never opens a radio or builds firmware. Download names narrow the
choice; the application's EndF identity remains authoritative after download.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import tempfile
from typing import Callable
from urllib.parse import quote, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener
import zipfile

import lora_ota as ota

DEFAULT_REPOSITORY = "mikecarper/MeshCore"
MAX_DOWNLOAD_BYTES = 64 * 1024 * 1024
MAX_CATALOG_BYTES = 16 * 1024 * 1024
MAX_ZIP_MEMBERS = 2048
_FAMILY_PREFIXES = ("repeater-room", "utility", "logging", "lora-ota", "full-profiles", "companion")
_SAFE_FILENAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,239}")
_VERSION = re.compile(r"(?:^|-)v(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?", re.I)


@dataclass(frozen=True)
class ReleaseAsset:
    name: str
    url: str
    size: int
    release_tag: str
    version: int
    target_env: str
    profile: str
    kind: str
    digest: str | None = None
    embedded_bootloader: bool = False

    @property
    def target_id(self) -> int:
        return int.from_bytes(hashlib.sha256(self.target_env.encode()).digest()[:4], "little")

    @property
    def version_text(self) -> str:
        return ota.format_version(self.version)


@dataclass(frozen=True)
class UpdatePlan:
    kind: str
    reason: str


def choose_update_strategy(*, platform: str, external_storage: bool,
                           exact_base: bool, delta_supported: bool,
                           low_power: bool = False, can_build_delta: bool | None = None,
                           prebuilt_delta: bool = False) -> UpdatePlan:
    """Choose a compatible transfer without silently inventing a delta base.

    ``prebuilt_delta`` means its target and base were independently checked.
    A ready builder can be used on any host; otherwise small hosts default to
    full packaging, which needs no Rust or firmware compiler.
    """
    if platform not in ("esp32", "nrf52"):
        return UpdatePlan("unavailable", "This platform has no qualified LoRa installer.")
    if delta_supported and prebuilt_delta:
        return UpdatePlan("delta", "A prepared delta matches the running firmware.")
    can_build = not low_power if can_build_delta is None else can_build_delta
    if delta_supported and exact_base and can_build:
        return UpdatePlan("delta", "The exact running image is available for a differential update.")
    if platform == "esp32" or external_storage:
        reason = ("The exact running image is unavailable; use a full update."
                  if not exact_base else
                  "A full update avoids installing or compiling a delta builder on this host."
                  if delta_supported and not can_build else
                  "This installer supports a full application update.")
        return UpdatePlan("full", reason)
    return UpdatePlan("unavailable", "Internal-flash nRF52 needs a compatible delta and its exact running image.")


def _repository(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", value):
        raise ota.OtaError("GitHub repository must be OWNER/REPOSITORY")
    return value


def _version(value: str) -> int | None:
    match = _VERSION.search(value)
    if not match:
        return None
    parts = tuple(int(item or 0) for item in match.groups())
    if any(item > 255 for item in parts):
        return None
    return sum(item << shift for item, shift in zip(parts, (24, 16, 8, 0)))


def _release_family(tag: str) -> str | None:
    if re.match(r"^v\d", tag, re.I):
        return tag
    prefixes = "|".join(re.escape(value) for value in _FAMILY_PREFIXES)
    match = re.fullmatch(rf"(?:{prefixes})(?:-(?:[2-9]|[1-9][0-9]+))?-(v\d.*)", tag)
    return match.group(1) if match else None


def _is_prerelease(release: dict) -> bool:
    return bool(release.get("prerelease")) or bool(re.search(
        r"(?:^|[-.])(?:alpha|beta|preview|rc)(?:[-.0-9]|$)",
        str(release.get("tag_name", "")), re.I))


def _valid_asset_url(url: str, repository: str, tag: str, name: str) -> bool:
    expected = f"https://github.com/{repository}/releases/download/{quote(tag, safe='')}/{quote(name, safe='')}"
    return url == expected


def _safe_redirect(url: str, *, api: bool = False) -> bool:
    try:
        parsed = urlsplit(url)
        host = (parsed.hostname or "").lower()
        return (parsed.scheme == "https" and not parsed.username and not parsed.password
                and parsed.port in (None, 443)
                and (host == "api.github.com" if api else
                     host == "github.com" or host.endswith(".githubusercontent.com")))
    except ValueError:
        return False


class _GithubRedirects(HTTPRedirectHandler):
    def __init__(self, *, api: bool):
        super().__init__()
        self.api = api

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not _safe_redirect(newurl, api=self.api):
            raise ota.OtaError("GitHub redirected the download to an unexpected host")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def _open(url: str, *, api: bool = False, opener: Callable | None = None):
    request = Request(url, headers={"User-Agent": "MeshCore-OTA-Wizard",
                                   "Accept": "application/vnd.github+json" if api else "application/octet-stream"})
    try:
        response = (opener or build_opener(_GithubRedirects(api=api)).open)(request, timeout=30)
    except (OSError, ValueError) as exc:
        raise ota.OtaError(f"Could not download GitHub firmware: {exc}") from exc
    final_url = response.geturl()
    if not _safe_redirect(final_url, api=api):
        response.close()
        raise ota.OtaError("GitHub returned an unexpected download host")
    return response


def parse_release_asset(asset: dict, release_tag: str, repository: str = DEFAULT_REPOSITORY) -> ReleaseAsset | None:
    name = asset.get("name", "")
    if not isinstance(name, str) or not _SAFE_FILENAME.fullmatch(name):
        return None
    suffix = Path(name).suffix.lower().lstrip(".")
    if suffix not in ("zip", "bin", "hex", "mota"):
        return None
    stem = name[:-(len(suffix) + 1)]
    version = _version(stem)
    match = _VERSION.search(stem)
    if version is None or match is None:
        return None
    published_target = stem[:match.start()].rstrip("-")
    if published_target.endswith("-merged") or stem.endswith("-merged"):
        return None
    published_target = re.sub(r"-ota$", "", published_target)
    embedded = bool(re.search(r"-with-bootloader-", published_target))
    profile_match = re.search(r"-(full|reduced)(?:-usb-wifi|-logging)?$", published_target, re.I)
    profile = profile_match.group(1).lower() if profile_match else "standard"
    target_env = published_target[:profile_match.start()] if profile_match else published_target
    target_env = re.sub(r"-with-bootloader-.*$", "", target_env)
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", target_env):
        return None
    if not re.search(r"_(?:(?:repeater|repeatr|room_server|room_svr|sensor)(?:$|[_-])|companion_radio_)", target_env, re.I):
        return None
    url = asset.get("browser_download_url", "")
    if not isinstance(url, str) or not _valid_asset_url(url, _repository(repository), release_tag, name):
        return None
    size = asset.get("size")
    if not isinstance(size, int) or isinstance(size, bool) or not 0 < size <= MAX_DOWNLOAD_BYTES:
        return None
    digest = asset.get("digest") or None
    if digest is not None and not re.fullmatch(r"sha256:[0-9a-fA-F]{64}", str(digest)):
        return None
    return ReleaseAsset(name, url, size, release_tag, version, target_env, profile,
                        suffix, digest.lower() if digest else None, embedded)


class GitHubCatalog:
    def __init__(self, repository: str = DEFAULT_REPOSITORY, *, opener: Callable | None = None):
        self.repository = _repository(repository)
        self.opener = opener
        self._assets: list[ReleaseAsset] | None = None
        self._all_assets: list[ReleaseAsset] = []

    def fetch_assets(self, *, include_prerelease: bool = False, max_pages: int = 3) -> list[ReleaseAsset]:
        if not 1 <= max_pages <= 10:
            raise ota.OtaError("Catalog page limit must be between 1 and 10")
        releases: list[dict] = []
        for page in range(1, max_pages + 1):
            # Release families contain thousands of per-board assets. Small
            # pages keep GitHub's embedded asset metadata bounded on a Pi.
            url = f"https://api.github.com/repos/{self.repository}/releases?per_page=10&page={page}"
            with _open(url, api=True, opener=self.opener) as response:
                raw = response.read(MAX_CATALOG_BYTES + 1)
            if len(raw) > MAX_CATALOG_BYTES:
                raise ota.OtaError("GitHub release catalog is unexpectedly large")
            try:
                rows = json.loads(raw)
            except (ValueError, UnicodeError) as exc:
                raise ota.OtaError("GitHub returned an invalid release catalog") from exc
            if not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows):
                raise ota.OtaError("GitHub returned an invalid release catalog")
            releases.extend(row for row in rows if not row.get("draft")
                            and (include_prerelease or not _is_prerelease(row)))
            if len(rows) < 10:
                break
        mains = [row for row in releases if re.match(r"^v\d", str(row.get("tag_name", "")), re.I)
                 and _version(row["tag_name"]) is not None]
        if not mains:
            raise ota.OtaError("No published main firmware release was found")
        main = max(mains, key=lambda row: (_version(row["tag_name"]), row.get("published_at") or ""))
        family = main["tag_name"]
        result: list[ReleaseAsset] = []
        historical: list[ReleaseAsset] = []
        for release in releases:
            tag = str(release.get("tag_name", ""))
            release_family = _release_family(tag)
            if release_family is None:
                continue
            rows = release.get("assets", [])
            if not isinstance(rows, list):
                raise ota.OtaError("GitHub returned an invalid release asset list")
            for row in rows:
                if isinstance(row, dict):
                    candidate = parse_release_asset(row, tag, self.repository)
                    if candidate is not None:
                        historical.append(candidate)
                        if release_family == family:
                            result.append(candidate)
        self._all_assets = historical
        self._assets = result
        return list(result)

    def candidates(self, target_env: str | None = None, *, target_id: int | None = None,
                   profile: str = "full", include_embedded: bool = False) -> list[ReleaseAsset]:
        if target_env is None and target_id is None:
            raise ota.OtaError("Select an exact firmware target")
        assets = self.fetch_assets() if self._assets is None else self._assets
        result = [row for row in assets if (target_env is None or row.target_env == target_env)
                  and (target_id is None or row.target_id == target_id)
                  and (include_embedded or not row.embedded_bootloader)]
        # Prefer a declared Full profile; older releases used the bare target.
        exact = [row for row in result if row.profile == profile]
        result = exact or [row for row in result if row.profile == "standard"]
        preference = {"mota": 0, "zip": 1, "bin": 2, "hex": 3}
        return sorted(result, key=lambda row: (-row.version, preference[row.kind], row.name))

    def choose_asset(self, target_env: str | None = None, **kwargs) -> ReleaseAsset:
        choices = self.candidates(target_env, **kwargs)
        if not choices:
            raise ota.OtaError("No published application matches this exact board, role, and profile")
        return choices[0]

    def base_candidates(self, target_env: str | None = None, *, target_id: int | None = None,
                        fw_version: int | None = None, limit: int = 8) -> list[ReleaseAsset]:
        """Find possible old images; their downloaded EndF body hash MUST be checked."""
        if target_env is None and target_id is None:
            raise ota.OtaError("Select an exact firmware target")
        if not 1 <= limit <= 32:
            raise ota.OtaError("Base candidate limit must be between 1 and 32")
        if self._assets is None:
            self.fetch_assets()
        preference = {"mota": 0, "zip": 1, "bin": 2, "hex": 3}
        matches = [row for row in self._all_assets
                   if not row.embedded_bootloader
                   and (target_env is None or row.target_env == target_env)
                   and (target_id is None or row.target_id == target_id)
                   and (fw_version is None or row.version == fw_version)]
        return sorted(matches, key=lambda row: (-row.version, preference[row.kind], row.name))[:limit]


def _atomic_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".ota-", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def download_asset(asset: ReleaseAsset, cache_dir: Path, *, opener: Callable | None = None) -> Path:
    """Download exactly one public release asset, verifying GitHub's SHA256 when supplied."""
    if not _SAFE_FILENAME.fullmatch(asset.name) or not 0 < asset.size <= MAX_DOWNLOAD_BYTES:
        raise ota.OtaError("Invalid release asset name or size")
    parsed = urlsplit(asset.url)
    parts = parsed.path.split("/")
    if len(parts) != 7 or not _valid_asset_url(asset.url, "/".join(parts[1:3]), asset.release_tag, asset.name):
        raise ota.OtaError("Release download URL does not match its asset")
    if asset.digest and not re.fullmatch(r"sha256:[0-9a-fA-F]{64}", asset.digest):
        raise ota.OtaError("Unsupported release asset digest")
    cache_dir = Path(cache_dir)
    cache_dir.mkdir(parents=True, exist_ok=True)
    key = hashlib.sha256(asset.url.encode()).hexdigest()[:16]
    path = cache_dir / f"{key}-{asset.name}"
    metadata = path.with_name(path.name + ".json")
    if path.is_file() and not path.is_symlink():
        data = ota.read_bounded_file(path, MAX_DOWNLOAD_BYTES, "cached release asset")
        digest = hashlib.sha256(data).hexdigest()
        try:
            saved = json.loads(ota.read_bounded_file(metadata, 4096, "cached download metadata"))
        except (OSError, ValueError, ota.OtaError):
            saved = {}
        if not isinstance(saved, dict):
            saved = {}
        expected = asset.digest.split(":", 1)[1] if asset.digest else saved.get("sha256")
        if len(data) == asset.size and digest == expected and saved.get("url") == asset.url:
            return path
    data = bytearray()
    try:
        with _open(asset.url, opener=opener) as response:
            while True:
                block = response.read(65536)
                if not block:
                    break
                data.extend(block)
                if len(data) > asset.size or len(data) > MAX_DOWNLOAD_BYTES:
                    raise ota.OtaError("Release download exceeded its declared size")
    except OSError as exc:
        raise ota.OtaError(f"Could not finish GitHub firmware download: {exc}") from exc
    if len(data) != asset.size:
        raise ota.OtaError("Release download is incomplete")
    digest = hashlib.sha256(data).hexdigest()
    if asset.digest and digest.lower() != asset.digest.split(":", 1)[1].lower():
        raise ota.OtaError("Release download SHA256 does not match GitHub")
    _atomic_bytes(path, bytes(data))
    _atomic_bytes(metadata, json.dumps({"url": asset.url, "sha256": digest}).encode())
    return path


def _check_identity(identity: ota.EndFInfo, target_id: int | None, hw_id: str | None) -> ota.EndFInfo:
    if target_id is not None and identity.target_id != target_id:
        raise ota.OtaError(f"Application target {identity.target_id:08X} does not match {target_id:08X}")
    if hw_id is not None and identity.hw_id != hw_id:
        raise ota.OtaError(f"Application hardware {identity.hw_id!r} does not match {hw_id!r}")
    return identity


def _application_bytes(raw: bytes, suffix: str) -> ota.EndFInfo:
    if suffix == ".mota":
        info = ota.parse_mota(raw)
        if info.is_bootloader or not info.is_full:
            raise ota.OtaError("An application image needs a full application mOTA, not a delta or bootloader")
        raw = info.payload
    elif suffix == ".hex":
        raw = ota.parse_intel_hex(raw)
    return ota.parse_endf(raw)


def load_application(path: Path, *, target_id: int | None = None,
                     hw_id: str | None = None) -> ota.EndFInfo:
    """Read a validated raw application, including the BIN inside a Nordic DFU ZIP."""
    path = Path(path)
    suffix = path.suffix.lower()
    if suffix not in (".zip", ".bin", ".hex", ".mota"):
        raise ota.OtaError("Choose an application BIN, HEX, DFU ZIP, or full mOTA")
    if suffix != ".zip":
        return _check_identity(_application_bytes(ota.read_bounded_file(
            path, MAX_DOWNLOAD_BYTES, "application file"), suffix), target_id, hw_id)
    ota.read_bounded_file(path, MAX_DOWNLOAD_BYTES, "application ZIP")
    try:
        with zipfile.ZipFile(path) as archive:
            members = archive.infolist()
            if len(members) > MAX_ZIP_MEMBERS:
                raise ota.OtaError("Application ZIP contains too many members")
            names = [member.filename for member in members]
            if len(set(names)) != len(names):
                raise ota.OtaError("Application ZIP contains duplicate member names")
            manifest_members = [member for member in members if member.filename == "manifest.json"]
            if manifest_members:
                manifest = json.loads(ota.read_zip_member(archive, manifest_members[0]))
                app = manifest.get("manifest", {}).get("application")
                if not isinstance(app, dict) or not isinstance(app.get("bin_file"), str):
                    raise ota.OtaError("DFU ZIP has no application image")
                name = app["bin_file"]
                if (PurePosixPath(name).is_absolute() or ".." in PurePosixPath(name).parts
                        or "\\" in name or ":" in name):
                    raise ota.OtaError("DFU ZIP has an unsafe application member")
                member = archive.getinfo(name)
                return _check_identity(ota.parse_endf(ota.read_zip_member(archive, member)), target_id, hw_id)
            matches: dict[bytes, ota.EndFInfo] = {}
            for member in members:
                member_suffix = Path(member.filename).suffix.lower()
                if member_suffix not in (".bin", ".hex", ".mota") or member.is_dir():
                    continue
                try:
                    identity = _check_identity(_application_bytes(
                        ota.read_zip_member(archive, member), member_suffix), target_id, hw_id)
                    matches[hashlib.sha256(identity.image).digest()] = identity
                except ota.OtaError:
                    continue
            if len(matches) != 1:
                raise ota.OtaError("Application ZIP must contain one unambiguous matching firmware image")
            return next(iter(matches.values()))
    except (OSError, KeyError, ValueError, TypeError, zipfile.BadZipFile) as exc:
        raise ota.OtaError(f"Invalid application ZIP: {exc}") from exc


class ImageCache:
    def __init__(self, cache_dir: Path):
        self.directory = Path(cache_dir) / "images"

    def remember(self, identity: ota.EndFInfo) -> Path:
        # Reparse callers' records rather than trusting manually constructed metadata.
        identity = ota.parse_endf(identity.image)
        path = self.directory / f"{identity.target_id:08X}" / f"{identity.body_hash.hex().upper()}.bin"
        fingerprint = path.with_suffix(".sha256")
        if path.is_file() and not path.is_symlink():
            try:
                previous = load_application(path, target_id=identity.target_id)
                digest = ota.read_bounded_file(fingerprint, 128, "cached image fingerprint").decode().strip()
            except (ota.OtaError, UnicodeError):
                previous = None
                digest = ""
            if (previous is not None and hashlib.sha256(previous.image).hexdigest() == digest
                    and previous.image != identity.image):
                raise ota.OtaError("Cached body hash already identifies a different complete image")
        _atomic_bytes(path, identity.image)
        _atomic_bytes(fingerprint, hashlib.sha256(identity.image).hexdigest().encode())
        return path

    def store(self, path: Path, *, target_id: int | None = None, hw_id: str | None = None) -> Path:
        return self.remember(load_application(path, target_id=target_id, hw_id=hw_id))

    def find(self, base_hash: bytes | str, target_id: int, hw_id: str | None = None,
             *, fw_version: int | None = None) -> Path | None:
        if isinstance(base_hash, str):
            if not re.fullmatch(r"[0-9a-fA-F]{16}", base_hash):
                raise ota.OtaError("Running base hash must have 16 hexadecimal characters")
            base_hash = bytes.fromhex(base_hash)
        if len(base_hash) != 8:
            raise ota.OtaError("Running base hash must contain eight bytes")
        path = self.directory / f"{target_id:08X}" / f"{base_hash.hex().upper()}.bin"
        if not path.is_file() or path.is_symlink():
            return None
        try:
            identity = load_application(path, target_id=target_id, hw_id=hw_id)
            digest = ota.read_bounded_file(path.with_suffix(".sha256"), 128,
                                          "cached image fingerprint").decode().strip()
        except (ota.OtaError, UnicodeError):
            return None
        return path if (hashlib.sha256(identity.image).hexdigest() == digest and
                        identity.body_hash == base_hash and
                        (fw_version is None or identity.fw_version == fw_version)) else None
