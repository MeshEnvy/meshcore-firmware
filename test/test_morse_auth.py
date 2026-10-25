#!/usr/bin/env python3
"""Execute the production legacy portable handoff and live manager allowlists."""
from pathlib import Path
import unittest

from test_morse_tx import compile_run
from test_replay_reset_integration import extract_braced

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

struct ClientInfo {
  enum Permission { Guest = 0, Admin = 1, RegionManager = 2, FilterManager = 4 };
  unsigned permissions;
  bool isAdmin() const { return (permissions & Admin) != 0; }
  bool isRegionMgr() const { return (permissions & RegionManager) != 0; }
  bool isFilterMgr() const { return (permissions & FilterManager) != 0; }
};

// Guest and discovery behavior have their own real-handler suites. These
// unrelated dispatch targets must not consume any of the Morse commands below.
namespace mesh { namespace cli {
bool handleGuestsGet(int, const char*, char*, size_t) { return false; }
bool handleLoginsGet(int, const char*, char*, size_t) { return false; }
} }

@MANAGER_HELPERS@

struct CommonCLI {
  unsigned calls = 0, rf_actions = 0, settings = 0;
  uint32_t last_timestamp = 0;
  void handleCommand(uint32_t timestamp, const char* command, char* reply) {
    ++calls;
    last_timestamp = timestamp;
    if (!strncmp(command, "morse send ", 11) || !strcmp(command, "morse stop")) ++rf_actions;
    if (!strncmp(command, "set morse.", 10)) ++settings;
    strcpy(reply, "delegated");
  }
};

struct LegacyPortable {
  CommonCLI _cli;
  int acl = 0;
  int login_history = 0;
  unsigned discovery_calls = 0;
  bool handleDiscoveryResponseCommand(const char*, char*, size_t) {
    ++discovery_calls;
    return false;
  }
  bool handleGuestCommand(const char*, char*, size_t) { return false; }
  void dispatch(ClientInfo* sender, uint32_t sender_timestamp,
                const char* command, char* reply) {
    @PORTABLE_HANDOFF@
  }
};

static void denied(LegacyPortable& node, ClientInfo* sender, const char* command) {
  for (uint32_t timestamp : {uint32_t(0), uint32_t(123)}) {
    const unsigned calls = node._cli.calls;
    const unsigned rf = node._cli.rf_actions;
    const unsigned settings = node._cli.settings;
    const unsigned discovery_calls = node.discovery_calls;
    char reply[160] = {};
    node.dispatch(sender, timestamp, command, reply);
    assert(!strcmp(reply, "Err - not permitted"));
    assert(node._cli.calls == calls);
    assert(node._cli.rf_actions == rf && node._cli.settings == settings);
    assert(node.discovery_calls == discovery_calls);
  }
}

static void allowed(LegacyPortable& node, ClientInfo* sender, const char* command) {
  const uint32_t timestamp = sender ? 123 : 0;
  const unsigned calls = node._cli.calls;
  char reply[160] = {};
  node.dispatch(sender, timestamp, command, reply);
  assert(!strcmp(reply, "delegated"));
  assert(node._cli.calls == calls + 1 && node._cli.last_timestamp == timestamp);
}

int main() {
  LegacyPortable node;
  ClientInfo guest{ClientInfo::Guest}, admin{ClientInfo::Admin};
  ClientInfo region{ClientInfo::RegionManager}, filter{ClientInfo::FilterManager};
  ClientInfo combined{ClientInfo::RegionManager | ClientInfo::FilterManager};
  for (const char* command : {
      "morse send TEST DE N0CALL", "morse stop", "set morse.freq 902.100", "set morse.wpm 25",
      "morse listen 5", "morse read", "morse read 128", "morse stats", "morse clear",
      "get morse", "get morse.freq", "get morse.wpm", "cw on", "cw off", "set cw2 on"}) {
    // These assertions also use the helpers if the production gate is
    // accidentally removed, so the regression fails on behavior, not on an
    // unrelated unused-function compiler warning.
    assert(!isRegionMgrAllowed(command) && !isFilterMgrAllowed(command));
    for (ClientInfo* sender : {&guest, &region, &filter, &combined}) denied(node, sender, command);
    allowed(node, &admin, command);
    allowed(node, nullptr, command);
  }
  assert(node._cli.rf_actions == 4 && node._cli.settings == 4);

  for (const char* command : {"ver", "board", "clock", "sensor get battery_voltage",
      "sensor list", "get role", "get public.key", "get rx.watchdog", "get clock.sync"}) {
    assert(isCommonManagerReadOnlyAllowed(command));
    denied(node, &guest, command);
    for (ClientInfo* sender : {&region, &filter, &combined, &admin}) allowed(node, sender, command);
    allowed(node, nullptr, command);
  }
  for (const char* command : {"get password", "get guest.password", "get wifi.pwd", "get bridge.secret",
      "get discovery.node.responses", "get discovery.region.responses",
      "set discovery.node.responses off", "set discovery.region.responses off"}) {
    for (ClientInfo* sender : {&guest, &region, &filter, &combined}) denied(node, sender, command);
    allowed(node, &admin, command);
    allowed(node, nullptr, command);
  }

  // Closing the old portable bypass must not erase delegated manager roles.
  allowed(node, &region, "set flood.channel.scope public #local");
  allowed(node, &combined, "set flood.channel.scope public #local");
  denied(node, &filter, "set flood.channel.scope public #local");
  allowed(node, &filter, "set flood.filter.1 group-text 4");
  allowed(node, &combined, "set flood.filter.1 group-text 4");
  denied(node, &region, "set flood.filter.1 group-text 4");
#if MESH_ENABLE_FLOOD_RULE_ENGINE
  allowed(node, &filter, "get flood.rule");
  allowed(node, &filter, "set flood.rule.0 drop");
#else
  denied(node, &filter, "get flood.rule");
  denied(node, &filter, "set flood.rule.0 drop");
#endif
  puts("Legacy portable Morse authorization and manager delegation passed");
}
'''


class MorseAuthorizationTests(unittest.TestCase):
    def test_actual_legacy_portable_handoff_keeps_morse_admin_only(self):
        source = (ROOT / "examples/simple_repeater/MyMesh.cpp").read_text(encoding="utf-8")
        handler = extract_braced(source, "void MyMesh::handleCommand(uint32_t sender_timestamp,")
        start = handler.index("// Compatibility path for manually defined legacy portable builds.")
        call = "_cli.handleCommand(sender_timestamp, command, reply);"
        end = handler.index(call, start) + len(call)
        # Keep the actual statements between the stable handoff marker and
        # CommonCLI invocation. Do not inject a test-only authorization gate;
        # the previous unguarded production branch must fail the C++ asserts.
        handoff = handler[start:end]
        helpers = "\n".join(extract_braced(source, signature) for signature in (
            "static bool commandFamilyMatches(", "static bool commandListMatches(",
            "static bool isCommonManagerReadOnlyAllowed(",
            "static bool isRegionMgrAllowed(", "static bool isFilterMgrAllowed(",
        ))
        helpers = f'#include "{(ROOT / "src/helpers/FloodCommandAliases.h").as_posix()}"\n' + helpers
        code = HARNESS.replace("@MANAGER_HELPERS@", helpers).replace("@PORTABLE_HANDOFF@", handoff)
        for extended_filters in (0, 1):
            with self.subTest(extended_filters=extended_filters):
                compile_run(self, code, flags=(f"-DMESH_ENABLE_FLOOD_RULE_ENGINE={extended_filters}",))


if __name__ == "__main__":
    unittest.main()
