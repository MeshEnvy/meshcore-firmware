#!/usr/bin/env python3
"""Exercise the production migration arm/tick state machine with host stubs.

The ARM SVC adapter is replaced by a failing trap, and the EndF stub refuses
installation before hardware access. This tests scheduling and cancellation,
not MBR copying or power-loss recovery; those require actual hardware.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
STUBS = ROOT / 'tools/mota/tests/migration_stubs'


class MigrationRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='migration-runtime-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / ('runtime.exe' if os.name == 'nt' else 'runtime')
        # The driver uses Cortex-M's 32-bit flash addresses. On a 64-bit host,
        # -fpermissive allows the unreachable pointer-to-address conversion.
        subprocess.run([
            os.environ.get('CXX', 'g++'), '-std=c++17', '-O2', '-Wall', '-Wextra',
            '-fpermissive', '-DOTA_BOOTLOADER_MIGRATION', '-DNRF52_PLATFORM', '-DRAK_4631',
            '-I', str(STUBS), '-I', str(ROOT / 'src'), str(STUBS / 'runtime.cpp'),
            '-o', str(cls.binary),
        ], check=True)

    def run_scenario(self, scenario):
        subprocess.run([str(self.binary), scenario], check=True)

    def test_wrong_confirmation_cannot_arm(self):
        self.run_scenario('wrong-confirmation')

    def test_grace_and_radio_drain_are_both_required(self):
        self.run_scenario('grace-and-drain')

    def test_duplicate_arm_does_not_extend_deadline(self):
        self.run_scenario('duplicate-arm')

    def test_timeout_wins_over_becoming_idle(self):
        self.run_scenario('timeout')

    def test_ota_activity_cancels_and_requires_fresh_arm(self):
        self.run_scenario('ota-cancel-rearm')

    def test_ota_activity_wins_at_install_boundary(self):
        self.run_scenario('ota-cancel-at-install')

    def test_millisecond_wrap_preserves_grace(self):
        self.run_scenario('clock-wrap')

    def test_current_results_override_stale_retained_marker(self):
        self.run_scenario('stale-retained-result')


if __name__ == '__main__':
    unittest.main()
