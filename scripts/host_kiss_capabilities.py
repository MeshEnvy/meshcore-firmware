#!/usr/bin/env python3
"""Qualify the optional host KISS service from effective flags and linked code."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import sys

from package_meshtext_license import linked_symbols

CAPABILITY = "radio2.kiss.host"
USB_CAPABILITY = "radio2.kiss.usb"
TCP_CAPABILITY = "radio2.kiss.tcp"
COMMAND_MARKERS = (b"Error: use set kiss on|off",
                   b"Error: use set kiss.transport usb|tcp",
                   b"USB: send KISS FEND to start")
TCP_MARKER = b"station TCP 5004"


def evidence(path):
    # LTO can inline the transport callbacks. Require executable framing and
    # the bounded Dispatcher queue shared by the actual RX/TX implementation.
    # A declaration, an undefined reference, or a CLI help string is not code.
    symbols = linked_symbols(path)
    required = ("_ZN4mesh8hostkiss5Codec4feedE",
                "_ZN4mesh8hostkiss5Codec11queueOutputE",
                "_ZN4mesh10Dispatcher22queueForeignRadioFrameE")
    selected = []
    for operation in required:
        matches = [name for name in symbols if name.startswith(operation)]
        if not matches:
            return []
        selected.append(matches[0])
    return selected


def operations(value):
    words = iter(shlex.split(value if isinstance(value, str) else "\n".join(map(str, value))))
    result = []
    for word in words:
        if word in ("-D", "-U"):
            word += next(words, "")
        match = re.fullmatch(r"-([DU])(\w+)(?:=(.*))?", word)
        if match:
            result.append(match.groups())
    return result


def required(config, env, platform, full_companion, overlay="", unflags=""):
    if platform not in ("ESP32_PLATFORM", "NRF52_PLATFORM"):
        return False
    rows = {name: dict(options) for name, options in config}
    options = rows.get("env:" + env, {})
    flags = operations(options.get("build_flags", [])) + operations(overlay)
    removed = set(operations(options.get("build_unflags", [])) + operations(unflags))
    flags = [flag for flag in flags if flag not in removed
             and not (flag[0] == "D" and ("D", flag[1], None) in removed)]
    defines = {name: "1" if value is None else value
               for kind, name, value in flags if kind == "D"}
    # PlatformIO emits surviving -U after CPPDEFINES, so -U always wins.
    for kind, name, _value in flags:
        if kind == "U":
            defines.pop(name, None)

    def enabled(name, default=False, seen=()):
        if name not in defines:
            return default
        if name in seen:
            return None
        value = defines[name].strip().strip("() ")
        if value in ("false", "true"):
            return value == "true"
        if re.fullmatch(r"[A-Za-z_]\w*", value):
            return enabled(value, False, (*seen, name))
        number = re.fullmatch(r"([+-]?(?:0[xX][0-9a-fA-F]+|[0-9]+))[uUlL]*", value)
        if number:
            value = number[1]
            base = 16 if "x" in value.lower() else 8 if re.fullmatch(r"[+-]?0[0-7]+", value) else 10
            return int(value, base) != 0
        return None

    # Match HostKissConfig.h's hard exclusion even with an explicit enable.
    if enabled("MESH_PRIMARY_ESPNOW") is True:
        return False
    if "MESH_ENABLE_HOST_KISS" in defines:
        return enabled("MESH_ENABLE_HOST_KISS") is not False
    return enabled("COMPANION_RADIO_FULL", full_companion) is not False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--required", action="store_true")
    parser.add_argument("--env", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--full-companion", action="store_true")
    args = parser.parse_args()
    try:
        selected = required(json.load(sys.stdin), args.env, args.platform, args.full_companion,
                            os.environ.get("PLATFORMIO_BUILD_FLAGS", ""),
                            os.environ.get("PLATFORMIO_BUILD_UNFLAGS", ""))
    except (OSError, ValueError, TypeError) as error:
        parser.exit(2, f"Cannot determine host KISS capability contract: {error}\n")
    return 0 if selected else 3


if __name__ == "__main__":
    raise SystemExit(main())
