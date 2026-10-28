#!/usr/bin/env python3
"""Keep a passing RAM check attached to exactly the firmware it qualified."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_nrf52_font_license import validate_artifact_notice
from package_meshtext_license import validate_artifact_notice as validate_meshtext_notice
from package_pocket_gui_license import validate_artifact_notice as validate_pocket_gui_notice
from package_cardputer_license import validate_artifact_notice as validate_cardputer_notice


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def passing_report(path):
    report = json.loads(Path(path).read_text())
    fields = ("available_internal_bytes", "required_heap_bytes",
              "largest_internal_region_bytes", "required_contiguous_bytes")
    if not all(type(report.get(key)) is int and report[key] > 0 for key in fields):
        raise ValueError("incomplete runtime RAM qualification")
    if (report.get("schema_version") != 1 or report.get("passed") is not True
            or report["available_internal_bytes"] < report["required_heap_bytes"]
            or report["largest_internal_region_bytes"] < report["required_contiguous_bytes"]
            or not re.fullmatch(r"[0-9a-f]{64}", str(report.get("elf_sha256", "")))):
        raise ValueError("missing or failing runtime RAM qualification")
    return report


def validate_build(directory):
    directory = Path(directory)
    report = passing_report(directory / "firmware.memory.json")
    if report["elf_sha256"] != digest(directory / "firmware.elf"):
        raise ValueError("runtime RAM report belongs to a different ELF")
    return report


def artifact_files(stem):
    stem = Path(stem)
    candidates = [stem.parent / (stem.name + suffix)
                  for suffix in (".bin", "-merged.bin", ".uf2", ".zip", ".hex", ".capabilities.json",
                                 ".font-license.txt")]
    candidates.append(stem.parent / (stem.name + ".meshtext-license.txt"))
    candidates.append(stem.parent / (stem.name + ".pocket-gui-license.txt"))
    candidates.append(stem.parent / (stem.name + ".cardputer-license.txt"))
    return [p for p in candidates if p.is_file()]


def package_report(directory, stem):
    stem = Path(stem)
    report = validate_build(directory)
    validate_artifact_notice(stem)
    validate_meshtext_notice(stem)
    validate_pocket_gui_notice(stem)
    validate_cardputer_notice(stem)
    files = artifact_files(stem)
    if len(files) < 2 or not any(p.suffix in (".bin", ".uf2", ".zip", ".hex") for p in files):
        raise ValueError("cannot qualify an empty firmware package")
    manifest = json.loads((stem.parent / (stem.name + ".capabilities.json")).read_text())
    report.update(target=manifest["target"], artifact_target=manifest["artifact_target"],
                  pio_environment=report["target"],
                  files={p.name: digest(p) for p in files})
    path = stem.parent / (stem.name + ".memory.json")
    path.write_text(json.dumps(report, indent=2) + "\n")


def validate_package(stem):
    stem = Path(stem)
    validate_artifact_notice(stem)
    validate_meshtext_notice(stem)
    validate_pocket_gui_notice(stem)
    validate_cardputer_notice(stem)
    report = passing_report(stem.parent / (stem.name + ".memory.json"))
    actual = {p.name: digest(p) for p in artifact_files(stem)}
    if len(actual) < 2 or actual != report.get("files"):
        raise ValueError("firmware package changed after its runtime RAM check")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("validate-build", "package", "validate-package"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--stem", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "validate-build":
            validate_build(args.path)
        elif args.action == "validate-package":
            validate_package(args.path)
        elif args.stem:
            package_report(args.path, args.stem)
        else:
            parser.error("package requires --stem")
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Runtime RAM qualification failed: {error}\n")


if __name__ == "__main__":
    main()
