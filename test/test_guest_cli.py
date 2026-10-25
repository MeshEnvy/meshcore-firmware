#!/usr/bin/env python3
"""Execute guest pages against the real ACL implementation and hex encoder."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

from test_replay_reset_integration import extract_braced

ROOT = Path(__file__).resolve().parents[1]
MOCKS = ROOT / "test/fixtures/client_acl_spiffs/mocks"
FIXTURE = ROOT / "test/fixtures/guest_cli/test_guest_cli.cpp"


def guest_dispatch(source):
    body = extract_braced(source, "void MyMesh::handleCommand(uint32_t sender_timestamp,")
    zero_guard = re.search(r"if \([^\n]+sender_timestamp == 0\) sender_timestamp = 1;", body).group()
    prefix = extract_braced(body, "if (strlen(command) > 4 && command[2] == '|')")
    dispatches = list(re.finditer(
        r"if \(handleDiscoveryResponseCommand\(command, reply, 157\)\s*"
        r"\|\| handleGuestCommand\(command, reply, 157\)\s*"
        r"\|\| mesh::cli::handleGuestsGet\(acl, command, reply, 157\)\s*"
        r"\|\| mesh::cli::handleLoginsGet\(login_history, command, reply, 157\)\) return;", body))
    if not dispatches:
        raise AssertionError("guest/login command dispatch is absent")
    generated = '#include <helpers/FloodCommandAliases.h>\n'
    # Keep the real guarded dispatch chain. Discovery settings have their own
    # production-handler tests and do not handle these guest/login queries.
    generated += 'static bool handleDiscoveryResponseCommand(const char*, char*, size_t) { return false; }\n'
    for signature in (
        "static bool commandFamilyMatches(", "static bool commandListMatches(",
        "static bool isCommonManagerReadOnlyAllowed(",
        "static bool isRegionMgrAllowed(", "static bool isFilterMgrAllowed(",
    ):
        generated += extract_braced(source, signature) + "\n"
    names = []
    for index, dispatch in enumerate(dispatches):
        guard_start = body.rindex("if (sender && !sender->isAdmin())", 0, dispatch.start())
        guard = extract_braced(body[guard_start:], "if (sender && !sender->isAdmin())")
        # No delegated code may run between the guard and guest command branch.
        if body[guard_start + len(guard):dispatch.start()].strip():
            raise AssertionError("guest command dispatch must immediately follow its role guard")
        name = f"guestCommand{index}"
        names.append(name)
        generated += f"""
static void {name}(ClientACL& acl, ClientInfo* sender, uint32_t sender_timestamp,
                   char* command, char* reply) {{
  {zero_guard}
  while (*command == ' ') ++command;
  {prefix}
  mesh::cli::normalizeCommandVerb(command);
  {guard}
  {dispatch.group()}
  strcpy(reply, "unhandled");
}}
"""
    generated += "using GuestHandler = void(*)(ClientACL&, ClientInfo*, uint32_t, char*, char*);\n"
    generated += "static const GuestHandler guest_handlers[] = {" + ",".join(names) + "};\n"
    return generated


class GuestCliTest(unittest.TestCase):
    def test_real_acl_and_guest_pages(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("a host C++17 compiler is required")
        utils = (ROOT / "src/Utils.cpp").read_text()
        hex_chars = re.search(r"static const char hex_chars\[\].*;", utils).group()
        generated = "namespace mesh {\n" + hex_chars + "\n"
        generated += extract_braced(utils, "void Utils::toHex(") + "\n}\n"
        generated += guest_dispatch((ROOT / "examples/simple_repeater/MyMesh.cpp").read_text())
        with tempfile.TemporaryDirectory(prefix=".tmp-guest-cli-", dir=ROOT) as directory:
            work = Path(directory)
            (work / "Utils.h").write_text("""#pragma once
#include <stddef.h>
#include <stdint.h>
namespace mesh { class Utils { public:
  static void toHex(char*, const uint8_t*, size_t);
}; }
""")
            (work / "production.h").write_text(generated)
            for clients in (32, 256):
                for repeater_only in (0, 1):
                    with self.subTest(max_clients=clients, repeater_only=repeater_only):
                        binary = work / f"guest-cli-{clients}-{repeater_only}.exe"
                        sanitizers = [] if os.name == "nt" else ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                        result = subprocess.run([
                            compiler, "-std=c++17", "-Wall", "-Wextra", *sanitizers,
                            "-DESP32=1", "-DESP32_PLATFORM=1",
                            "-DMESH_ENABLE_FLOOD_RULE_ENGINE=1",
                            f"-DMAX_CLIENTS={clients}", f"-DMESH_CLIENT_REPEATER_ONLY={repeater_only}",
                            f"-I{work}", f"-I{MOCKS}", f"-I{ROOT / 'src'}",
                            str(FIXTURE), "-o", str(binary),
                        ], capture_output=True, text=True, timeout=60)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        checked = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
                        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
                        self.assertIn("9 guest CLI checks passed", checked.stdout)


if __name__ == "__main__":
    unittest.main()
