"""Execute LR1121 RXPS production methods with faultable RadioLib doubles."""

from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

from cpp_source import body


ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <helpers/radiolib/RXPowerSaving.h>
#include <helpers/radiolib/RadioRegulatorState.h>

#define MESH_DEBUG_PRINTLN(...) ((void)0)
#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_ERR_UNSUPPORTED -2
#define RADIOLIB_ERR_INVALID_RX_PERIOD -3
#define RADIOLIB_ERR_INVALID_SLEEP_PERIOD -4
#define RADIOLIB_ERR_UNKNOWN -5
#define RADIOLIB_NC 0xffffffffUL
#define RADIOLIB_LR11X0_STANDBY_RC 0
#define RADIOLIB_LR11X0_LF_CLK_RC 0
#define RADIOLIB_LR11X0_LF_BUSY_RELEASE_DISABLED 0
#define RADIOLIB_LR11X0_LF_BUSY_RELEASE_ENABLED 16
#define RADIOLIB_LR11X0_RX_DUTY_CYCLE_MODE_RX 1
#define RADIOLIB_LR11X0_RX_TIMEOUT_INF 0xffffff
#define RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE 0x12
#define RADIOLIB_LR11X0_IRQ_ALL 0xffffffffUL
#define RADIOLIB_LR11X0_IRQ_PREAMBLE_DETECTED (1UL << 4)
#define RADIOLIB_LR11X0_IRQ_SYNC_WORD_HEADER_VALID (1UL << 5)
#define RADIOLIB_LR11X0_IRQ_HEADER_ERR (1UL << 6)
#define RADIOLIB_IRQ_RX_DONE 2
#define RADIOLIB_IRQ_PREAMBLE_DETECTED 4
#define RADIOLIB_IRQ_HEADER_ERR 6
#define RADIOLIB_IRQ_RX_DEFAULT_FLAGS (1UL << RADIOLIB_IRQ_RX_DONE)
#define RADIOLIB_IRQ_RX_DEFAULT_MASK (1UL << RADIOLIB_IRQ_RX_DONE)
#define RADIOLIB_RADIO_MODE_RX 1
#define RADIOLIB_ASSERT(state) do { if ((state) != 0) return (state); } while (0)
#define STATE_IDLE 0
#define STATE_RX 1
#define STATE_TX_WAIT 3

using RadioLibTime_t = uint32_t;
using RadioLibIrqFlags_t = uint32_t;
enum class ModemType_t { RADIOLIB_MODEM_LORA };
struct RadioModeConfig_t {
  struct { uint32_t timeout, irqFlags, irqMask; size_t len; } receive;
};
static uint32_t now_ms = 100;
static unsigned delays;
uint32_t millis() { return now_ms; }
void delay(uint32_t value) { assert(value == 50); ++delays; }

struct Hal {
  bool busy = false;
  unsigned reads = 0;
  bool digitalRead(uint32_t pin) { assert(pin == 9); ++reads; return busy; }
};
struct Module {
  enum { MODE_RX = 1 };
  Hal hal_value;
  Hal* hal = &hal_value;
  std::string* trace = nullptr;
  uint32_t gpio = 9;
  uint32_t getGpio() { return gpio; }
  void setRfSwitchState(unsigned mode) { assert(mode == MODE_RX && trace); *trace += 'W'; }
};

class LR1121 {
public:
  virtual ~LR1121() = default;
  Module module;
  Module* mod = &module;
  float tcxoVoltage = 1.8f;
  uint32_t tcxoDelay = 5000;
  uint8_t spreadingFactor = 8;
  float bandwidthKhz = 62.5f;
  uint16_t preambleLengthLoRa = 32;
  std::string trace;
  int16_t standby_error = 0, lf_error = 0, stage_error = 0, duty_error = 0;
  int16_t buffer_error = 0, irq_error = 0, continuous_error = 0;
  int16_t tcxo_error = 0, calibration_error = 0, begin_error = 0;
  uint32_t programmed_rx = 0, programmed_sleep = 0, irq = 0;
  uint32_t continuous_flags = 0, continuous_mask = 0;
  unsigned stage_calls = 0, duty_calls = 0, irq_reads = 0, irq_clears = 0;
  unsigned base_length_calls = 0, modem_queries = 0, toa_calls = 0;
  unsigned tcxo_calls = 0, calibration_calls = 0;
  uint32_t tcxo_programmed = 0;
  size_t stale_length = 17;
  RadioModeConfig_t staged = {};
  LR1121() { module.trace = &trace; }

  int16_t standby(uint8_t mode = RADIOLIB_LR11X0_STANDBY_RC) {
    assert(mode == RADIOLIB_LR11X0_STANDBY_RC);
    trace += 'S'; return standby_error;
  }
  int16_t configLfClock(uint8_t setup) {
    assert(setup == (RADIOLIB_LR11X0_LF_CLK_RC | RADIOLIB_LR11X0_LF_BUSY_RELEASE_ENABLED)
        || setup == (RADIOLIB_LR11X0_LF_CLK_RC | RADIOLIB_LR11X0_LF_BUSY_RELEASE_DISABLED));
    trace += setup & RADIOLIB_LR11X0_LF_BUSY_RELEASE_ENABLED ? 'L' : 'R';
    return lf_error;
  }
  int16_t stageMode(int mode, RadioModeConfig_t* config) {
    assert(mode == RADIOLIB_RADIO_MODE_RX);
    trace += 'G'; ++stage_calls; staged = *config; return stage_error;
  }
  int16_t setRxDutyCycle(uint32_t rx, uint32_t sleep, uint8_t mode) {
    assert(mode == RADIOLIB_LR11X0_RX_DUTY_CYCLE_MODE_RX);
    trace += 'D'; ++duty_calls; programmed_rx = rx; programmed_sleep = sleep;
    return duty_error;
  }
  int16_t clearRxBuffer() { trace += 'B'; stale_length = 0; return buffer_error; }
  int16_t clearIrqState(uint32_t bits) {
    trace += 'C'; ++irq_clears; irq &= ~bits; return irq_error;
  }
  uint32_t getIrqStatus() { ++irq_reads; return irq; }
  int16_t checkIrq(unsigned bit) { return (getIrqStatus() & (1UL << bit)) ? 1 : 0; }
  virtual size_t getPacketLength(bool) { ++base_length_calls; return stale_length; }
  virtual int16_t startReceive() { return startReceive(0, 0, 0, 0); }
  int16_t startReceive(uint32_t, uint32_t flags, uint32_t mask, size_t) {
    trace += 'X'; continuous_flags = flags; continuous_mask = mask;
    return continuous_error;
  }
  virtual RadioLibTime_t getTimeOnAir(size_t) { ++modem_queries; return 0xffffffffUL; }
  RadioLibTime_t getToA(size_t length, ModemType_t modem) {
    assert(modem == ModemType_t::RADIOLIB_MODEM_LORA);
    ++toa_calls; return uint32_t(length * 900);
  }
  int16_t setTCXO(float voltage, uint32_t value) {
    assert(voltage == tcxoVoltage); trace += 'T'; ++tcxo_calls;
    tcxo_programmed = value;
    if (!tcxo_error) tcxoDelay = value;
    return tcxo_error;
  }
  int16_t calibrate(uint8_t mask) {
    assert(mask == 0x3f); trace += 'A'; ++calibration_calls;
    return calibration_error;
  }
  int16_t begin(float, float, uint8_t, uint8_t, uint8_t, int8_t, uint16_t, float voltage) {
    trace += 'I'; tcxoVoltage = voltage; tcxoDelay = voltage > 0 ? 5000 : 0;
    return begin_error;
  }
  int16_t setRegulatorLDO() { trace += 'O'; return 0; }
  int16_t setRegulatorDCDC() { trace += 'V'; return 0; }
};

class CustomLR1121 : public LR1121 {
public:
  mesh::power::RadioRegulatorState _regulator{
      mesh::power::REGULATOR_DCDC | mesh::power::REGULATOR_LDO,
      mesh::power::RegulatorMode::Dcdc};
  uint32_t _preambleMillis = 66, _maxPayloadMillis = 3934, _activityAt = 0;
  bool _headerSeen = false, _lfClockRestoreNeeded = false;
  @DRIVER_METHODS@
};

namespace mesh { enum class RadioParamApplyResult : uint8_t { APPLIED, BUSY, FAILED }; }
struct Board {
  bool finish_ok = true;
  unsigned finish_calls = 0;
  bool finishRadioHardReset() { ++finish_calls; return finish_ok; }
};
struct RadioLibWrapper {
  virtual ~RadioLibWrapper() = default;
  static constexpr bool SupportsRxPowerSaving = false;
  CustomLR1121* _radio;
  Board board;
  Board* _board = &board;
  bool prepare_ok = true, hard_reset_path = true;
  unsigned prepare_calls = 0;
  bool _rx_ps_enabled = true, _rx_ps_armed = true, _rx_hold_continuous = true;
  bool _rx_ps_continuous_fallback = false, _nf_calib_active = false;
  bool _profile_rxps_suspended = false, _profile_saved_rxps = false;
  bool _profile_repair_required = false, _cw_active = false, _beacon_owned = false;
  bool _loop_event_pending = false, interrupt_pending = false;
  uint8_t _irq_probe_failures = 0, state = STATE_RX;
  uint32_t _rx_ps_rx_us = 78000, _rx_ps_sleep_us = 26851;
  int16_t _rx_ps_last_error = 0;
  unsigned n_recv_errors = 0, reconfigure_calls = 0, rearm_calls = 0;
  RxPowerSavingArmRetryState _rx_ps_arm_retry;
  explicit RadioLibWrapper(CustomLR1121& value) : _radio(&value) {}
  virtual bool supportsRxPowerSaving() const { return false; }
  virtual uint32_t rxPowerSavingTransitionUs() const { return 0; }
  virtual bool isChipBusy() { return false; }
  virtual bool isReceivingPacket() { return false; }
  virtual int startReceiveMode() { return -1; }
  virtual int16_t armDutyCycle(RadioLibIrqFlags_t, RadioLibIrqFlags_t, uint32_t*, uint32_t*) { return -1; }
  virtual int16_t stopDutyCycleHardware() { return -1; }
  virtual int16_t startContinuousReceive() { return -1; }
  virtual void onReceiveProcessed() {}
  virtual bool radioDeepInit() { return false; }
  virtual bool supportsRadioDeepInit() const { return false; }
  bool prepareRadioHardReset() { ++prepare_calls; return prepare_ok; }
  bool supportsRadioHardResetPath() const { return hard_reset_path; }
  bool hasPendingRadioInterrupt() const { return interrupt_pending; }
  uint8_t beginReconfigure() { ++reconfigure_calls; return 1; }
  void endReconfigure(uint8_t) { ++reconfigure_calls; }
  void startRecv() { ++rearm_calls; }
  int startReceivePowerSaving() { return 77; }
  @BASE_METHODS@
};
struct CustomLR1121Wrapper : RadioLibWrapper {
  using DeepInitCallback = bool (*)();
  DeepInitCallback _deep_init = nullptr;
  explicit CustomLR1121Wrapper(CustomLR1121& value) : RadioLibWrapper(value) {}
  @CAPABILITY@
  @WRAPPER_METHODS@
};

static bool reinit_ok = true;
static unsigned reinit_calls = 0;
static bool board_reinit() { ++reinit_calls; return reinit_ok; }

static void arm(CustomLR1121& radio) {
  uint32_t effective = 0;
  assert(radio.startReceiveDutyCycle(1000000, 500000, 0x35, 0x46, &effective) == 0);
  assert(effective == 1000000 && radio.programmed_rx == 32768);
  assert(radio.programmed_sleep == uint32_t((500000ULL - 6000) * 32768 / 1000000));
  assert(radio.trace == "SLGWD" && radio.needsLfClockRestore());
  assert(radio.staged.receive.timeout == RADIOLIB_LR11X0_RX_TIMEOUT_INF);
  assert(radio.staged.receive.irqFlags == 0x35 && radio.staged.receive.irqMask == 0x46);
}

int main() {
  static_assert(CustomLR1121Wrapper::SupportsRxPowerSaving, "LR1121 advertises RXPS");
  {
    CustomLR1121 radio;
    arm(radio);
    radio.trace.clear();
    assert(radio.restoreLfClock() == 0 && !radio.needsLfClockRestore());
    assert(radio.trace == "R");
    assert(radio.restoreLfClock() == 0 && radio.trace == "R");
  }
  // The maximum CLI periods must survive the native 32768 Hz conversion.
  {
    CustomLR1121 radio;
    uint32_t effective = 0;
    assert(radio.startReceiveDutyCycle(30000000, 30000000, 1, 2, &effective) == 0);
    assert(effective == 30000000 && radio.programmed_rx == 983040);
    assert(radio.programmed_sleep == uint32_t((30000000ULL - 6000) * 32768 / 1000000));
  }
  // Invalid periods fail before changing hardware or underflowing the sleep.
  for (uint32_t sleep : {0U, 5999U, 6000U, 6001U}) {
    CustomLR1121 radio;
    assert(radio.startReceiveDutyCycle(78000, sleep, 1, 2, nullptr)
        == RADIOLIB_ERR_INVALID_SLEEP_PERIOD);
    assert(radio.trace.empty() && !radio.needsLfClockRestore());
  }
  {
    CustomLR1121 radio;
    assert(radio.startReceiveDutyCycle(1000000, 6031, 1, 2, nullptr) == 0);
    assert(radio.programmed_sleep == 1);
  }
  {
    CustomLR1121 radio;
    assert(radio.startReceiveDutyCycle(0, 1000000, 1, 2, nullptr)
        == RADIOLIB_ERR_INVALID_RX_PERIOD);
    assert(radio.trace.empty());
    for (uint32_t rx : {0U, 30U}) {
      assert(radio.startReceiveDutyCycle(rx, 60000, 1, 2, nullptr)
          == RADIOLIB_ERR_INVALID_RX_PERIOD);
      assert(radio.trace.empty());
    }
    assert(radio.startReceiveDutyCycle(512000000, 1000000, 1, 2, nullptr)
        == RADIOLIB_ERR_INVALID_RX_PERIOD);
    assert(radio.trace.empty());
    assert(radio.startReceiveDutyCycle(1000000, 512006000, 1, 2, nullptr)
        == RADIOLIB_ERR_INVALID_SLEEP_PERIOD);
    assert(radio.trace.empty());
    uint32_t effective = 0;
    assert(radio.startReceiveDutyCycle(31, 30000, 1, 2, &effective) == 0);
    assert(effective == 76564);  // SF8/BW62.5: 43 symbols plus 1 ms, less sleep.
  }
  // Stop at the first failed command and remember LF restoration after any
  // attempt to enable BUSY release, including a failing configuration command.
  for (unsigned fault = 0; fault < 4; ++fault) {
    CustomLR1121 radio;
    if (fault == 0) radio.standby_error = -71;
    if (fault == 1) radio.lf_error = -72;
    if (fault == 2) radio.stage_error = -73;
    if (fault == 3) radio.duty_error = -74;
    assert(radio.startReceiveDutyCycle(1000000, 500000, 1, 2, nullptr) == -71 - int(fault));
    const char* expected[] = {"S", "SL", "SLG", "SLGWD"};
    assert(radio.trace == expected[fault]);
    assert(radio.needsLfClockRestore() == (fault != 0));
  }
  {
    CustomLR1121 radio;
    radio._lfClockRestoreNeeded = true; radio.lf_error = -75;
    assert(radio.restoreLfClock() == -75 && radio.needsLfClockRestore());
    radio.lf_error = 0;
    assert(radio.restoreLfClock() == 0 && !radio.needsLfClockRestore());
  }
  for (unsigned fault = 0; fault < 3; ++fault) {
    CustomLR1121 radio;
    radio._activityAt = 90; radio._headerSeen = true;
    if (fault == 0) radio.standby_error = -81;
    if (fault == 1) radio.buffer_error = -82;
    if (fault == 2) radio.irq_error = -83;
    assert(radio.recoverReceivePath() == -81 - int(fault));
    assert(radio.trace == "SBC" && !radio._activityAt && !radio._headerSeen);
  }
  // Driver activity polling does no SPI in a duty-cycle sleep window, and
  // leaves HEADER_ERR visible to the receive owner when awake.
  {
    CustomLR1121 radio;
    radio.module.hal->busy = true;
    radio.irq = RADIOLIB_LR11X0_IRQ_HEADER_ERR;
    assert(!radio.isReceiving() && radio.irq_reads == 0 && radio.irq_clears == 0);
    radio.module.hal->busy = false;
    assert(!radio.isReceiving() && radio.irq == RADIOLIB_LR11X0_IRQ_HEADER_ERR);
    assert(radio.irq_clears == 0);
    assert(radio.getPacketLength(true) == 0 && radio.trace == "SBC");
    assert(radio.base_length_calls == 0 && radio.stale_length == 0 && radio.irq == 0);
    radio.module.gpio = RADIOLIB_NC;
    const unsigned reads = radio.module.hal->reads;
    assert(!radio.isChipBusy() && radio.module.hal->reads == reads);
  }
  {
    CustomLR1121 radio;
    radio.module.hal->busy = true;
    assert(radio.getTimeOnAir(32) == 28800 && radio.toa_calls == 1 && radio.modem_queries == 0);
    radio._activityAt = 99; radio._headerSeen = true;
    assert(radio.startReceive() == 0 && !radio._activityAt && !radio._headerSeen);
    assert(radio.continuous_flags & RADIOLIB_LR11X0_IRQ_PREAMBLE_DETECTED);
    assert(radio.continuous_mask & RADIOLIB_LR11X0_IRQ_HEADER_ERR);
  }
  {
    CustomLR1121 radio;
    assert(radio.begin() == 0 && radio.getTcxoDelay() == 5000);
    assert(radio.trace == "IV" && radio.tcxo_calls == 0 && delays == 0);
    radio.trace.clear();
    assert(radio.begin(434,125,9,7,0x12,10,8,0.0f) == 0);
    assert(radio.getTcxoDelay() == 0 && radio.trace == "IV" && radio.tcxo_calls == 0);
    CustomLR1121Wrapper xo_wrapper(radio);
    assert(xo_wrapper.rxPowerSavingTransitionUs() == 1000);
    radio.trace.clear();
    assert(radio.begin() == 0 && radio.getTcxoDelay() == 5000 && radio.trace == "IV");
  }
  {
    CustomLR1121 radio;
    CustomLR1121Wrapper wrapper(radio);
    assert(wrapper.supportsRxPowerSaving() && wrapper.rxPowerSavingTransitionUs() == 6000);
    assert(wrapper.startReceiveMode() == 77);
    uint32_t effective_rx = 0, effective_sleep = 0;
    assert(wrapper.armDutyCycle(0x31,0x42,&effective_rx,&effective_sleep) == 0);
    assert(effective_rx >= wrapper._rx_ps_rx_us && effective_sleep == wrapper._rx_ps_sleep_us);
    assert(radio.staged.receive.irqFlags == 0x31 && radio.staged.receive.irqMask == 0x42);
    radio.trace.clear();
    assert(wrapper.stopDutyCycleHardware() == 0 && radio.trace == "SR");
    assert(!radio.needsLfClockRestore());
    radio._lfClockRestoreNeeded = true; radio.trace.clear(); radio.lf_error = -78;
    assert(wrapper.startContinuousReceive() == -78 && radio.trace == "SR");
    assert(radio.needsLfClockRestore());
    radio.trace.clear(); radio.lf_error = 0;
    assert(wrapper.startContinuousReceive() == 0 && radio.trace == "SRX");
    assert(!radio.needsLfClockRestore());
    radio.trace.clear();
    assert(wrapper.startContinuousReceive() == 0 && radio.trace == "X");
    radio._lfClockRestoreNeeded = true; radio.standby_error = -79; radio.trace.clear();
    assert(wrapper.startContinuousReceive() == -79 && radio.trace == "S");
    assert(radio.needsLfClockRestore());
    radio.lf_error = -80; radio.trace.clear();
    assert(wrapper.stopDutyCycleHardware() == -79 && radio.trace == "SR");
    assert(radio.needsLfClockRestore());
    radio.standby_error = 0; radio.lf_error = 0; radio.trace.clear();
    assert(wrapper.stopDutyCycleHardware() == 0 && radio.trace == "SR");
    assert(!radio.needsLfClockRestore());
  }
  {
    CustomLR1121 radio;
    CustomLR1121Wrapper wrapper(radio);
    radio.module.hal->busy = true;
    radio.irq = RADIOLIB_LR11X0_IRQ_HEADER_ERR;
    assert(!wrapper.isReceivingPacket() && radio.irq_reads == 0);
    radio.module.hal->busy = false;
    assert(!wrapper.isPacketReady() && radio.trace == "SBC");
    assert(wrapper.n_recv_errors == 1 && !wrapper._rx_ps_armed && !wrapper._rx_hold_continuous);
    assert(!wrapper.isPacketReady());  // A non-RxDone IRQ cannot replay stale FIFO data.
    radio.irq = 1UL << RADIOLIB_IRQ_RX_DONE;
    assert(wrapper.isPacketReady());
  }
  // The driver's completion callback must retain continuous RX while another
  // packet owns it, then hand control back to the generic duty-cycle rearm.
  {
    CustomLR1121 radio;
    CustomLR1121Wrapper wrapper(radio);
    wrapper._rx_ps_armed = false; wrapper.interrupt_pending = true;
    wrapper.onReceiveProcessed();
    assert(wrapper._rx_hold_continuous && wrapper.rearm_calls == 0);
    wrapper.interrupt_pending = false;
    wrapper.onReceiveProcessed();
    assert(!wrapper._rx_hold_continuous && wrapper.rearm_calls == 1 && wrapper.state == STATE_IDLE);
  }
  // Dual-profile scanning keeps RX continuous while changing only the saved
  // requested RXPS setting. Existing generic scan teardown restores that intent.
  {
    CustomLR1121 radio;
    CustomLR1121Wrapper wrapper(radio);
    wrapper._profile_rxps_suspended = true; wrapper._rx_ps_enabled = false;
    assert(wrapper.trySetRxPowerSaving(true,100000,50000) == mesh::RadioParamApplyResult::APPLIED);
    assert(wrapper._profile_saved_rxps && !wrapper._rx_ps_enabled && wrapper.reconfigure_calls == 0);
    assert(wrapper._rx_ps_rx_us == 100000 && wrapper._rx_ps_sleep_us == 50000);
    assert(wrapper.trySetRxPowerSaving(false,100000,50000) == mesh::RadioParamApplyResult::APPLIED);
    assert(!wrapper._profile_saved_rxps && radio.trace.empty());
    wrapper._beacon_owned = true;
    assert(wrapper.trySetRxPowerSaving(true,100000,50000) == mesh::RadioParamApplyResult::BUSY);
    assert(!wrapper._profile_saved_rxps);
  }
  {
    CustomLR1121 radio;
    CustomLR1121Wrapper wrapper(radio);
    assert(!wrapper.supportsRadioDeepInit() && !wrapper.radioDeepInit());
    assert(wrapper.prepare_calls == 0 && wrapper.board.finish_calls == 0 && reinit_calls == 0);
    wrapper.setDeepInitCallback(board_reinit);
    assert(wrapper.supportsRadioDeepInit());
    wrapper.hard_reset_path = false;
    assert(!wrapper.supportsRadioDeepInit());
    wrapper.hard_reset_path = true;
    wrapper.prepare_ok = false;
    assert(!wrapper.radioDeepInit() && wrapper.prepare_calls == 1);
    assert(wrapper.board.finish_calls == 0 && reinit_calls == 0);
    wrapper.prepare_ok = true; reinit_ok = false;
    assert(!wrapper.radioDeepInit() && wrapper.prepare_calls == 2 && reinit_calls == 1);
    assert(wrapper.board.finish_calls == 0);
    reinit_ok = true; wrapper.board.finish_ok = false;
    assert(!wrapper.radioDeepInit() && wrapper.prepare_calls == 3 && reinit_calls == 2);
    assert(wrapper.board.finish_calls == 1);
    wrapper.board.finish_ok = true;
    assert(wrapper.radioDeepInit() && wrapper.prepare_calls == 4 && reinit_calls == 3);
    assert(wrapper.board.finish_calls == 2);
  }
}
'''


class LR1121RxpsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
        if not cls.compiler:
            raise unittest.SkipTest("C++ compiler unavailable")

    def compile_and_run(self, source, flags=()):
        with tempfile.TemporaryDirectory(prefix="lr1121-rxps-") as directory:
            folder = Path(directory)
            cpp, binary = folder / "probe.cpp", folder / "probe"
            cpp.write_text(source, encoding="ascii")
            command = [self.compiler, "-std=gnu++17", "-O1", "-g", "-Wall", "-Wextra",
                       "-Werror", "-I", str(ROOT / "src"), *flags]
            if os.name != "nt":
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-pie", "-no-pie"]
            built = subprocess.run([*command, str(cpp), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_driver_and_wrapper_hardware_lifecycle(self):
        driver = (ROOT / "src/helpers/radiolib/CustomLR1121.h").read_text()
        wrapper = (ROOT / "src/helpers/radiolib/CustomLR1121Wrapper.h").read_text()
        generic = (ROOT / "src/helpers/radiolib/RadioLibWrappers.cpp").read_text()
        driver_methods = "\n".join(body(driver, signature) for signature in (
            "uint32_t getTcxoDelay()", "bool needsLfClockRestore()",
            "int16_t restoreLfClock()", "int16_t startReceiveDutyCycle(",
            "RadioLibTime_t getTimeOnAir(",
            "bool isChipBusy()", "int16_t recoverReceivePath()",
            "size_t getPacketLength(", "int16_t startReceive()", "bool isReceiving()",
            "int16_t begin(float",
        ))
        wrapper_methods = "\n".join(body(wrapper, signature) for signature in (
            "uint32_t rxPowerSavingTransitionUs()", "bool supportsRxPowerSaving()",
            "bool isChipBusy()", "bool isReceivingPacket()", "int startReceiveMode()",
            "bool isPacketReady()", "int16_t armDutyCycle(", "int16_t stopDutyCycleHardware()",
            "int16_t startContinuousReceive()", "void onReceiveProcessed()",
            "bool radioDeepInit()", "bool supportsRadioDeepInit()",
            "void setDeepInitCallback(",
        ))
        capability = re.search(r"static constexpr bool SupportsRxPowerSaving\s*=\s*true;", wrapper)
        self.assertIsNotNone(capability)
        base_methods = "\n".join(body(generic, signature).replace("RadioLibWrapper::", "")
                                 for signature in (
                                     "bool RadioLibWrapper::isPacketReady()",
                                     "void RadioLibWrapper::finishReceiveProcessing()",
                                     "mesh::RadioParamApplyResult RadioLibWrapper::trySetRxPowerSaving(",
                                 ))
        base_methods = base_methods.replace("bool isPacketReady()", "virtual bool isPacketReady()", 1)
        source = (HARNESS.replace("@DRIVER_METHODS@", driver_methods)
                  .replace("@WRAPPER_METHODS@", wrapper_methods)
                  .replace("@BASE_METHODS@", base_methods)
                  .replace("@CAPABILITY@", capability.group(0)))
        self.compile_and_run(source, ["-DMESH_ENABLE_TX_BEACONS=1", "-DUSE_LR1121"])

    def test_board_recipes_select_lr11x0_math_before_target_headers(self):
        recipes = [
            ("variants/waveshare_esp32s3_lr1121/platformio.ini", "Waveshare_S3_LR1121"),
            ("variants/muzi_base/platformio.ini", "muzi_base_duo"),
        ]
        source = r'''
#include <cassert>
// Intentionally included before any target/driver header: recipe flags must
// select family-specific math in CommonCLI and role constructors too.
#include <helpers/radiolib/RXPowerSaving.h>
int main() {
  assert(rxPowerSavingDefaultCaptureCost() == 8.0f);
  assert(rxPowerSavingDefaultTransitionUs() == EXPECT_TRANSITION);
  uint32_t rx = 0, sleep = 0, explicit_rx = 0, explicit_sleep = 0;
  assert(calcRxPowerSavingLevel(6,8,62.5f,32,&rx,&sleep));
  assert(calcRxPowerSavingLevel(6,8,62.5f,32,&explicit_rx,&explicit_sleep,
      8.0f,EXPECT_TRANSITION));
  assert(rx == explicit_rx && sleep == explicit_sleep);
#if defined(USE_LR1121) && EXPECT_TRANSITION == 6000
  assert(!calcRxPowerSavingLevel(6,8,812.5f,16,&rx,&sleep));
  uint8_t effective_level = 0, effective_preamble = 0;
  assert(recalcRxPowerSavingFromLevel(6,8,812.5f,16,&rx,&sleep,
      &effective_level,&effective_preamble));
  // Preserve the existing policy of promoting to the actual longer wire
  // preamble when a short configured preamble cannot cover TCXO startup.
  assert(!rxPowerSavingUsesContinuousFallback(rx,sleep));
  assert(effective_level >= 6 && effective_preamble > 16);
  assert(recalcRxPowerSavingFromLevel(6,5,812.5f,16,&rx,&sleep));
  assert(rxPowerSavingUsesContinuousFallback(rx,sleep));
  assert(recalcRxPowerSavingFromLevel(6,10,812.5f,16,&rx,&sleep));
  assert(!rxPowerSavingUsesContinuousFallback(rx,sleep));
#endif
}
'''
        for relative, section in recipes:
            text = (ROOT / relative).read_text()
            section_text = text.split("[" + section + "]", 1)[1].split("\n[", 1)[0]
            self.assertRegex(section_text, r"(?m)^\s*-D\s+USE_LR1121\b", relative)
            voltage = re.search(r"-D\s+LR11X0_DIO3_TCXO_VOLTAGE=([0-9.]+)", section_text)
            self.assertIsNotNone(voltage)
            with self.subTest(recipe=relative):
                self.compile_and_run(source, ["-DUSE_LR1121",
                    "-DEXPECT_TRANSITION=6000",
                    "-DLR11X0_DIO3_TCXO_VOLTAGE=" + voltage.group(1)])
        with self.subTest(family="LR1110"):
            self.compile_and_run(source, ["-DUSE_LR1110", "-DEXPECT_TRANSITION=2600",
                                          "-DLR11X0_DIO3_TCXO_VOLTAGE=1.8"])
        with self.subTest(oscillator="LR1121 XO"):
            self.compile_and_run(source, ["-DUSE_LR1121", "-DEXPECT_TRANSITION=1000",
                                          "-DLR11X0_DIO3_TCXO_VOLTAGE=0.0"])

    def test_boards_register_deep_init_after_restoring_rf_switch_and_gain(self):
        for relative in ("variants/waveshare_esp32s3_lr1121/target.cpp",
                         "variants/muzi_base/target.cpp"):
            with self.subTest(board=relative):
                initialization = body((ROOT / relative).read_text(), "bool radio_init()")
                callback = initialization.index("radio_driver.setDeepInitCallback(radio_init)")
                switch = initialization.index("radio.setRfSwitchTable(")
                gain = initialization.index("radio.setRxBoostedGainMode(")
                self.assertLess(initialization.index("radio.begin("), switch)
                self.assertLess(switch, gain)
                self.assertLess(gain, callback)
                self.assertLess(callback, initialization.index("return true", callback))


if __name__ == "__main__":
    unittest.main()
