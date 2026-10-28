#!/usr/bin/env python3
"""Bind resumable firmware to its effective inputs without publishing secrets."""

import argparse
import configparser
import glob
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

SCHEMA = 1
RECIPE_ENVIRONMENT = (
    "PLATFORMIO_BUILD_FLAGS", "PLATFORMIO_BUILD_UNFLAGS",
    "PLATFORMIO_BUILD_SRC_FILTER", "PLATFORMIO_EXTRA_SCRIPTS",
    "MESHCORE_ESP32_FULL_BUILD", "MESHCORE_REQUIRE_PACKET_LOGGING",
    "MESHCORE_COMPANION_RADIO_FULL", "MESHCORE_ESP32_FULL_PARTITION_TABLE",
    "MESHCORE_NRF52_INTERNAL_BOOTLOADER_UPDATE", "MESHCORE_FORCE_LORA_OTA",
    "MESHCORE_NRF52_LEGACY_FLASH_OTA", "MESHCORE_REDUCED_TLS",
    "MESHCORE_NRF52_EMBED_BOOTLOADER", "MESHCORE_NRF52_LEGACY_BOOTLOADER",
)
PACKAGE_SUFFIXES = (".bin", "-merged.bin", ".uf2", ".zip", ".hex",
                    ".capabilities.json", ".memory.json", ".font-license.txt",
                    ".meshtext-license.txt", ".pocket-gui-license.txt", ".cardputer-license.txt")


def canonical_digest(value):
    payload = json.dumps(value, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=True).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def git(root, *arguments):
    return subprocess.check_output(["git", "-C", str(root), *arguments],
                                   stderr=subprocess.PIPE)


def file_digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def source_digest(root, output, ancestors=(), expected_commit=None):
    """Read actual tracked bytes, including staged/unstaged and missing files."""
    root = root.resolve()
    output = Path(os.path.abspath(output))
    output_paths = (output, output.resolve())
    if any(selected == root or selected in root.parents for selected in output_paths):
        raise ValueError("output overlaps the source checkout or its ancestors")
    if root in ancestors:
        raise ValueError("recursive source checkout")
    commit = git(root, "rev-parse", "HEAD").decode("ascii").strip()
    if expected_commit is not None and commit != expected_commit:
        raise ValueError("source commit no longer matches artifact identity")
    entries = {}
    for record in git(root, "ls-files", "--stage", "-z").split(b"\0"):
        if not record:
            continue
        metadata, name = record.split(b"\t", 1)
        mode, object_id, stage = metadata.split()
        if stage != b"0":
            raise ValueError("unresolved source merge")
        entries[os.fsdecode(name)] = (mode.decode("ascii"), object_id.decode("ascii"))
    for name in git(root, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0"):
        if name:
            entries.setdefault(os.fsdecode(name), ("untracked", ""))
    # The project imports this Git-ignored file. Its absence is an input too;
    # cached resolved options alone cannot see edits during a long compile.
    entries.setdefault("platformio.local.ini", ("local-config", ""))
    content = []
    for name, (mode, object_id) in sorted(entries.items()):
        path = root / name
        # Only generated, untracked output may be excluded. Tracked sources
        # and imported configuration must always bind their actual bytes,
        # even if a caller incorrectly places output within their subtree.
        if mode == "untracked" and any(path == selected or selected in path.parents
                                       for selected in output_paths):
            continue
        item = [name, mode]
        if mode == "160000":
            item.append(object_id)
            if path.is_dir() and (path / ".git").exists():
                item.append(source_digest(path, output, (*ancestors, root)))
            else:
                if path.is_dir() and any(path.iterdir()):
                    raise ValueError("nonempty source checkout lacks Git metadata")
                item.append("uninitialized")
        elif path.is_symlink():
            item.extend(("symlink", os.readlink(path)))
            if path.is_file():
                item.append(file_digest(path))
            elif path.exists():
                raise ValueError("source symlink is not a regular file")
            else:
                item.append("missing-target")
        elif path.is_file():
            item.extend(("file", bool(path.stat().st_mode & stat.S_IXUSR), file_digest(path)))
        elif not path.exists():
            item.append("missing")
        else:
            raise ValueError("source is not a regular file")
        content.append(item)
    if git(root, "rev-parse", "HEAD").decode("ascii").strip() != commit:
        raise ValueError("source commit changed during snapshot")
    return canonical_digest({"commit": commit, "files": content})


def configuration_inputs(root, options):
    """Bind config imports, not package caches; unresolved imports fail closed."""
    patterns = options.get("extra_configs", [])
    if isinstance(patterns, str):
        patterns = [line.strip() for line in patterns.splitlines() if line.strip()]
    if not isinstance(patterns, list) or not all(isinstance(value, str) for value in patterns):
        raise ValueError("invalid imported configuration paths")
    # Resolved options may contain only the final imported setting. Include
    # literal imports from the project and each loaded file too, so an
    # intermediate external config cannot disappear from the receipt.
    pending = ["platformio.ini", "platformio.local.ini", *patterns]
    seen_patterns = set()
    parsed_files = set()
    inputs = []
    while pending:
        pattern = pending.pop(0)
        expanded = os.path.expanduser(os.path.expandvars(pattern))
        if "$" in expanded or "%(" in expanded:
            raise ValueError("unresolved imported configuration path")
        absolute = str(root.resolve() / expanded)
        if absolute in seen_patterns:
            continue
        seen_patterns.add(absolute)
        files = []
        for name in sorted(glob.glob(absolute, recursive=True)):
            path = Path(name)
            if not path.is_file():
                raise ValueError("imported configuration is not a regular file")
            files.append({"path": name, "resolved": str(path.resolve()),
                          "link": os.readlink(path) if path.is_symlink() else None,
                          "sha256": file_digest(path)})
            resolved = path.resolve()
            if resolved not in parsed_files:
                parsed_files.add(resolved)
                parser = configparser.RawConfigParser(inline_comment_prefixes=("#", ";"))
                with path.open(encoding="utf-8") as stream:
                    parser.read_file(stream)
                imports = parser.get("platformio", "extra_configs", fallback="")
                if imports:
                    pending.extend(value.strip() for value in imports.split(
                        "\n" if "\n" in imports else ", ") if value.strip())
        # An unmatched optional file/glob is retained, so later creation or
        # removal is visible even when the resolved PIO options are cached.
        inputs.append({"pattern": pattern, "expanded": absolute, "files": files})
    return inputs


def recipe_digest(args, options):
    if not isinstance(options, list):
        raise ValueError("invalid resolved PlatformIO configuration")
    selected = {}
    for value in options:
        if not isinstance(value, list) or len(value) != 2:
            raise ValueError("invalid PlatformIO section")
        section, pairs = value
        if section not in ("platformio", "env:" + args.pio_env):
            continue
        if section in selected or not isinstance(pairs, list):
            raise ValueError("invalid PlatformIO options")
        selected[section] = {}
        for pair in pairs:
            if not isinstance(pair, list) or len(pair) != 2 or not isinstance(pair[0], str):
                raise ValueError("invalid PlatformIO option")
            key, setting = pair
            if key in selected[section]:
                raise ValueError("duplicate PlatformIO option")
            selected[section][key] = setting
    if "env:" + args.pio_env not in selected:
        raise ValueError("missing resolved PlatformIO environment")
    names = set(RECIPE_ENVIRONMENT) | {name for name in os.environ
                                      if name.startswith(("PLATFORMIO_", "MESHCORE_"))}
    names.discard("MESHCORE_RECIPE_ORIGINAL_FLAGS")
    environment = {name: os.environ.get(name) for name in names}
    # A legacy bridge binds the supplied bytes, not just their external path.
    legacy = os.environ.get("MESHCORE_NRF52_LEGACY_BOOTLOADER")
    if legacy:
        environment["MESHCORE_NRF52_LEGACY_BOOTLOADER_SHA256"] = file_digest(Path(legacy))
    return canonical_digest({
        "schema_version": SCHEMA,
        "source": source_digest(args.root, args.output, expected_commit=args.source_commit),
        "target": args.target, "artifact_target": args.artifact_target,
        "platformio_env": args.pio_env, "platform": args.platform,
        "embedded_version": args.embedded_version, "profile": args.profile,
        "sensor_profile": args.sensor_profile, "ota_policy": args.ota_policy,
        "pio_options": selected, "environment": environment,
        "config_inputs": configuration_inputs(args.root, selected.get("platformio", {})),
        "original_flags": os.environ.get("MESHCORE_RECIPE_ORIGINAL_FLAGS", ""),
    })


def recipe_record(digest):
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError("invalid recipe digest")
    return {"schema_version": SCHEMA, "sha256": digest}


def attach_recipe(manifest_path, digest):
    manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict):
        raise ValueError("invalid capability manifest")
    manifest["build_recipe"] = recipe_record(digest)
    temporary = manifest_path.with_name(f".{manifest_path.name}.{os.getpid()}.tmp")
    try:
        temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=True) + "\n")
        temporary.replace(manifest_path)
    finally:
        temporary.unlink(missing_ok=True)


def matches_recipe(manifest_path, digest):
    manifest = json.loads(manifest_path.read_text())
    record = manifest.get("build_recipe")
    return (isinstance(record, dict) and type(record.get("schema_version")) is int
            and record == recipe_record(digest))


def package_occupied(stem):
    return any(os.path.lexists(str(stem) + suffix) for suffix in PACKAGE_SUFFIXES)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    digest = commands.add_parser("digest")
    digest.add_argument("--root", required=True, type=Path)
    digest.add_argument("--output", required=True, type=Path)
    for name in ("target", "artifact-target", "pio-env", "platform", "source-commit", "embedded-version",
                 "profile", "sensor-profile", "ota-policy"):
        digest.add_argument("--" + name, required=True)
    for name in ("attach", "matches"):
        action = commands.add_parser(name)
        action.add_argument("manifest", type=Path)
        action.add_argument("digest")
    occupied = commands.add_parser("occupied")
    occupied.add_argument("stem", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "digest":
            print(recipe_digest(args, json.load(sys.stdin)))
        elif args.command == "attach":
            attach_recipe(args.manifest, args.digest)
        elif args.command == "matches":
            return 0 if matches_recipe(args.manifest, args.digest) else 1
        else:
            return 0 if package_occupied(args.stem) else 1
    except (OSError, RuntimeError, ValueError, TypeError, AttributeError,
            configparser.Error, subprocess.CalledProcessError):
        # Raw options, flags, paths and nested Git diagnostics can contain
        # credentials. Report a fixed diagnostic, never their exception text.
        print("firmware build recipe: cannot qualify recipe inputs or manifest", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
