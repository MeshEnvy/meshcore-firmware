"""Companion receive-profile byte: raw and channel data frames say which
profile a packet arrived on (0xA0 primary, 0xA1 radio2) while radio2 is on,
and keep the stock reserved value otherwise or for UDP-bridge ingress."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from test_replay_reset_integration import extract_braced
from test_serial_wifi_sessions import SANITIZER_FLAGS

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <RadioProfiles.h>
namespace mesh {
struct Packet {
  uint8_t radio_origin = 0;
#if defined(WITH_UDP_BRIDGE)
  bool udp_bridge_ingress = false;
#endif
};
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
  bridged.radio_origin = 1;
#if defined(WITH_UDP_BRIDGE)
  bridged.udp_bridge_ingress = true;
#endif

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
    for (uint8_t none : {uint8_t{0}, uint8_t{0xFF}}) {
      assert(mesh_.rxViaTag(&primary, none) == 0xA0);
      assert(mesh_.rxViaTag(&second, none) == 0xA1);
#if defined(WITH_UDP_BRIDGE)
      assert(mesh_.rxViaTag(&bridged, none) == none); // UDP is not a radio profile.
#else
      assert(mesh_.rxViaTag(&bridged, none) == 0xA1);
#endif
    }
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
            binary = work / 'test.exe'
            for udp_bridge in (False, True):
                with self.subTest(udp_bridge=udp_bridge):
                    flags = ['-DWITH_UDP_BRIDGE'] if udp_bridge else []
                    compiled = subprocess.run([
                        os.environ.get('CXX', 'g++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        *flags, *SANITIZER_FLAGS, f'-I{ROOT / "src"}', str(cpp), '-o', str(binary)],
                        capture_output=True, text=True, timeout=60)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    checked = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                    self.assertEqual(checked.returncode, 0, checked.stderr)


if __name__ == '__main__':
    unittest.main()
