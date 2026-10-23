#!/usr/bin/env python3
"""Validate and embed an exact RAK4631 bootloader for opt-in migration builds."""
import hashlib
from pathlib import Path
import struct
import zlib

BOOT_BASE = 0xF4000
BOOT_SIZE = 0xA000
BOARD_ID = 0x239A0029


def load_bootloader(path):
    path = Path(path)
    if path.suffix.lower() == '.hex':
        from intelhex import IntelHex
        image = IntelHex(str(path))
        return bytes(image.tobinarray(start=BOOT_BASE, size=BOOT_SIZE))
    image = path.read_bytes()
    if len(image) != BOOT_SIZE:
        raise ValueError('raw bootloader must be the complete padded 40 KiB image')
    return image


def validate_candidate(image):
    if len(image) != BOOT_SIZE:
        raise ValueError('candidate must be exactly 40 KiB')
    sp, reset = struct.unpack_from('<II', image)
    if not (0x20000000 <= sp <= 0x20040000 and sp % 8 == 0 and reset & 1 and
            BOOT_BASE <= reset & ~1 < BOOT_BASE + BOOT_SIZE):
        raise ValueError('invalid candidate vectors')
    offset = BOOT_SIZE - 76
    fields = struct.unpack_from('<IIHHIII16sI', image, offset)
    if fields[:6] != (0x464D4C42, 0x31435243, 1, 44, BOOT_BASE, BOOT_SIZE):
        raise ValueError('candidate lacks the fixed BLMF envelope')
    if fields[6] != BOARD_ID or fields[7] != b'4631_DFU' + bytes(8):
        raise ValueError('candidate is not the normal exact RAK4631 bootloader')
    extension = struct.unpack_from('<IIHHIHHIHHI', image, offset + 44)
    if (extension[:4] != (0x324D4C42, 0x54464F53, 2, 32) or
            extension[4] in (0, 0xffffffff) or not extension[4] & 0xff or
            extension[5:] != (140, 0xB6, 0x26000, 1, 0, 0)):
        raise ValueError('candidate changes the RAK4631 SoftDevice/application layout')
    normalized = bytearray(image)
    normalized[offset + 40:offset + 44] = bytes(4)
    if fields[8] != zlib.crc32(normalized):
        raise ValueError('candidate whole-image CRC mismatch')
    return extension[4]


def emit_resource(legacy_path, candidate_path, output):
    legacy = load_bootloader(legacy_path)
    candidate = load_bootloader(candidate_path)
    validate_candidate(candidate)
    # Pin the exact lab/rollout source bootloader. No broad "all old versions"
    # exception and no removal of current bootloader identity checks.
    if legacy == candidate:
        raise ValueError('legacy and candidate images must differ')
    arrays = {
        'migration_candidate': candidate,
        'migration_candidate_sha256': hashlib.sha256(candidate).digest(),
        'migration_legacy_sha256': hashlib.sha256(legacy).digest(),
    }
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    header = '#pragma once\n#include <stdint.h>\n'
    source = '#include "BootloaderMigrationResource.h"\n'
    for name, data in arrays.items():
        header += f'extern const uint8_t {name}[{len(data)}];\n'
        source += f'alignas(4) const uint8_t {name}[{len(data)}] = {{\n'
        for index in range(0, len(data), 16):
            source += '  ' + ', '.join(f'0x{value:02x}' for value in data[index:index + 16]) + ',\n'
        source += '};\n'
    (output / 'BootloaderMigrationResource.h').write_text(header, encoding='ascii')
    (output / 'BootloaderMigrationResource.cpp').write_text(source, encoding='ascii')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--legacy', required=True)
    parser.add_argument('--candidate', required=True)
    parser.add_argument('--output', required=True)
    options = parser.parse_args()
    emit_resource(options.legacy, options.candidate, options.output)
