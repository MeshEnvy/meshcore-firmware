"""Companion receive-profile byte: raw and channel data frames say which
profile a packet arrived on (0xA0 primary, 0xA1 radio2) while radio2 is on,
and keep the stock reserved value otherwise or for UDP-bridge ingress."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

from test_replay_reset_integration import extract_braced

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <RadioProfiles.h>
namespace mesh {
struct Packet { uint8_t radio_origin = 0; bool udp_bridge_ingress = false; };
struct Radio {
  RadioProfiles* p = nullptr;
  const RadioProfiles* profiles() const { return p; }
};
}
struct MyMesh {
  mesh::Radio* _radio = nullptr;
  uint8_t rxViaTag(const mesh::Packet* pkt, uint8_t none) const;
};
@METHOD@
int main() {
  mesh::RadioProfiles profiles;
  mesh::Radio radio;
  MyMesh mesh_;
  mesh::Packet primary, second, bridged;
  second.radio_origin = 1;
  bridged.radio_origin = 1; bridged.udp_bridge_ingress = true;

  // No radio, no profiles, radio2 off: the stock reserved values.
  assert(mesh_.rxViaTag(&primary, 0xFF) == 0xFF);
  mesh_._radio = &radio;
  assert(mesh_.rxViaTag(&second, 0) == 0);
  radio.p = &profiles;
  profiles.secondary.mode = mesh::RadioProfileMode::Off;
  assert(mesh_.rxViaTag(&second, 0xFF) == 0xFF);
  assert(mesh_.rxViaTag(nullptr, 0) == 0);

  // radio2 listening or sending: tagged by the profile it arrived on.
  for (auto mode : {mesh::RadioProfileMode::Rx, mesh::RadioProfileMode::RxTx}) {
    profiles.secondary.mode = mode;
    assert(mesh_.rxViaTag(&primary, 0xFF) == 0xA0);
    assert(mesh_.rxViaTag(&second, 0) == 0xA1);
    assert(mesh_.rxViaTag(&bridged, 0) == 0);   // came over the UDP bridge, not a radio
  }
  return 0;
}
'''


class CompanionRxViaTest(unittest.TestCase):
    def test_rx_via_tag(self):
        source = (ROOT / 'examples/companion_radio/MyMesh.cpp').read_text(encoding='utf-8')
        method = extract_braced(source, 'uint8_t MyMesh::rxViaTag(')
        self.assertIn('rxViaTag(pkt, 0)', source)        # channel data, reserved1
        self.assertIn('rxViaTag(packet, 0xFF)', source)  # raw data, reserved
        with tempfile.TemporaryDirectory(prefix='companion-rx-via-') as directory:
            work = Path(directory)
            cpp = work / 'test.cpp'
            cpp.write_text(HARNESS.replace('@METHOD@', method), encoding='utf-8')
            flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all'] if sys.platform.startswith('linux') else []
            binary = work / 'test.exe'
            compiled = subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                       '-DWITH_UDP_BRIDGE', *flags, f'-I{ROOT / "src"}', str(cpp), '-o', str(binary)],
                                      capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            checked = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(checked.returncode, 0, checked.stderr)


if __name__ == '__main__':
    unittest.main()
