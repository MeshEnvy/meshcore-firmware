"""Real trim transaction and LR2021 RX-noise admission, with completed IRQ races."""
from pathlib import Path
import subprocess
import tempfile
import unittest

import test_lr2021_apply_transaction as transaction_fixture
from test_lr2021_apply_transaction import method

ROOT = Path(__file__).resolve().parents[1]

MAIN = r'''
static void noise(Wrapper& w,unsigned tick) {
  w.chip.irq=(tick&1)?RADIOLIB_LR2021_IRQ_LORA_HEADER_VALID:RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED;
}
static void arm(Wrapper& w) {
  now_ms=100;noise(w,0);
  assert(w.trySetXoscTrim(true,8,8)==mesh::RadioParamApplyResult::BUSY);
}
int main(){
 using Result=mesh::RadioParamApplyResult;
 {
  Wrapper w;bool applied=false;
  for(unsigned tick=0;tick<751;++tick){
   now_ms=100+tick*40;noise(w,tick);
   const auto result=w.trySetXoscTrim(true,8,8);
   if(result==Result::APPLIED){applied=true;break;}
   assert(result==Result::BUSY&&!w._primary_receive_preempt&&w.chip.commands==0);
  }
  assert(applied&&w.chip.enabled&&w.chip.a==8&&w.chip.b==8&&w.rearms==1);
  assert(!w._primary_receive_preempt&&w._trim_apply_key==UINT16_MAX);
 }
 // Ordinary reconfiguration never inherits the expired accepted-trim lease.
 {
  Wrapper w;arm(w);
  for(unsigned tick=1;tick<1500;++tick){
   now_ms=100+tick*40;noise(w,tick);assert(w.beginReconfigure()==2);
  }
  assert(w.chip.standbys==0&&w.chip.commands==0);
 }
 // Every protected owner and completed packet remains stronger than the bound.
 for(unsigned guard=0;guard<7;++guard){
  Wrapper w;arm(w);now_ms+=31000;noise(w,1);
  if(guard==0)w.pending=true;
  if(guard==1)w.busy=true;
  if(guard==2)w.state=STATE_TX_WAIT;
  if(guard==3)w._cw_active=true;
  if(guard==4)w._beacon_owned=true;
  if(guard==5)w._morse_owned=true;
  if(guard==6)w._morse_rx_owned=true;
  assert(w.trySetXoscTrim(true,8,8)==Result::BUSY);
  assert(w.chip.commands==0&&w.chip.standbys==0&&!w._primary_receive_preempt);
  w.pending=w.busy=w._cw_active=w._beacon_owned=w._morse_owned=w._morse_rx_owned=false;
  w.state=STATE_RX;
  // Morse is checked before the timer starts, so release then permit its own
  // complete bound instead of borrowing another owner's elapsed time.
  auto result=w.trySetXoscTrim(true,8,8);
  if(result==Result::BUSY){now_ms+=31000;noise(w,0);result=w.trySetXoscTrim(true,8,8);}
  assert(result==Result::APPLIED);
 }
 // Completion during the probe or standby preserves the old packet and trim.
 for(bool during_standby:{false,true}){
  Wrapper w;arm(w);now_ms+=31000;noise(w,1);
  if(during_standby)w.chip.standby_irq=&w.pending;else w.activity_irq=true;
  assert(w.trySetXoscTrim(true,8,8)==Result::BUSY);
  assert(w.pending&&w.chip.commands==0&&!w.chip.enabled&&!w._primary_receive_preempt);
  assert(w.chip.standbys==unsigned(during_standby));
  w.pending=w.activity_irq=false;w.chip.standby_irq=nullptr;
  assert(w.trySetXoscTrim(true,8,8)==Result::APPLIED);
 }
 // Replacing the requested capacitor codes must restart the admission wait.
 {
  Wrapper w;arm(w);now_ms+=31000;noise(w,1);
  assert(w.trySetXoscTrim(true,9,10)==Result::BUSY);
  assert(w.chip.commands==0);
  now_ms+=31000;noise(w,0);
  assert(w.trySetXoscTrim(true,9,10)==Result::APPLIED&&w.chip.a==9&&w.chip.b==10);
 }
 // Timeout/cancellation followed by an identical retry earns a fresh wait.
 {
  Wrapper w;arm(w);now_ms+=31000;noise(w,1);w.pending=true;
  assert(w.trySetXoscTrim(true,8,8)==Result::BUSY);
  w.cancelXoscTrimApply();w.pending=false;
  assert(w.trySetXoscTrim(true,8,8)==Result::BUSY&&w.chip.commands==0);
  now_ms+=31000;noise(w,0);
  assert(w.trySetXoscTrim(true,8,8)==Result::APPLIED);
 }
 // A scope restores its caller's admission flag on both guarded and successful
 // returns, rather than clearing a surrounding accepted-primary transaction.
 for(bool guarded:{false,true}){
  Wrapper w;w._primary_receive_preempt=true;w.pending=guarded;
  assert(w.trySetXoscTrim(true,8,8)==(guarded?Result::BUSY:Result::APPLIED));
  assert(w._primary_receive_preempt);
 }
}
'''


class Capture:
    def run_cpp(self, code):
        self.code = code


class LR2021TrimApplyWaitTests(unittest.TestCase):
    def test_actual_trim_activity_admission_and_completion_guards(self):
        captured = Capture()
        transaction_fixture.LR2021ApplyTransactionTest.test_real_trim_transaction_restores_selection_modem_and_receiver(captured)
        code = captured.code[:captured.code.index('int main()')]
        code = code.replace('#include <cassert>', '''#include <cassert>
#define USE_LR2021 1
#define MESH_ENABLE_TX_BEACONS 1
#define STATE_IDLE 0
#define STATE_RX 1
#define STATE_TX_WAIT 3
#define RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED 32
#define RADIOLIB_LR2021_IRQ_LORA_HEADER_VALID 64
#define RADIOLIB_LR2021_IRQ_LORA_HDR_CRC_ERROR 512
#define MESH_DEBUG_PRINTLN(...) ((void)0)
''')
        native = (ROOT/'src/helpers/radiolib/CustomLR2021.h').read_text()
        code = code.replace('struct CustomLR2021 {', '''struct CustomLR2021 {
  uint32_t _activityAt=0,_preambleMillis=66,_maxPayloadMillis=3934,irq=0;
  bool _headerSeen=false;
  unsigned standbys=0;
  bool* standby_irq=nullptr;
  bool isChipBusy(){return false;}
  uint32_t getIrqStatus(){return irq;}
  int clearIrqFlags(uint32_t flags){irq &= ~flags;return 0;}
  int standby(){++standbys;if(standby_irq)*standby_irq=true;return 0;}
''' + method(native,'bool isReceiving()'))
        code = code.replace('  bool _primary_receive_preempt=false;', '''  bool _primary_receive_preempt=false;
  bool pending=false,busy=false,activity_irq=false;
  bool _cw_active=false,_beacon_owned=false,_beacon_restoring=false;
  bool _rx_hold_continuous=false,_rx_ps_armed=false;
  uint8_t state=STATE_RX;
  bool hasPendingRadioInterrupt()const{return pending;}
  bool claimRadioInterrupt(){const bool had=pending;pending=false;return had;}
  bool isChipBusy()const{return busy;}
  bool isReceivingPacket(){if(activity_irq)pending=true;return chip.isReceiving();}
''')
        wrapper = (ROOT/'src/helpers/radiolib/RadioLibWrappers.cpp').read_text()
        admission = method(wrapper,'uint8_t RadioLibWrapper::beginReconfigure()').replace('RadioLibWrapper::','')
        code = code.replace(method(code,'uint8_t beginReconfigure()'),admission)
        code = code.replace('else rx=true;','else {rx=true;state=STATE_RX;}')
        code = code.replace('bool isInRecvMode()const{return rx;}','bool isInRecvMode()const{return state==STATE_RX;}')
        code += MAIN
        with tempfile.TemporaryDirectory(prefix='lr2021-trim-admission-') as folder:
            cpp,exe=Path(folder)/'test.cpp',Path(folder)/'test'
            for mutation in (False,True):
                cpp.write_text(code.replace('isReceivingPacket() && !_primary_receive_preempt',
                                            'isReceivingPacket()') if mutation else code)
                built=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                                      '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                                      '-fno-pie','-no-pie','-I',str(ROOT/'src'),str(cpp),'-o',str(exe)],
                                     capture_output=True,text=True)
                self.assertEqual(built.returncode,0,built.stderr)
                ran=subprocess.run([str(exe)],capture_output=True,text=True)
                if mutation:
                    self.assertNotEqual(ran.returncode,0,'unbounded admission negative control passed')
                    self.assertIn('Assertion',ran.stderr)
                else:
                    self.assertEqual(ran.returncode,0,ran.stderr)


if __name__=='__main__':
    unittest.main()
