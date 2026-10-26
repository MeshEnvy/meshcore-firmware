"""Exercise actual LR2021 activity/admission/primary transaction under RX noise."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from test_radio_profile_scan import HARNESS
from test_radio_receive_contract import method

ROOT = Path(__file__).resolve().parents[1]

MAIN = r'''
static void noise(RadioLibWrapper& w, unsigned tick) {
  w.chip.irq = (tick & 1) ? RADIOLIB_LR2021_IRQ_LORA_HEADER_VALID
                           : RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED;
}
int main() {
  using Result = mesh::RadioParamApplyResult;
  for (bool temporary : {false, true}) {
    RadioLibWrapper w;
    auto target = w._profiles.primary; target.freq = 910.54f;
    bool applied = false;
    for (unsigned tick = 0; tick < 751; ++tick) {
      elapsed_us = 100000 + tick * 40000ULL; noise(w, tick);
      auto result = w.trySetPrimaryParams(target, temporary);
      if (result == Result::APPLIED) { applied = true; break; }
      assert(result == Result::BUSY && w.applies == 0 && !w._primary_receive_preempt);
    }
    assert(applied && w.applies == 1 && w._cur_freq == target.freq);
    assert(!w._primary_receive_preempt && w._profiles.primary_temporary == temporary);
  }
  // A different accepted tuple restarts the wait; invalid edits do not arm it.
  {
    RadioLibWrapper w;
    auto target = w._profiles.primary; target.freq = 910.54f;
    noise(w, 0); assert(w.trySetPrimaryParams(target, true) == Result::BUSY);
    elapsed_us += 29000000; noise(w, 1);
    target.freq = 911; assert(w.trySetPrimaryParams(target, true) == Result::BUSY);
    assert(w.applies == 0);
  }
  // Even after its bound, no ordinary settings/scan/TX entry gets an exception.
  {
    RadioLibWrapper w;
    auto target = w._profiles.primary; target.freq = 910.54f;
    for (unsigned tick = 0; tick < 1500; ++tick) {
      elapsed_us = 100000 + tick * 40000ULL; noise(w, tick);
      assert(w.beginReconfigure() == 2);
      assert(w.trySetParams(target.freq,target.bw,target.sf,target.cr) == Result::BUSY);
    }
    assert(w.applies == 0 && w.chip.standbys == 0);
  }
  // An opted-out wrapper retains its existing non-preemptive behavior.
  {
    RadioLibWrapper w; w.bounded = false;
    auto target = w._profiles.primary; target.freq = 910.54f;
    for (unsigned tick = 0; tick < 1500; ++tick) {
      elapsed_us = 100000 + tick * 40000ULL; noise(w, tick);
      assert(w.trySetPrimaryParams(target, true) == Result::BUSY);
    }
    assert(w.applies == 0 && w.chip.standbys == 0);
  }
  // Completion, chip BUSY and every exclusive owner remain stronger than the
  // bounded command. Once each guard clears the accepted tuple still applies.
  for (unsigned guard = 0; guard < 7; ++guard) {
    RadioLibWrapper w;
    auto target = w._profiles.primary; target.freq = 910.54f;
    noise(w, 0); assert(w.trySetPrimaryParams(target, false) == Result::BUSY);
    elapsed_us += 31000000; noise(w, 1);
    if (guard == 0) w.events.record();
    if (guard == 1) w.busy = true;
    if (guard == 2) w.state = STATE_TX_WAIT;
    if (guard == 3) w._cw_active = true;
    if (guard == 4) w._beacon_owned = true;
    if (guard == 5) w._morse_owned = true;
    if (guard == 6) w._morse_rx_owned = true;
    assert(w.trySetPrimaryParams(target, false) == Result::BUSY);
    assert(w.applies == 0 && w.chip.standbys == 0);
    w.events.claim(); w.busy = w._cw_active = w._beacon_owned = false;
    w._morse_owned = w._morse_rx_owned = false; w.state = STATE_RX;
    assert(w.trySetPrimaryParams(target, false) == Result::APPLIED);
  }
  // A completion racing the activity probe or the actual standby retains its
  // old tuple/FIFO; neither settings apply nor receive rearm may consume it.
  for (bool during_standby : {false,true}) {
    RadioLibWrapper w;
    auto target = w._profiles.primary; target.freq = 910.54f;
    noise(w, 0); assert(w.trySetPrimaryParams(target, false) == Result::BUSY);
    elapsed_us += 31000000; noise(w, 1);
    if (during_standby) w.chip.standby_irq = &w.events;
    else w.activity_irq = true;
    assert(w.trySetPrimaryParams(target, false) == Result::BUSY);
    assert(w.events.pending() && w.applies == 0 && w._cur_freq != target.freq);
    assert(w.chip.standbys == unsigned(during_standby));
    w.events.claim(); w.activity_irq = false; w.chip.standby_irq = nullptr;
    assert(w.trySetPrimaryParams(target, false) == Result::APPLIED);
  }
  // Hardware failure and failed RX resume keep ordinary rollback semantics.
  for (unsigned failure = 0; failure < 3; ++failure) {
    RadioLibWrapper w;
    auto original = w._profiles.primary;
    auto target = original; target.freq = 910.54f;
    noise(w, 0); assert(w.trySetPrimaryParams(target, true) == Result::BUSY);
    elapsed_us += 31000000; noise(w, 1);
    if (failure == 0) w.chip.standby_error = -706;
    if (failure == 1) w.fail = true;
    if (failure == 2) w.failRxStarts = 1;
    assert(w.trySetPrimaryParams(target, true) == Result::FAILED);
    assert(w._profiles.primary == original && !w._profiles.primary_temporary);
    assert(w._cur_freq == original.freq && !w._primary_receive_preempt);
    w.chip.standby_error = 0; w.fail = false;
    assert(w.trySetPrimaryParams(target, true) == Result::APPLIED);
  }
  // The public deadline helper handles long-airtime cap and millis rollover.
  {
    mesh::PrimaryRadioApplyWait wait;
    mesh::RadioProfileParams slow; slow.freq=910; slow.bw=7.8f; slow.sf=12; slow.cr=8;
    const uint32_t start = UINT32_MAX - 10000;
    assert(!wait.mayAbandonReceive(start,slow,false,slow,65528,65528,255));
    assert(!wait.mayAbandonReceive(start+29999,slow,false,slow,65528,65528,255));
    assert(wait.mayAbandonReceive(start+30000,slow,false,slow,65528,65528,255));
    wait.clear();
    assert(!wait.mayAbandonReceive(start+30001,slow,false,slow,65528,65528,255));
  }
}
'''


class LR2021PrimaryApplyWaitTests(unittest.TestCase):
    def test_actual_activity_admission_primary_transaction(self):
        wrapper = (ROOT / 'src/helpers/radiolib/RadioLibWrappers.cpp').read_text()
        native = (ROOT / 'src/helpers/radiolib/CustomLR2021.h').read_text()
        signatures = ('uint8_t RadioLibWrapper::beginReconfigure(',
                      'void RadioLibWrapper::endReconfigure(',
                      'mesh::RadioParamApplyResult RadioLibWrapper::trySetParams(',
                      'mesh::RadioParamApplyResult RadioLibWrapper::trySetPrimaryParams(')
        methods = '\n'.join(method(wrapper, signature) for signature in signatures)
        template = HARNESS[:HARNESS.index('int main()')]
        template = template.replace('#include <cassert>', '''#include <cassert>
#define USE_LR2021 1
#define MESH_ENABLE_TX_BEACONS 1
#define MESH_ENABLE_CARRIER_TOOLS 1
#define MESH_ENABLE_MORSE_RX 1
#define MAX_TRANS_UNIT 255
#define RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED 32
#define RADIOLIB_LR2021_IRQ_LORA_HEADER_VALID 64
#define RADIOLIB_LR2021_IRQ_LORA_HDR_CRC_ERROR 512
#include <helpers/radiolib/PrimaryRadioApplyWait.h>
''')
        chip = '''struct Chip {
  bool standbyXOSC=false, _headerSeen=false;
  uint32_t _activityAt=0, _preambleMillis=66, _maxPayloadMillis=3934, irq=0;
  unsigned standbys=0;
  int standby_error=0;
  RadioInterruptEvents* standby_irq=nullptr;
  bool isChipBusy(){return false;}
  uint32_t getIrqStatus(){return irq;}
  int clearIrqFlags(uint32_t flags){irq &= ~flags; return 0;}
  int standby(){++standbys;if(standby_irq)standby_irq->record();return standby_error;}
  @ACTIVITY@
};'''.replace('@ACTIVITY@',method(native,'bool isReceiving()'))
        template = template.replace('struct Chip { bool standbyXOSC=false; int standby() { return 0; } };',chip)
        template = template.replace('  bool _cw_active=false;', '''  bool _cw_active=false;
  bool _beacon_owned=false, _beacon_restoring=false, _morse_owned=false, _morse_rx_owned=false;
  bool _primary_receive_preempt=false, bounded=true, activity_irq=false;
  mesh::PrimaryRadioApplyWait _primary_apply_wait;
  bool supportsBoundedPrimaryReconfigure() const { return bounded; }
  void noteReconfigureAdmission(const char*,int16_t) {}
  uint16_t currentPreambleLength()const{return _physical_preamble;}
''')
        template = template.replace('bool isReceivingPacket() { return packet; }',
                                    'bool isReceivingPacket() { if(activity_irq)events.record();return chip.isReceiving(); }')
        code = template.replace('@METHODS@',methods) + MAIN
        with tempfile.TemporaryDirectory() as folder:
            cpp, exe = Path(folder)/'test.cpp', Path(folder)/'test'
            (Path(folder)/'Arduino.h').write_text('#pragma once\n#include <cstdint>\n#include <cmath>\n')
            cpp.write_text(code)
            result = subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-Wall','-Wextra',
                                     '-fsanitize=address,undefined','-fno-pie','-no-pie',
                                     '-I',folder,'-I',str(ROOT/'src'),str(cpp),'-o',str(exe)],
                                    capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result = subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            # The regression must fail with either the former unbounded
            # activity gate or a removed completion guard, not just compile.
            mutations = (
                ('isReceivingPacket() && !_primary_receive_preempt', 'isReceivingPacket()'),
                ('if (hasPendingRadioInterrupt())', 'if (false)'),
            )
            for old, replacement in mutations:
                self.assertIn(old, code)
                cpp.write_text(code.replace(old,replacement))
                result = subprocess.run([os.environ.get('CXX','g++'),'-std=c++17',
                                         '-I',folder,'-I',str(ROOT/'src'),str(cpp),'-o',str(exe)],
                                        capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                result = subprocess.run([str(exe)],capture_output=True,text=True)
                self.assertNotEqual(result.returncode,0,'negative control unexpectedly passed')
                self.assertIn('Assertion',result.stderr)


if __name__ == '__main__':
    unittest.main()
