"""Keep the reviewed GNSS version parser without linking the scanf engine.

Only the qualified nRF52 Companion trim installs this middleware. Compile a
private copy of the reviewed SparkFun GNSS sources; never change downloaded dependencies.
Unknown sources or headers retain their ordinary implementation.
"""

import hashlib
from pathlib import Path


SOURCE_NAME = "SparkFun_u-blox_GNSS_Arduino_Library.cpp"
SOURCE_SHA256 = "8ed1d313177df864b6821b3cb3e4fbcc59d850d09e2460269d571f70cd2b9f1c"
HEADERS = {
    "SparkFun_u-blox_GNSS_Arduino_Library.h": "5da7a95ddd4b24e26ca474d017a6a5347137a31aee58fb7b450a4ce85bbcc07a",
    "u-blox_config_keys.h": "86cae35ec200871f748be21d6d59583c48db82621f4c5504e257e8358a241499",
    "u-blox_structs.h": "59c0a7db1e3fe8eb78692a3ef9411f12048573a6513c41937e51e9d5ede06498",
}
ORIGINAL_CONVERSION = '''      int scanned = sscanf(ptr, "%d.%d", &protHi, &protLo);'''
VERSION_CONVERSION = '''      // Preserve %d.%d conversion, including signs and leading whitespace.
      // strtol is already used by the application; scanf needs a separate engine.
      char *versionEnd;
      protHi = static_cast<int>(strtol(ptr, &versionEnd, 10));
      int scanned = 0;
      if (versionEnd != ptr && *versionEnd == '.')
      {
        const char *minor = versionEnd + 1;
        protLo = static_cast<int>(strtol(minor, &versionEnd, 10));
        if (versionEnd != minor)
          scanned = 2;
      }'''
INCLUDE = '#include "SparkFun_u-blox_GNSS_Arduino_Library.h"'


def digest(source):
    return hashlib.sha256(source.encode("utf-8")).hexdigest()


def patched_source(source):
    source = source.replace("\r\n", "\n")
    if (digest(source) != SOURCE_SHA256 or source.count(ORIGINAL_CONVERSION) != 1
            or source.count(INCLUDE) != 1):
        raise RuntimeError("changed reviewed SparkFun GNSS source")
    return source.replace(INCLUDE, "#include <stdlib.h>\n" + INCLUDE, 1).replace(
        ORIGINAL_CONVERSION, VERSION_CONVERSION, 1)


def write_if_changed(path, content):
    if not path.exists() or path.read_text(encoding="utf-8") != content:
        path.write_text(content, encoding="utf-8")


def replace_gnss_source(build_env, node):
    source = Path(node.srcnode().get_abspath())
    if source.name != SOURCE_NAME or source.parent.name != "src":
        return node
    try:
        patched = patched_source(source.read_text(encoding="utf-8"))
        headers = {}
        for name, expected in HEADERS.items():
            header = (source.parent / name).read_text(encoding="utf-8").replace("\r\n", "\n")
            if digest(header) != expected:
                raise RuntimeError("changed reviewed SparkFun GNSS header: " + name)
            headers[name] = header
    except (OSError, UnicodeError, RuntimeError) as error:
        print("nRF52 Companion GNSS trim: ordinary source retained (" + str(error) + ")")
        return node
    destination = Path(build_env.subst("$BUILD_DIR")) / "trimmed-nrf52-gnss" / source.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Keep its quoted sibling includes bound to the same reviewed library.
    for name, header in headers.items():
        write_if_changed(destination.parent / name, header)
    write_if_changed(destination, patched)
    print("nRF52 Companion GNSS trim: reviewed version conversion uses strtol")
    return build_env.File(str(destination))


def install(build_env):
    build_env.AddBuildMiddleware(replace_gnss_source, "*" + SOURCE_NAME)
