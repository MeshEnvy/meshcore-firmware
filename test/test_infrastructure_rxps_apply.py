"""Retained RXPS preferences must not block radios without RX duty cycling."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_repeater_radio_timing_integration import method

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <helpers/radiolib/RXPowerSaving.h>
#define MESH_DEBUG_PRINTLN(...) ((void)0)
#define WRAPPER_CLASS RadioDriver
struct RadioDriver {
  static constexpr bool SupportsRxPowerSaving = RXPS_SUPPORTED;
  bool supportsRxPowerSaving() const { return SupportsRxPowerSaving; }
} radio_driver;
namespace mesh { enum class RadioParamApplyResult { APPLIED, BUSY, FAILED }; }
struct CommonCLI {
  unsigned calls=0;
  bool got_timings=false, last_temporary=false;
  uint32_t rx_us=0, sleep_us=0;
  float applied_freq=910.525f;
  static bool calculateRxPowerSavingLevel(uint32_t,uint8_t,float,uint32_t,uint32_t*,uint32_t*);
  CommonCLI& radioProfiles() { return *this; }
  bool primaryRadioApplyReady(bool) { return true; }
  mesh::RadioParamApplyResult applyPrimary(float freq,float bw,uint8_t sf,uint8_t cr,
      bool temporary,uint16_t preamble,const uint32_t* timings) {
    assert(freq==910.539978f && bw==62.5f && sf==7 && cr==5 && preamble==32);
    assert(!timings || radio_driver.supportsRxPowerSaving());
    ++calls;applied_freq=freq;last_temporary=temporary;got_timings=timings;
    if(timings) { rx_us=timings[0];sleep_us=timings[1]; }
    return mesh::RadioParamApplyResult::APPLIED;
  }
};
@CALCULATE@
static constexpr uint32_t now_ms=3000;
uint32_t nextRadioApplyRetryDelay(uint8_t& failures) { ++failures;return 1000; }
struct Prefs {
  bool rx_powersaving_enabled=true;
  uint8_t rx_ps_level=8;
  uint16_t rx_ps_preamble=16;
  uint32_t rx_ps_rx_us=65625,rx_ps_sleep_us=60000;
};
struct Role {
  Prefs _prefs;
  CommonCLI _cli;
  uint32_t set_radio_at=2000,revert_radio_at=86402000,radio_apply_retry_at=0;
  uint8_t radio_apply_failures=0,active_cr=5,active_sf=7,pending_sf=7,pending_cr=5;
  uint16_t pending_preamble=32;
  float pending_freq=910.539978f,pending_bw=62.5f,active_bw=62.5f;
  bool temp_radio_applied=false,saved_radio_apply_pending=false;
  struct Manager { unsigned getOutboundTotal() const { return 0; } } manager;
  Manager* _mgr=&manager;
  bool hasOutbound() const { return false; }
  bool millisHasNowPassed(uint32_t at) const { return int32_t(now_ms-at)>=0; }
  uint32_t futureMillis(uint32_t delay) const { return now_ms+delay; }
  bool applySavedRadioParams() { assert(false);return false; }
  @METHOD@
  bool apply(bool temporary) { @APPLY@ }
};
int main() {
  uint32_t expected_rx=0,expected_sleep=0;
  bool supported=radio_driver.supportsRxPowerSaving();
  bool calculated=CommonCLI::calculateRxPowerSavingLevel(2,7,62.5f,
      rxPowerSavingPreambleForParams(7,62.5f),&expected_rx,&expected_sleep);
  assert(calculated==supported); // The actual calculator rejects unsupported hardware.
  for(bool temporary:{false,true}) for(bool enabled:{false,true})
      for(uint8_t level:{0,2,8,11}) for(uint16_t preamble:{0,16,32}) {
    Role node;
    node._prefs.rx_powersaving_enabled=enabled;
    node._prefs.rx_ps_level=level;
    node._prefs.rx_ps_preamble=preamble;
    const bool expected_ok=!supported || !enabled || level<=10;
    if(supported && enabled && level>0 && level<=10) {
      assert(CommonCLI::calculateRxPowerSavingLevel(level,7,62.5f,
          preamble ? preamble : rxPowerSavingPreambleForParams(7,62.5f),
          &expected_rx,&expected_sleep));
    }
    assert(node.apply(temporary)==expected_ok);
    assert(node._cli.calls==(expected_ok ? 1U : 0U));
    if(expected_ok) {
      assert(node._cli.applied_freq==910.539978f);
      assert(node._cli.got_timings==(supported && enabled));
      if(supported && enabled) {
        assert(node._cli.rx_us==(level==0 ? 65625 : expected_rx));
        assert(node._cli.sleep_us==(level==0 ? 60000 : expected_sleep));
      }
    }
    assert(node._prefs.rx_powersaving_enabled==enabled && node._prefs.rx_ps_level==level);
    assert(node._prefs.rx_ps_preamble==preamble);
    assert(node._prefs.rx_ps_rx_us==65625 && node._prefs.rx_ps_sleep_us==60000);
    assert(node.revert_radio_at==86402000); // Preserve the accepted 24-hour lease.
    @STATE@
  }
}
'''


class InfrastructureRxpsApplyTest(unittest.TestCase):
    def test_unsupported_rxps_does_not_block_primary_or_temporary_apply(self):
        calculate = method((ROOT / 'src/helpers/CommonCLI.cpp').read_text(),
                           'bool CommonCLI::calculateRxPowerSavingLevel(')
        for role, filename, cls in (
                ('simple_repeater', 'MyMesh.cpp', 'MyMesh'),
                ('simple_room_server', 'MyMesh.cpp', 'MyMesh'),
                ('simple_sensor', 'SensorMesh.cpp', 'SensorMesh')):
            source = (ROOT / 'examples' / role / filename).read_text()
            if role == 'simple_repeater':
                production = method(source, f'bool {cls}::applyRadioParams(')
                apply = 'return applyRadioParams(910.539978f,62.5f,7,5,32,temporary);'
                state = 'if(expected_ok) assert(node._cli.last_temporary==temporary);'
            else:
                production = method(source, f'void {cls}::servicePendingPrimaryRadioSettings(')
                apply = '(void)temporary;servicePendingPrimaryRadioSettings(false);return temp_radio_applied;'
                state = '''
    assert(node.temp_radio_applied==expected_ok);
    assert(node.set_radio_at==(expected_ok ? 0U : 2000U));
    assert(node.saved_radio_apply_pending==!expected_ok);
    assert(node.radio_apply_retry_at==(expected_ok ? 0U : 4000U));
    if(expected_ok) assert(node._cli.last_temporary);
'''
            production = production.replace(cls + '::', '')
            code = HARNESS.replace('@CALCULATE@', calculate).replace('@METHOD@', production)
            code = code.replace('@APPLY@', apply).replace('@STATE@', state)
            for supported in (False, True):
                with self.subTest(role=role, supported=supported):
                    self.run_cpp(code, supported)
            # Prove this fixture catches the original early rejection, rather
            # than merely compiling a driver that always accepts the tuple.
            mutated = code.replace('if (apply_rxps && _prefs.rx_ps_level != 0)',
                                   'if (_prefs.rx_powersaving_enabled && _prefs.rx_ps_level != 0)')
            self.assertNotEqual(mutated, code)
            with self.subTest(role=role, negative_control=True):
                self.run_cpp(mutated, False, succeeds=False)

    def run_cpp(self, code, supported, succeeds=True):
        with tempfile.TemporaryDirectory(prefix='meshcore-rxps-apply-') as folder:
            cpp, binary = Path(folder) / 'test.cpp', Path(folder) / 'test'
            cpp.write_text(code)
            built = subprocess.run([
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-fno-pie', '-no-pie', f'-DRXPS_SUPPORTED={int(supported)}',
                '-I', str(ROOT / 'src'), str(cpp), '-o', str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            if succeeds:
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            else:
                self.assertNotEqual(ran.returncode, 0, 'negative control unexpectedly passed')
                self.assertIn('Assertion', ran.stderr)


if __name__ == '__main__':
    unittest.main()
