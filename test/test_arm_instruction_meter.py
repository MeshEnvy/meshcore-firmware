#!/usr/bin/env python3
"""Keep native and Python ARM per-instruction measurements identical."""
from pathlib import Path
import shutil
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
try:
    import unicorn
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
    from unicorn import arm_const as arm
    import arm_instruction_meter as module
except ImportError:
    unicorn = None


@unittest.skipUnless(unicorn is not None, 'Unicorn is not installed in this host job')
class ArmInstructionMeterTest(unittest.TestCase):
    def machine(self, native):
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(0x1000, 0x1000)
        uc.mem_map(0x20000000, 0x1000)
        # push {r7,lr}; sub sp,32; movs r0,100; loop: subs r0,1;
        # bne loop; add sp,32; pop {r7,pc}. Exactly 205 instructions.
        uc.mem_write(0x1100, bytes.fromhex('80b588b064200138fdd108b080bd'))
        return uc, module.ArmInstructionMeter(uc, prefer_native=native)

    def run_machine(self, machine, meter, top):
        machine.reg_write(arm.UC_ARM_REG_SP, top)
        machine.reg_write(arm.UC_ARM_REG_LR, 0x1001)
        meter.reset(top)
        machine.emu_start(0x1101, 0x1000, timeout=1000000, count=1000)
        self.assertEqual(machine.reg_read(arm.UC_ARM_REG_PC), 0x1000)
        self.assertEqual(machine.reg_read(arm.UC_ARM_REG_SP), top)
        self.assertEqual(machine.reg_read(arm.UC_ARM_REG_R0), 0)
        self.assertEqual(meter.snapshot(), {'minimum_sp': top - 40, 'instructions': 205})
        return meter.snapshot()

    def test_native_python_exact_measurements_repeated_and_independent_contexts(self):
        if unicorn.__version__ != '2.1.4':
            self.skipTest('native private handle API is pinned to Unicorn 2.1.4')
        if not any(shutil.which(name) for name in ('cc', 'gcc', 'clang')):
            self.skipTest('host C compiler unavailable')
        python_uc, python_meter = self.machine(False)
        native_uc, native_meter = self.machine(True)
        second_uc, second_meter = self.machine(True)
        self.assertEqual(native_meter.backend, 'native')
        self.assertEqual(second_meter.backend, 'native')
        for top in (0x20001000, 0x20000f00, 0x20001000):
            expected = self.run_machine(python_uc, python_meter, top)
            self.assertEqual(self.run_machine(native_uc, native_meter, top), expected)
            self.assertEqual(self.run_machine(second_uc, second_meter, top), expected)

    def test_missing_native_compiler_uses_identical_python_measurement(self):
        module._native_library.cache_clear()
        try:
            with mock.patch.object(module.shutil, 'which', return_value=None):
                machine, meter = self.machine(True)
        finally:
            module._native_library.cache_clear()
        self.assertEqual(meter.backend, 'python')
        self.run_machine(machine, meter, 0x20001000)

    def test_unreviewed_unicorn_version_uses_python_measurement(self):
        with mock.patch.object(module.unicorn, '__version__', 'unreviewed-core'):
            machine, meter = self.machine(True)
        self.assertEqual(meter.backend, 'python')
        self.run_machine(machine, meter, 0x20001000)


if __name__ == '__main__':
    unittest.main()
