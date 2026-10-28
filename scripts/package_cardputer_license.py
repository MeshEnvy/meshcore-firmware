#!/usr/bin/env python3
"""Carry Cardputer UI and font notices beside firmware containing that code."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re

from firmware_elf import FirmwareElf

ROOT = Path(__file__).resolve().parents[1]
UI_NOTICE_PATH = ROOT / "variants/m5stack_cardputer_adv/LICENSE.MultiMote"
FONT_NOTICE_PATH = ROOT / "variants/m5stack_cardputer_adv/LICENSE.DejaVu"
SIDECAR_SUFFIX = ".cardputer-license.txt"
COMPONENT = "cardputer-ui-and-fonts"
LICENSE = "MIT AND LicenseRef-DejaVu"
UI_LABEL = b"--- LICENSE.MultiMote ---\n\n"
FONT_LABEL = b"--- LICENSE.DejaVu ---\n\n"


def valid_ui_notice(data):
    return (0 < len(data) <= 262144 and all(text in data for text in (
        b"Copyright (c) 2026 MultiMote", b"Permission is hereby granted, free of charge",
        b"copies or substantial portions of the Software", b'THE SOFTWARE IS PROVIDED "AS IS"',
        b"LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE",
        b"SOFTWARE.")))


def valid_font_notice(data):
    return (0 < len(data) <= 262144 and all(text in data for text in (
        b"DejaVu changes are in public domain.",
        b"Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved.",
        b"The above copyright and trademark notices and this permission notice shall",
        b'are renamed to names not containing either the words "Bitstream" or the word',
        b"THE FONT SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND",
        b"Copyright (c) 2006 by Tavmjong Bah. All Rights Reserved.",
        b"name of Tavmjong Bah shall not",
        b"AMSFonts (v. 2.2) copyright")))


def valid_notice(data):
    if not (0 < len(data) <= 524288 and data.count(UI_LABEL) == 1
            and data.count(FONT_LABEL) == 1):
        return False
    ui_start = data.index(UI_LABEL) + len(UI_LABEL)
    font_start = data.index(FONT_LABEL)
    return (ui_start < font_start and valid_ui_notice(data[ui_start:font_start])
            and valid_font_notice(data[font_start + len(FONT_LABEL):]))


def notice_bytes():
    ui, fonts = UI_NOTICE_PATH.read_bytes(), FONT_NOTICE_PATH.read_bytes()
    if not valid_ui_notice(ui) or not valid_font_notice(fonts):
        raise ValueError("Cardputer UI/font source notice is incomplete")
    return (b"Cardputer Adv UI and font notices\n\n"
            b"Source: MultiMote/meshcore-cardputer-adv\n\n"
            + UI_LABEL + ui.rstrip() + b"\n\n" + FONT_LABEL + fonts.rstrip() + b"\n")


def cardputer_evidence(path):
    """Only allocated definitions qualify, never target names or debug strings."""
    if Path(path).read_bytes()[:4] != b"\x7fELF":
        return []
    elf = FirmwareElf(path)
    evidence = []
    for name, (_address, size, index) in elf.symbols.items():
        if size <= 0 or index >= len(elf.sections):
            continue
        section = elf.sections[index]
        if section[1] == 8 or not section[2] & 2:
            continue
        ui = (section[2] & 4 and name.startswith((
            "_ZN15CardputerUITask", "_ZNK15CardputerUITask")))
        font = re.fullmatch(r"(?:_ZL[0-9]+)?DejaVuSans_[A-Za-z0-9_]+(?:\..+)?", name)
        if ui or font:
            evidence.append(name)
    return sorted(evidence)


def sidecar(stem):
    stem = Path(stem)
    return stem.parent / (stem.name + SIDECAR_SUFFIX)


def notice_record(stem, data):
    return {"component": COMPONENT, "license": LICENSE,
            "file": sidecar(stem).name, "sha256": hashlib.sha256(data).hexdigest()}


def manifest_record(stem):
    stem = Path(stem)
    manifest = json.loads((stem.parent / (stem.name + ".capabilities.json")).read_text())
    records = [record for record in manifest.get("third_party_notices", [])
               if record.get("component") == COMPONENT]
    if len(records) > 1:
        raise ValueError(f"{stem}: duplicate Cardputer UI/font notice declarations")
    if not records:
        return None
    record = records[0]
    if (record.get("file") != sidecar(stem).name or record.get("license") != LICENSE
            or not re.fullmatch(r"[0-9a-f]{64}", str(record.get("sha256", "")))):
        raise ValueError(f"{stem}: invalid Cardputer UI/font notice declaration")
    return record


def validate_artifact_notice(stem):
    record = manifest_record(stem)
    output = sidecar(stem)
    if record is None:
        if output.exists():
            raise ValueError(f"{output}: stale notice for unaffected firmware")
        return None
    if not output.is_file():
        raise ValueError(f"{output}: missing Cardputer UI/font notice")
    data = output.read_bytes()
    if not valid_notice(data) or hashlib.sha256(data).hexdigest() != record["sha256"]:
        raise ValueError(f"{output}: incomplete or mismatched Cardputer UI/font notice")
    return output


def collect_build_notice(directory, stem):
    evidence = cardputer_evidence(Path(directory) / "firmware.elf")
    record = manifest_record(stem)
    if bool(evidence) != bool(record):
        raise ValueError("Cardputer UI/font notice declaration does not match the linked ELF")
    if not evidence:
        return validate_artifact_notice(stem)
    data = notice_bytes()
    if record != notice_record(stem, data):
        raise ValueError("Cardputer UI/font notice declaration does not match the source notice")
    output = sidecar(stem)
    if not output.exists():
        temporary = output.with_name(f".{output.name}.{os.getpid()}.tmp")
        try:
            temporary.write_bytes(data)
            temporary.replace(output)
        finally:
            temporary.unlink(missing_ok=True)
    return validate_artifact_notice(stem)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("collect", "validate"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--stem", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "collect":
            if args.stem is None:
                parser.error("collect requires --stem")
            collect_build_notice(args.path, args.stem)
        else:
            validate_artifact_notice(args.path)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Cardputer UI/font notice qualification failed: {error}\n")


if __name__ == "__main__":
    main()
