"""Exercise production LR11xx/LR2021 Meshtext packet and restoration methods."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


COMMON = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <RadioProfiles.h>
#include <helpers/radiolib/LR1110ProfileSwitchState.h>
#include <helpers/radiolib/LR2021ApplyStatus.h>
#define MESH_ENABLE_TX_BEACONS 1
#define MESH_ENABLE_MESHTEXT_WIRE 1
#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE 0x12
struct ApplyRecorder {
  mesh::lr2021::ApplyStatus _apply_status;
  int16_t recordApply(mesh::lr2021::ApplyStatus::Stage stage, int16_t code) {
    return _apply_status.record(stage, code);
  }
};
struct Wire {
  unsigned calls = 0, fail = 0;
  uint8_t sync = 0x12;
  uint16_t preamble = 16;
  bool crc = false, explicit_header = false, inverted = true;
  int16_t next() { return ++calls == fail ? -705 : 0; }
  int16_t setSyncWord(uint8_t value) { sync = value; return next(); }
  int16_t setCRC(uint8_t value) { assert(value == 2); crc = value > 0; return next(); }
  int16_t explicitHeader() { explicit_header = true; return next(); }
  int16_t invertIQ(bool value) { inverted = value; return next(); }
};
'''


class LRMeshtextWireTest(unittest.TestCase):
    def run_cpp(self, code):
        with tempfile.TemporaryDirectory(prefix="meshcore-lr-meshtext-") as folder:
            source, binary = Path(folder) / "test.cpp", Path(folder) / "test"
            source.write_text(code, encoding="ascii")
            result = subprocess.run(
                ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src"),
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-pie", "-no-pie",
                 str(source), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
            result = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout

    def test_actual_three_lr_families_protocol_errors_and_restore(self):
        for family in ("LR1110", "LR1121", "LR2021"):
            with self.subTest(family=family):
                driver = (ROOT / f"src/helpers/radiolib/Custom{family}.h").read_text()
                wrapper = (ROOT / f"src/helpers/radiolib/Custom{family}Wrapper.h").read_text()
                protocol = method(driver, "int16_t setProfileProtocol(")
                apply = method(wrapper, "bool applyProtocol(").replace(" override", "")
                support = method(wrapper, "bool supportsMeshtext()").replace(" override", "")
                code = COMMON + r'''
struct @FAMILY@ : Wire {};
struct Custom@FAMILY@ : @FAMILY@ {
  LR1110ProfileSwitchState _profileSwitch{true};
  bool _profilePacketDirty = true;
  @PROTOCOL@
};
struct Wrapper : ApplyRecorder {
  Custom@FAMILY@ chip;
  Custom@FAMILY@* _radio = &chip;
  @SUPPORT@
  @APPLY@
};
int main() {
  using Protocol = mesh::RadioProfileProtocol;
  Wrapper invalid;
  assert(!invalid.applyProtocol(static_cast<Protocol>(255)) && invalid.chip.calls == 0);
  unsigned scenarios = 1;
  for (Protocol protocol : {Protocol::MeshCore, Protocol::Meshtext}) {
    Wrapper wrapper;
    assert(wrapper.supportsMeshtext());
    wrapper.chip._profileSwitch.rxValid = wrapper.chip._profileSwitch.active = true;
    wrapper.chip._profileSwitch.modulationValid = wrapper.chip._profileSwitch.standbyReady = true;
    wrapper.chip._profileSwitch.open = true;
    assert(wrapper.applyProtocol(protocol));
    assert(wrapper.chip.sync == (protocol == Protocol::Meshtext ? 0x2b : 0x12));
    assert(wrapper.chip.crc && wrapper.chip.explicit_header && !wrapper.chip.inverted);
    assert(wrapper.chip.preamble == 16 && wrapper.chip.calls == 4);
    @FAST_SUCCESS@
    wrapper.chip.preamble = 32;
    assert(wrapper.applyProtocol(Protocol::MeshCore));
    assert(wrapper.chip.sync == 0x12 && wrapper.chip.crc && wrapper.chip.explicit_header);
    assert(!wrapper.chip.inverted && wrapper.chip.preamble == 32);
    ++scenarios;
  }
  for (unsigned failure = 1; failure <= 4; ++failure) {
    Wrapper wrapper;
    wrapper.chip._profileSwitch.rxValid = wrapper.chip._profileSwitch.active = true;
    wrapper.chip._profileSwitch.modulationValid = wrapper.chip._profileSwitch.standbyReady = true;
    wrapper.chip._profileSwitch.open = true;
    wrapper.chip.fail = failure;
    assert(!wrapper.applyProtocol(Protocol::Meshtext));
    assert(wrapper.chip.calls == failure); // no later setters after an error
    @FAST_FAILURE@
    wrapper.chip.fail = 0;
    assert(wrapper.applyProtocol(Protocol::MeshCore));
    assert(wrapper.chip.sync == 0x12 && wrapper.chip.crc && wrapper.chip.explicit_header);
    assert(!wrapper.chip.inverted);
    ++scenarios;
  }
  std::printf("PASS: @FAMILY@ %u actual protocol cases\n", scenarios);
}
'''
                fast = "assert(!wrapper.chip._profileSwitch.rxValid && !wrapper.chip._profileSwitch.modulationValid);\n"
                fast += "assert(!wrapper.chip._profileSwitch.canResumeFast() && !wrapper.chip._profilePacketDirty);"
                code = (code.replace("@FAMILY@", family).replace("@PROTOCOL@", protocol)
                        .replace("@APPLY@", apply).replace("@SUPPORT@", support)
                        .replace("@FAST_SUCCESS@", fast if family == "LR1110" else "")
                        .replace("@FAST_FAILURE@", fast if family == "LR1110" else ""))
                self.assertIn("7 actual protocol cases", self.run_cpp(code))

    def test_lr2021_transient_side_detectors_and_physical_power_band(self):
        source = (ROOT / "src/helpers/radiolib/CustomLR2021Wrapper.h").read_text()
        build = method(source, "static void buildSideDetectorConfig(")
        side = method(source, "bool applySideDetectorConfig(")
        scan = method(source, "int16_t performChannelScan()").replace(" override", "")
        power = method(source, "int16_t applyCachedTxPower(").replace(" override", "")
        code = COMMON + r'''
#define USE_LR2021 1
#define LORA_BW 125
#define RADIOLIB_ERR_UNKNOWN -1
#define RADIOLIB_ERR_INVALID_SIDE_DETECT -1000
#define RADIOLIB_CHANNEL_FREE -2
#define MESH_DEBUG_PRINTLN(...) ((void)0)
#include <helpers/radiolib/LR2021SideDetectorConfig.h>
#include <helpers/radiolib/RadioPowerLimits.h>
struct LR2021LoRaSideDetector_t {
  uint8_t sf = 0;
  bool ldro = false, invertIQ = true;
  uint8_t syncWord = 0;
};
struct CustomLR2021 {
  unsigned calls = 0;
  size_t count = 0;
  bool fail = false;
  float frequency = 906;
  float bandwidth = 125;
  uint8_t sf = 7;
  unsigned sf_changes = 0;
  int8_t power = 0;
  LR2021LoRaSideDetector_t values[3];
  int16_t setSideDetector(const LR2021LoRaSideDetector_t* config, size_t size) {
    ++calls; count = size;
    if (size) for (size_t i = 0; i < size; ++i) values[i] = config[i];
    return fail ? -705 : 0;
  }
  float getFreqMHz() const { return frequency; }
  int16_t setOutputPower(int8_t value) { power = value; return 0; }
  float getBandwidthKhz() const { return bandwidth; }
  int16_t standby() { return 0; }
  int16_t setSpreadingFactor(uint8_t value) { sf = value; ++sf_changes; return 0; }
};
struct RadioLibWrapper {
  unsigned primary_scans = 0;
  int16_t performChannelScan() { ++primary_scans; return RADIOLIB_CHANNEL_FREE; }
};
struct Board {
  unsigned watchdog_services = 0;
  void serviceWatchdog() { ++watchdog_services; }
};
struct Wrapper : RadioLibWrapper, ApplyRecorder {
  CustomLR2021 chip;
  CustomLR2021* _radio = &chip;
  Board board;
  Board* _board = &board;
  mesh::RadioProfiles _profiles;
  bool beacon_owned = false, _beacon_restoring = false;
  bool _params_valid = true;
  float _cur_bw = 125;
  uint8_t _sideDetSFs[3] = {8, 9, 10};
  size_t _numSideDet = 3;
  uint8_t scanned_sfs[4] = {};
  unsigned multi_scans = 0;
  uint8_t getSpreadingFactor() const { return chip.sf; }
  int16_t performChannelScanWithTimeout(uint32_t timeout) {
    assert(timeout > 0 && multi_scans < 4);
    scanned_sfs[multi_scans++] = chip.sf;
    return RADIOLIB_CHANNEL_FREE;
  }
  uint32_t cadScanTimeoutMillis(uint8_t sf, float bw) {
    assert(sf >= 5 && sf <= 12 && bw > 0);
    return 100;
  }
  bool restoreAfterDeepInit() { assert(false); return false; }
  bool isBeaconRadioOwned() const { return beacon_owned; }
  @BUILD@
  @SIDE@
  @SCAN@
  @POWER@
};
int main() {
  Wrapper wrapper;
  // Valid normal RX set: SF7, 125 kHz with SF8/9/10 side detectors.
  assert(wrapper.applySideDetectorConfig(7, 125) && wrapper.chip.count == 3);
  // Same saved set is illegal at LongFast/SF11; disable, not reject or mutate.
  wrapper.beacon_owned = true;
  assert(wrapper.applySideDetectorConfig(11, 250) && wrapper.chip.count == 0);
  assert(wrapper._numSideDet == 3 && wrapper._sideDetSFs[0] == 8);
  // Cleanup repairs normal RX before relinquishing exclusive ownership.
  wrapper._beacon_restoring = true;
  assert(wrapper.applySideDetectorConfig(7, 125) && wrapper.chip.count == 3);
  for (size_t i = 0; i < 3; ++i) {
    assert(wrapper.chip.values[i].sf == 8 + i);
    assert(!wrapper.chip.values[i].invertIQ && wrapper.chip.values[i].syncWord == 0x12);
  }
  wrapper._beacon_restoring = false;
  wrapper.chip.fail = true;
  assert(!wrapper.applySideDetectorConfig(11, 250));
  wrapper.chip.fail = false;
  wrapper._profiles.secondary.mode = mesh::RadioProfileMode::RxTx;
  wrapper._beacon_restoring = true;
  assert(wrapper.applySideDetectorConfig(7, 125) && wrapper.chip.count == 0);
  // An off-channel TX keeps the wrapper's normal RX cache. Clamp using the
  // driver's actual band, matching the PA chosen by its setOutputPower().
  wrapper.chip.frequency = 2440;
  assert(wrapper.applyCachedTxPower(22) == 0 && wrapper.chip.power == 12);
  wrapper.chip.frequency = 906;
  assert(wrapper.applyCachedTxPower(22) == 0 && wrapper.chip.power == 22);

  // Temporary SF11 cannot use the saved SF8/9/10 list. Keep the saved list,
  // disable its hardware detectors, and scan only SF11 while it is suspended.
  Wrapper temporary;
  temporary._profiles.primary_temporary = true;
  temporary.chip.sf = 11;
  assert(temporary.applySideDetectorConfig(11, 250) && temporary.chip.count == 0);
  assert(temporary._numSideDet == 3 && temporary._sideDetSFs[0] == 8);
  assert(temporary.performChannelScan() == RADIOLIB_CHANNEL_FREE);
  assert(temporary.primary_scans == 1 && temporary.multi_scans == 0);
  assert(temporary.chip.sf == 11 && temporary.chip.sf_changes == 0);
  // A failed disable must remain a real apply failure, even during TempRadio.
  temporary.chip.fail = true;
  assert(!temporary.applySideDetectorConfig(11, 250));
  assert(temporary._numSideDet == 3 && temporary._sideDetSFs[2] == 10);
  temporary.chip.fail = false;
  temporary._profiles.primary_temporary = false;
  assert(!temporary.applySideDetectorConfig(11, 250)); // permanent validation remains strict
  // Expiry or cancellation restores the complete saved detector list and CAD.
  temporary.chip.sf = 7;
  assert(temporary.applySideDetectorConfig(7, 125) && temporary.chip.count == 3);
  assert(temporary.performChannelScan() == RADIOLIB_CHANNEL_FREE);
  assert(temporary.multi_scans == 4 && temporary.chip.sf == 7);
  for (unsigned i = 0; i < 4; ++i) assert(temporary.scanned_sfs[i] == 7 + i);

  // The reported SF7/62.5 tuple also conflicts with SF12 from a valid saved
  // SF9 setup: its detector gap exceeds four. Resume all extras on normal SF9.
  Wrapper lower;
  lower._sideDetSFs[0] = 10; lower._sideDetSFs[1] = 11; lower._sideDetSFs[2] = 12;
  lower._profiles.primary_temporary = true;
  lower._cur_bw = 62.5;
  assert(lower.applySideDetectorConfig(7, 62.5) && lower.chip.count == 0);
  assert(lower.performChannelScan() == RADIOLIB_CHANNEL_FREE && lower.primary_scans == 1);
  lower._profiles.primary_temporary = false;
  lower.chip.sf = 9;
  assert(lower.applySideDetectorConfig(9, 62.5) && lower.chip.count == 3);
  assert(lower._numSideDet == 3 && lower._sideDetSFs[2] == 12);

  // A compatible temporary tuple keeps multi-SF reception and CAD available.
  Wrapper compatible;
  compatible._profiles.primary_temporary = true;
  assert(compatible.applySideDetectorConfig(7, 125) && compatible.chip.count == 3);
  assert(compatible.performChannelScan() == RADIOLIB_CHANNEL_FREE);
  assert(compatible.primary_scans == 0 && compatible.multi_scans == 4);
  std::puts("PASS: LR2021 independent side detectors and physical power band");
}
'''
        code = (code.replace("@BUILD@", build).replace("@SIDE@", side)
                .replace("@SCAN@", scan).replace("@POWER@", power))
        self.assertIn("PASS:", self.run_cpp(code))


if __name__ == "__main__":
    unittest.main()
