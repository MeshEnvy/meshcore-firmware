"""Qualify the private GNSS transform and native scanf replacement behavior."""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from test_ota_cli_listing import function


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("gnss_trim", ROOT / "scripts/nrf52_gnss_version_trim.py")
TRIM = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TRIM)
SOURCE = TRIM.INCLUDE + "\nvoid version(const char* ptr) {\n" + TRIM.ORIGINAL_CONVERSION + "\n}\n"
HEADERS = {name: "// fixture: " + name + "\n" for name in TRIM.HEADERS}


class Node:
    def __init__(self, path):
        self.path = Path(path)

    def srcnode(self):
        return self

    def get_abspath(self):
        return str(self.path)


class BuildEnv:
    def __init__(self, path):
        self.path = Path(path)
        self.registrations = []

    def subst(self, value):
        assert value == "$BUILD_DIR"
        return str(self.path)

    def File(self, path):
        return Path(path)

    def AddBuildMiddleware(self, callback, pattern):
        self.registrations.append((callback, pattern))


class GnssVersionTrimTests(unittest.TestCase):
    def test_private_copy_checks_source_and_all_headers_and_preserves_downloads(self):
        # Small synthetic dependencies test the transform mechanics; production
        # fingerprints remain fixed to the complete reviewed library files.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "libdeps/gnss/src" / TRIM.SOURCE_NAME
            source.parent.mkdir(parents=True)
            source.write_bytes(SOURCE.replace("\n", "\r\n").encode())
            for name, text in HEADERS.items():
                (source.parent / name).write_text(text)
            original = {path.name: path.read_bytes() for path in source.parent.iterdir()}
            env = BuildEnv(root / "build")
            with patch.object(TRIM, "SOURCE_SHA256", TRIM.digest(SOURCE)), patch.object(
                    TRIM, "HEADERS", {name: TRIM.digest(text) for name, text in HEADERS.items()}):
                destination = TRIM.replace_gnss_source(env, Node(source))
                self.assertEqual(destination, env.path / "trimmed-nrf52-gnss" / TRIM.SOURCE_NAME)
                output = destination.read_text()
                self.assertNotIn("sscanf", output)
                self.assertIn(TRIM.VERSION_CONVERSION, output)
                times = {path.name: path.stat().st_mtime_ns for path in destination.parent.iterdir()}
                self.assertEqual(TRIM.replace_gnss_source(env, Node(source)), destination)
                self.assertEqual(times, {path.name: path.stat().st_mtime_ns for path in destination.parent.iterdir()})
                self.assertEqual(original, {path.name: path.read_bytes() for path in source.parent.iterdir()})
                for name, text in HEADERS.items():
                    self.assertEqual((destination.parent / name).read_text(), text)
            for change in ("source", *HEADERS):
                with self.subTest(change=change), patch.object(TRIM, "SOURCE_SHA256", TRIM.digest(SOURCE)), patch.object(
                        TRIM, "HEADERS", {name: TRIM.digest(text) for name, text in HEADERS.items()}):
                    target = source if change == "source" else source.parent / change
                    before = target.read_bytes()
                    target.write_bytes(before + b"// unreviewed\n")
                    other_env = BuildEnv(root / ("unknown-" + change))
                    notice = io.StringIO()
                    node = Node(source)
                    with contextlib.redirect_stdout(notice):
                        self.assertIs(TRIM.replace_gnss_source(other_env, node), node)
                    self.assertIn("ordinary source retained", notice.getvalue())
                    self.assertFalse(other_env.path.exists())
                    target.write_bytes(before)

    def test_only_qualified_companion_installs_middleware(self):
        spec = importlib.util.spec_from_file_location("flash_trim", ROOT / "scripts/nrf52_flash_trim.py")
        policy = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(policy)

        class Env(dict):
            def PioPlatform(self):
                class Platform:
                    name = "nordicnrf52"
                    def get_package_dir(self, _name): return ""
                return Platform()
            def BoardConfig(self): return {"build.mcu": "nrf52840"}
            def AppendUnique(self, **kwargs):
                for name, value in kwargs.items(): self.setdefault(name, []).extend(value)
            def subst(self, value): return value.replace("$PROJECT_DIR", str(ROOT))
            def AddBuildMiddleware(self, callback, pattern): self.setdefault("middleware", []).append(pattern)

        for role, flags, expected in (("companion_radio", "", True),
                ("companion_radio", "-DMESH_NRF52_COMPANION_TRIM=0", False),
                ("simple_repeater", "", False), ("custom", "", False)):
            with self.subTest(role=role, flags=flags):
                env = Env(BUILD_FLAGS=flags, CPPDEFINES=[], PIOENV="custom",
                          SRC_FILTER="+<../examples/" + role + ">")
                policy.install(env)
                self.assertEqual("*" + TRIM.SOURCE_NAME in env.get("middleware", []), expected)

    def test_native_gnss_conversion_parity_and_bounded_cli_tokens(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("a native C++ compiler is required")
        cli = (ROOT / "src/helpers/ota/OtaCli.cpp").read_text()
        native = r'''
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
struct Version { bool valid; uint8_t high, low; };
static Version original(const char* ptr) {
  int protHi = 0, protLo = 0;
@ORIGINAL@
  return {scanned == 2, uint8_t(protHi), uint8_t(protLo)};
}
static Version trimmed(const char* ptr) {
  int protHi = 0, protLo = 0;
@CONVERSION@
  return {scanned == 2, uint8_t(protHi), uint8_t(protLo)};
}
static void parity(const char* text) {
  const Version before = original(text), after = trimmed(text);
  assert(before.valid == after.valid);
  if (before.valid) assert(before.high == after.high && before.low == after.low);
}
@TOKENS@
int main() {
  for (const char* text : {"18.00", "27.12", "0.0", "255.255", "256.256",
      "  +27.\t+12 extra", "\t\r\n-1.\v\f-2 trailing", "0018.0000",
      "", " ", "18", "18.", ".12", "18 .12", "18.x", "x18.12",
      "+.12", "18.+", "18.12.34", "18.0x20", "0x18.12",
      "2147483647.2147483647", "-2147483648.-2147483648",
      "2147483648.2147483648", "-2147483649.-2147483649",
      "9999999999999999999999999999999.12",
      "18.-9999999999999999999999999999999"}) parity(text);
  uint32_t random = 0xB43A1123;
  for (unsigned n = 0; n < 3000; ++n) {
    random = random * 1664525U + 1013904223U;
    char text[96];
    snprintf(text, sizeof(text), "\t%ld. %ld extra", long(int32_t(random)), long(int32_t(random ^ 0x91D43AA5U)));
    parity(text);
  }
  struct { char first[8]; uint8_t guard1; char second[10]; uint8_t guard2; } words = {};
  words.guard1 = words.guard2 = 0xA5;
  assert(parse_two_words("\t1234567\v\fabcdefghi\r\n", words.first, sizeof(words.first), words.second, sizeof(words.second)) == 2);
  assert(!strcmp(words.first, "1234567") && !strcmp(words.second, "abcdefghi"));
  for (const char* text : {"12345678 x", "x abcdefghij", "x y z"})
    assert(parse_two_words(text, words.first, sizeof(words.first), words.second, sizeof(words.second)) == 3);
  assert(words.guard1 == 0xA5 && words.guard2 == 0xA5);
  assert(parse_two_words(" \t\r\n", words.first, sizeof(words.first), words.second, sizeof(words.second)) == 0);
}
'''.replace("@ORIGINAL@", TRIM.ORIGINAL_CONVERSION).replace("@CONVERSION@", TRIM.VERSION_CONVERSION).replace(
            "@TOKENS@", function(cli, "parse_two_words"))
        native = "#include <initializer_list>\n" + native
        sanitizers = [] if os.name == "nt" else ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-pie", "-no-pie"]
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "parity.cpp"
            source.write_text(native)
            binary = Path(directory) / ("parity.exe" if os.name == "nt" else "parity")
            built = subprocess.run([compiler, "-std=c++11", "-O1", "-Wall", "-Wextra", "-Werror",
                                    *sanitizers, str(source), "-o", str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            tested = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == "__main__":
    unittest.main()
