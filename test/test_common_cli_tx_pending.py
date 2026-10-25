"""Exercise saved TX intent through the production CLI, save and loop methods."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from test_replay_reset_integration import extract_braced

ROOT = Path(__file__).resolve().parents[1]


def preprocess(source, hil=False):
    result = subprocess.run([
        os.environ.get('CXX', 'g++'), '-E', '-P', '-x', 'c++',
        *(['-DMESH_HIL_TX_FREQ_MHZ=909.5f'] if hil else []), '-',
    ], input=source, text=True, capture_output=True, check=True)
    return result.stdout


def production_fixture():
    source = (ROOT / 'src/helpers/CommonCLI.cpp').read_text()
    header = (ROOT / 'src/helpers/CommonCLI.h').read_text()
    dispatcher = (ROOT / 'src/Dispatcher.h').read_text()
    code = (ROOT / 'test/fixtures/common_cli_tx_pending.cpp').read_text()
    methods = '\n'.join(extract_braced(source, signature) for signature in (
        'void CommonCLI::applyAndSaveTxPower(', 'void CommonCLI::servicePendingTxPower(',
        'bool CommonCLI::trySavePrefs(', 'void CommonCLI::loop('))
    setter = extract_braced(source, 'void CommonCLI::handleSetCmd(')
    getter = preprocess(extract_braced(source, 'void CommonCLI::handleGetCmd('), hil=True)
    values = {
        'METHODS': methods,
        'SET': extract_braced(setter, 'if (memcmp(config, "tx ", 3) == 0)'),
        'GET': ' else '.join(extract_braced(getter,
            f'if (configKeyEquals(config, "{key}"))') for key in ('tx.status', 'tx')),
        'KEY_EQUALS': extract_braced(source, 'static bool configKeyEquals('),
        'ENUM': extract_braced(dispatcher, 'enum class RadioParamApplyResult') + ';',
        'DEFAULT_CALLBACK': extract_braced(header,
            'virtual mesh::RadioParamApplyResult trySetTxPower('),
        'DEFAULT_APPLIED': extract_braced(header, 'virtual bool isTxPowerApplied('),
        'PENDING_QUERY': extract_braced(header, 'bool hasPendingTxPower() const'),
    }
    for key, value in values.items():
        code = code.replace('@' + key + '@', value)
    return code


class CommonCLITxPendingTests(unittest.TestCase):
    def test_saved_power_applies_automatically_without_resaving(self):
        code = production_fixture()
        with tempfile.TemporaryDirectory(prefix='meshcore-common-cli-tx-') as directory:
            cpp, exe = Path(directory) / 'test.cpp', Path(directory) / 'test'
            variants = [('production', code, True),
                        ('drops_pending', code.replace(
                            '_tx_power_pending = result == Result::BUSY;',
                            '_tx_power_pending = false;'), False),
                        ('no_background_service', code.replace(
                            '    servicePendingTxPower();', ''), False),
                        ('unverified_applied_claim', code.replace(
                            ' && _callbacks->isTxPowerApplied(_prefs->tx_power_dbm)', ''), False),
                        ('stale_save_ack', code.replace(
                            '    _common_save_result_known = false;\n'
                            '    _common_save_succeeded = false;',
                            '/* stale save acknowledgement */'), False)]
            for name, fixture, succeeds in variants:
                with self.subTest(variant=name):
                    cpp.write_text(fixture, encoding='ascii')
                    flags = [] if os.name == 'nt' else [
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-pie', '-no-pie']
                    built = subprocess.run([
                        os.environ.get('CXX', 'g++'), '-std=c++17', '-Wall', '-Wextra',
                        '-Werror', *flags, '-I', str(ROOT / 'src'), str(cpp), '-o', str(exe),
                    ], text=True, capture_output=True)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    ran = subprocess.run([str(exe)], text=True, capture_output=True)
                    if succeeds:
                        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
                    else:
                        self.assertNotEqual(ran.returncode, 0, name + ' escaped detection')

    def test_actual_completion_callbacks_apply_before_a_saturated_queue_starts_next_tx(self):
        dispatcher = extract_braced((ROOT / 'src/Dispatcher.cpp').read_text(),
                                    'void Dispatcher::loop(')
        completed = extract_braced(dispatcher, 'if (_radio->isSendComplete())')
        self.assertLess(completed.index('_radio->onSendFinished();'),
                        completed.index('onSendComplete(outbound);'))
        self.assertLess(dispatcher.index('onSendComplete(outbound);'), dispatcher.rindex('checkSend();'))
        prefix = production_fixture().split('int main() {', 1)[0]
        fixture = r'''
#define MAX_HASH_SIZE 8
#define PAYLOAD_TYPE_REQ 1
namespace mesh {
namespace tracker { constexpr unsigned kReplyWindowSeconds = 30; }
struct Packet {
  int getPayloadType() const { return 0; }
  void calculatePacketHash(uint8_t* out) const { memset(out, 0, MAX_HASH_SIZE); }
};
struct Mesh {
  virtual void onSendComplete(Packet*) { completion_seen = true; }
};
}
struct TxRole : mesh::Mesh {
  CommonCLI _cli;
  struct Barrier {
    bool complete(mesh::Packet*) { return false; }
    bool waiting() const { return false; }
  } temp_radio_reply_barrier;
  mesh::Packet* pending_battery_alert_packet = nullptr;
  bool battery_alert_sent = false, tracker_pending = false, tracker_sent = false;
  uint64_t uptime_millis = 0, last_battery_alert_sent = 0;
  uint32_t last_millis = 0, tracker_reply_until = 0;
  uint8_t tracker_packet_hash[MAX_HASH_SIZE] = {};
  uint32_t futureMillis(uint32_t delay) const { return millis() + delay; }
  void finishRadioReply(bool) {}
  void processScheduledRadioSettings(bool) {}
  void servicePendingPrimaryRadioSettings(bool) {}
  void onSendComplete(mesh::Packet*) override;
};
@CALLBACK@
int main() {
  // The queue never drains: every completion schedules another packet before
  // CommonCLI::loop gets control again. Only the actual completion hook can
  // admit the queued TX setting in the idle hardware gap.
  for (bool receive_busy_at_first_completion : {false, true}) {
    now_ms = 1000;
    TxRole role;
    auto& cli = role._cli;
    auto& driver = cli.callbacks;
    mesh::Packet packet;
    char reply[160];
    driver.transmitting = true;
    cli.command("set tx 14", reply);
    assert(!strcmp(reply, "OK - saved; pending"));
    assert(cli.hasPendingTxPower() && driver.saves == 1 && driver.hardware == 10);
    unsigned applied_after = 0;
    for (unsigned sent = 1; sent <= 4; ++sent) {
      cli.loop(); assert(driver.transmitting);
      // Dispatcher calls onSendFinished before the role completion callback.
      driver.transmitting = false;
      completion_seen = false;
      driver.require_completion = true;
      if (sent == 1 && receive_busy_at_first_completion) driver.results = {Result::BUSY};
      now_ms += 5000;
      role.onSendComplete(&packet);
      assert(completion_seen && driver.saves == 1);
      if (!receive_busy_at_first_completion || sent > 1) {
        assert(!cli.hasPendingTxPower() && driver.hardware == 14);
        if (!applied_after) applied_after = now_ms;
      } else assert(cli.hasPendingTxPower() && driver.hardware == 10);
      driver.require_completion = false;
      // checkSend() immediately starts the next queued packet. A post-Mesh
      // service would now see BUSY for the entire burst.
      driver.transmitting = true;
      cli.loop();
    }
    assert(applied_after > 0 && applied_after < 30000);
    assert(driver.persisted == 14 && driver.saves == 1);
  }
}
'''
        with tempfile.TemporaryDirectory(prefix='meshcore-common-cli-tx-burst-') as directory:
            cpp, exe = Path(directory) / 'test.cpp', Path(directory) / 'test'
            for role, name, filename in (
                ('simple_repeater', 'MyMesh', 'MyMesh.cpp'),
                ('simple_room_server', 'MyMesh', 'MyMesh.cpp'),
                ('simple_sensor', 'SensorMesh', 'SensorMesh.cpp'),
                ('simple_sensor', 'SensorMesh', 'Tracker.cpp')):
                source = (ROOT / 'examples' / role / filename).read_text()
                callback = preprocess(extract_braced(source, f'void {name}::onSendComplete('))
                callback = callback.replace(name + '::', 'TxRole::')
                for omit_gap in (False, True):
                    with self.subTest(role=role, file=filename, omit_gap=omit_gap):
                        actual = callback.replace('_cli.servicePendingTxPower();', '') if omit_gap else callback
                        cpp.write_text(prefix + fixture.replace('@CALLBACK@', actual), encoding='ascii')
                        flags = [] if os.name == 'nt' else [
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie']
                        built = subprocess.run([
                            os.environ.get('CXX', 'g++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            *flags, '-I', str(ROOT / 'src'), str(cpp), '-o', str(exe),
                        ], capture_output=True, text=True)
                        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                        ran = subprocess.run([str(exe)], capture_output=True, text=True)
                        if omit_gap:
                            self.assertNotEqual(ran.returncode, 0, 'missing completion service escaped detection')
                        else:
                            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_applied_power_diagnostic_and_callbacks_are_hil_only(self):
        common = (ROOT / 'src/helpers/CommonCLI.cpp').read_text()
        header = (ROOT / 'src/helpers/CommonCLI.h').read_text()
        getter = extract_braced(common, 'void CommonCLI::handleGetCmd(')
        self.assertNotIn('"tx.status"', preprocess(getter))
        self.assertIn('"tx.status"', preprocess(getter, hil=True))
        self.assertIn('"tx"', preprocess(getter))

        def check_guard(text, definition):
            guarded = '#if defined(MESH_HIL_TX_FREQ_MHZ)\n' + definition + '\n#endif'
            self.assertIn(guarded, text)
            self.assertNotIn('isTxPowerApplied', preprocess(guarded))
            self.assertIn('isTxPowerApplied', preprocess(guarded, hil=True))

        check_guard(header, '  ' + extract_braced(header, 'virtual bool isTxPowerApplied('))
        for role, name in (('simple_repeater', 'MyMesh'),
                           ('simple_room_server', 'MyMesh'),
                           ('simple_sensor', 'SensorMesh')):
            with self.subTest(role=role):
                source = (ROOT / 'examples' / role / (name + '.cpp')).read_text()
                role_header = (ROOT / 'examples' / role / (name + '.h')).read_text()
                check_guard(source, extract_braced(source, f'bool {name}::isTxPowerApplied('))
                check_guard(role_header, '  bool isTxPowerApplied(int8_t power_dbm) const override;')

    def test_every_infrastructure_role_preserves_tristate_and_pending_work(self):
        for role, name in (('simple_repeater', 'MyMesh'),
                           ('simple_room_server', 'MyMesh'),
                           ('simple_sensor', 'SensorMesh')):
            with self.subTest(role=role):
                source = (ROOT / 'examples' / role / (name + '.cpp')).read_text()
                header = (ROOT / 'examples' / role / (name + '.h')).read_text()
                callback = extract_braced(source,
                    f'mesh::RadioParamApplyResult {name}::trySetTxPower(')
                self.assertIn('return radio_driver.trySetTxPower(power_dbm);', callback)
                applied = extract_braced(source,
                    f'bool {name}::isTxPowerApplied(')
                self.assertIn('return radio_driver.isTxPowerApplied(power_dbm);', applied)
                pending = extract_braced(source,
                    f'mesh::powerdiag::Mask {name}::pendingWorkReason() const')
                # Include pending intent during hardware-failure backoff too:
                # nRF52's event-driven sleep has no guaranteed timer wake.
                self.assertIn('_cli.hasPendingRadioSettings()', pending)
                self.assertLess(pending.index('_cli.hasPendingRadioSettings()'),
                                pending.index('radio_apply_backoff')
                                if 'radio_apply_backoff' in pending else len(pending))
                recovery = extract_braced(header, 'bool canRecoverUsbLogging() const')
                self.assertIn('!_cli.hasPendingRadioSettings()', recovery)
                self.assertIn('_cli.loop();', extract_braced(source, f'void {name}::loop('))


if __name__ == '__main__':
    unittest.main()
