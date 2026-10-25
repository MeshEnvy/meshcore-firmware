"""Execute production RXPS arming, fallback and status against faultable hardware."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from cpp_source import body

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <helpers/radiolib/RXPowerSaving.h>
namespace mesh { enum class RadioParamApplyResult : uint8_t { APPLIED, BUSY, FAILED }; }

#define MESH_ENABLE_TX_BEACONS 1
#define MESH_DEBUG_PRINTLN(...) ((void)0)
constexpr int RADIOLIB_ERR_NONE = 0;
using RadioLibIrqFlags_t = uint32_t;
constexpr uint32_t RADIOLIB_IRQ_RX_DEFAULT_FLAGS = 0x100;
constexpr unsigned RADIOLIB_IRQ_PREAMBLE_DETECTED = 1, RADIOLIB_IRQ_RX_DONE = 2;
constexpr unsigned RADIOLIB_IRQ_TIMEOUT = 3, RADIOLIB_IRQ_CRC_ERR = 4;
constexpr unsigned RADIOLIB_IRQ_HEADER_ERR = 5;

struct RadioLibWrapper {
  bool _rx_ps_enabled = true, _rx_ps_armed = false;
  bool _rx_ps_continuous_fallback = false, _nf_calib_active = false;
  bool _profile_rxps_suspended = false;
  bool _profile_repair_required = false;
  bool _profile_saved_rxps = false, _beacon_owned = false, _cw_active = false;
  uint32_t _rx_ps_rx_us = 50000, _rx_ps_sleep_us = 50000;
  uint32_t _rx_ps_eff_rx_us = 0, _rx_ps_eff_sleep_us = 0;
  int16_t _rx_ps_last_error = RADIOLIB_ERR_NONE;
  uint32_t _rx_ps_arm_failures = 0, n_recv = 0, n_sent = 0, n_recv_errors = 0;
  RxPowerSavingArmRetryState _rx_ps_arm_retry;

  bool packet_pending = false, supported = true;
  int16_t arm_error = 0, continuous_error = 0, stop_error = 0;
  uint32_t hardware_rx_us = 48000, hardware_sleep_us = 47000;
  unsigned arm_calls = 0, continuous_calls = 0, stop_calls = 0;
  unsigned begin_calls = 0, end_calls = 0;
  uint8_t reconfigure_result = 1;
  std::string events;

  bool supportsRxPowerSaving() const { return supported; }
  bool isPacketPendingOrReceiving() const { return packet_pending; }
  uint32_t rxPowerSavingTransitionUs() const { return 2600; }
  int16_t startContinuousReceive() {
    ++continuous_calls; events += 'C'; return continuous_error;
  }
  int16_t stopDutyCycleHardware() {
    ++stop_calls; events += 'S'; return stop_error;
  }
  int16_t armDutyCycle(RadioLibIrqFlags_t flags, RadioLibIrqFlags_t mask,
                      uint32_t* rx_us, uint32_t* sleep_us) {
    ++arm_calls; events += 'A';
    assert(flags == (RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED)));
    assert(mask == ((1UL << RADIOLIB_IRQ_RX_DONE) | (1UL << RADIOLIB_IRQ_TIMEOUT)
        | (1UL << RADIOLIB_IRQ_CRC_ERR) | (1UL << RADIOLIB_IRQ_HEADER_ERR)));
    assert(*rx_us == _rx_ps_rx_us && *sleep_us == _rx_ps_sleep_us);
    if (!arm_error) { *rx_us = hardware_rx_us; *sleep_us = hardware_sleep_us; }
    return arm_error;
  }
  uint8_t beginReconfigure() { ++begin_calls; return reconfigure_result; }
  void endReconfigure(bool resume_rx) {
    ++end_calls;
    if (resume_rx) assert(startReceivePowerSaving() == RADIOLIB_ERR_NONE);
  }

  int startReceivePowerSaving();
  void stopReceiveDutyCycle();
  bool setRxPowerSaving(bool, uint32_t, uint32_t);
  mesh::RadioParamApplyResult trySetRxPowerSaving(bool, uint32_t, uint32_t);
  RxPowerSavingStatus getRxPowerSavingStatus() const;
  @RESET_STATS@
};

@METHODS@

static void exhaust(RadioLibWrapper& radio) {
  radio.arm_error = -708;
  for (unsigned attempt = 1; attempt <= 3; ++attempt) {
    assert(radio.startReceivePowerSaving() == RADIOLIB_ERR_NONE);
    assert(radio.arm_calls == attempt && radio.stop_calls == attempt);
    assert(radio.continuous_calls == attempt);
    const auto status = radio.getRxPowerSavingStatus();
    assert(status.supported && !status.armed && status.last_error == -708);
    assert(status.arm_failures == attempt);
    assert(!status.effective_rx_us && !status.effective_sleep_us);
    assert(radio._rx_ps_continuous_fallback);
  }
  assert(radio.events == "ASCASCASC");
  for (unsigned poll = 0; poll < 5; ++poll)
    assert(radio.startReceivePowerSaving() == RADIOLIB_ERR_NONE);
  assert(radio.arm_calls == 3 && radio.stop_calls == 3 && radio.continuous_calls == 8);
  assert(radio.getRxPowerSavingStatus().last_error == -708);
}

int main() {
  {
    using Result = mesh::RadioParamApplyResult;
    RadioLibWrapper radio;
    assert(radio.trySetRxPowerSaving(true, 50000, 50000) == Result::APPLIED);
    assert(!radio.begin_calls && !radio.end_calls); // Same accepted timings do not re-arm RX.
    radio.reconfigure_result = 2;
    assert(radio.trySetRxPowerSaving(true, 70000, 90000) == Result::BUSY);
    assert(radio._rx_ps_rx_us == 50000 && !radio.end_calls);
    radio.reconfigure_result = 3;
    assert(radio.trySetRxPowerSaving(true, 70000, 90000) == Result::FAILED);
    assert(radio._rx_ps_rx_us == 50000 && !radio.end_calls);
    radio.supported = false;
    const unsigned before = radio.begin_calls;
    assert(radio.trySetRxPowerSaving(true, 70000, 90000) == Result::FAILED);
    assert(radio.trySetRxPowerSaving(true, 0, 90000) == Result::FAILED);
    assert(radio.begin_calls == before);
    radio.supported = true; radio._cw_active = true;
    assert(radio.trySetRxPowerSaving(true, 50000, 50000) == Result::BUSY);
    assert(radio.begin_calls == before);
  }
  {
    RadioLibWrapper radio;
    exhaust(radio);
  }
  {
    // Successful continuous RX for calibration must retain the arm failure.
    RadioLibWrapper radio;
    radio.arm_error = -708;
    assert(!radio.startReceivePowerSaving());
    radio._nf_calib_active = true;
    assert(!radio.startReceivePowerSaving());
    const auto status = radio.getRxPowerSavingStatus();
    assert(status.last_error == -708 && status.arm_failures == 1);
    assert(radio.arm_calls == 1 && radio.continuous_calls == 2);
    radio._nf_calib_active = false;
    assert(!radio.startReceivePowerSaving() && radio.arm_calls == 2);
  }
  {
    // A real arm success replenishes the consecutive-error budget.
    RadioLibWrapper radio;
    radio.arm_error = -708;
    assert(!radio.startReceivePowerSaving());
    assert(!radio.startReceivePowerSaving());
    radio.arm_error = 0;
    assert(!radio.startReceivePowerSaving());
    auto status = radio.getRxPowerSavingStatus();
    assert(status.armed && status.last_error == 0 && status.arm_failures == 2);
    assert(status.effective_rx_us == 48000 && status.effective_sleep_us == 47000);
    assert(status.effective_rx_us != radio._rx_ps_rx_us);
    assert(status.effective_sleep_us != radio._rx_ps_sleep_us);
    radio.arm_error = -707;
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
      assert(!radio.startReceivePowerSaving());
      assert(radio.arm_calls == 3 + attempt);
    }
    assert(!radio.startReceivePowerSaving() && radio.arm_calls == 6);
    assert(radio.getRxPowerSavingStatus().arm_failures == 5);
  }
  {
    RadioLibWrapper radio;
    assert(!radio.startReceivePowerSaving());
    radio._nf_calib_active = radio.packet_pending = true;
    assert(!radio.startReceivePowerSaving());
    assert(radio.arm_calls == 1 && radio.stop_calls == 0 && radio.continuous_calls == 0);
    assert(radio.getRxPowerSavingStatus().armed);
    radio.packet_pending = false;
    assert(!radio.startReceivePowerSaving());
    auto status = radio.getRxPowerSavingStatus();
    assert(!status.armed && !status.effective_rx_us && !status.effective_sleep_us);
    assert(radio.events == "ASC");
  }
  {
    RadioLibWrapper radio;
    assert(!radio.startReceivePowerSaving());
    radio.stop_error = -705;
    radio.stopReceiveDutyCycle();
    auto status = radio.getRxPowerSavingStatus();
    assert(!status.armed && status.last_error == -705);
    assert(!status.effective_rx_us && !status.effective_sleep_us);
  }
  {
    RadioLibWrapper radio;
    radio._rx_ps_sleep_us = 2000; // Below the real 2600 us wake transition.
    assert(!radio.startReceivePowerSaving());
    assert(!radio.arm_calls && radio.continuous_calls == 1);
    assert(radio.getRxPowerSavingStatus().last_error == RX_POWERSAVING_ERR_TIMING);
  }
  {
    // Hardware fallback failure must be visible to the ordinary RX retry loop.
    RadioLibWrapper radio;
    radio.arm_error = -708; radio.continuous_error = -705;
    assert(radio.startReceivePowerSaving() == -705);
    assert(radio.getRxPowerSavingStatus().last_error == -705);
    assert(radio.getRxPowerSavingStatus().arm_failures == 1);
  }
  {
    RadioLibWrapper radio;
    radio._rx_ps_rx_us = radio._rx_ps_sleep_us = RX_POWERSAVING_CONTINUOUS_FALLBACK_US;
    assert(!radio.startReceivePowerSaving());
    assert(!radio.arm_calls && radio.continuous_calls == 1);
    assert(radio._rx_ps_continuous_fallback && !radio.getRxPowerSavingStatus().armed);
    assert(!radio.getRxPowerSavingStatus().arm_failures);
  }
  {
    RadioLibWrapper radio;
    exhaust(radio);
    radio.arm_error = 0;
    assert(radio.setRxPowerSaving(true, 70000, 90000));
    assert(radio.begin_calls == 1 && radio.end_calls == 1 && radio.arm_calls == 4);
    auto status = radio.getRxPowerSavingStatus();
    assert(status.armed && status.last_error == 0 && status.arm_failures == 3);
    assert(radio._rx_ps_rx_us == 70000 && radio._rx_ps_sleep_us == 90000);
  }
  {
    RadioLibWrapper radio;
    exhaust(radio);
    radio.n_recv = 2; radio.n_sent = 3; radio.n_recv_errors = 4;
    radio.resetStats();
    assert(!radio.n_recv && !radio.n_sent && !radio.n_recv_errors);
    assert(!radio.getRxPowerSavingStatus().arm_failures);
    radio.arm_error = 0;
    assert(!radio.startReceivePowerSaving() && radio.arm_calls == 4);
    assert(radio.getRxPowerSavingStatus().armed);
  }
  std::puts("PASS: production RXPS fallback, retry, status and recovery");
}
'''


def production_source():
    wrapper = (ROOT / "src/helpers/radiolib/RadioLibWrappers.cpp").read_text()
    header = (ROOT / "src/helpers/radiolib/RadioLibWrappers.h").read_text()
    methods = "\n\n".join(body(wrapper, signature) for signature in (
        "int RadioLibWrapper::startReceivePowerSaving()",
        "void RadioLibWrapper::stopReceiveDutyCycle()",
        "bool RadioLibWrapper::setRxPowerSaving(",
        "mesh::RadioParamApplyResult RadioLibWrapper::trySetRxPowerSaving(",
        "RxPowerSavingStatus RadioLibWrapper::getRxPowerSavingStatus()"))
    return HARNESS.replace("@METHODS@", methods).replace(
        "@RESET_STATS@", body(header, "void resetStats()"))


class RxPowerSavingRuntimeTests(unittest.TestCase):
    def run_harness(self, source, expect_pass=True):
        with tempfile.TemporaryDirectory(prefix="meshcore-rxps-runtime-") as directory:
            cpp, binary = Path(directory) / "runtime.cpp", Path(directory) / "runtime"
            cpp.write_text(source, encoding="ascii")
            build = subprocess.run([
                os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-pie", "-no-pie",
                "-I", str(ROOT / "src"), str(cpp), "-o", str(binary)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            if expect_pass:
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("PASS:", run.stdout)
            else:
                self.assertNotEqual(run.returncode, 0, "negative control unexpectedly passed")
                self.assertIn("Assertion", run.stderr)

    def test_actual_wrapper_rxps_retry_status_and_recovery(self):
        self.run_harness(production_source())

    def test_retry_budget_and_calibration_error_negative_controls(self):
        source = production_source()
        mutations = (
            ("|| !_rx_ps_arm_retry.canAttempt()", "|| false"),
            ("_rx_ps_arm_retry.recordSuccess();", "/* omit successful-arm budget reset */"),
            ("if (err != RADIOLIB_ERR_NONE || !_rx_ps_enabled) _rx_ps_last_error = err;",
             "_rx_ps_last_error = err;"),
        )
        for original, replacement in mutations:
            with self.subTest(mutation=original):
                self.assertEqual(source.count(original), 1)
                self.run_harness(source.replace(original, replacement, 1), expect_pass=False)


if __name__ == "__main__":
    unittest.main()
