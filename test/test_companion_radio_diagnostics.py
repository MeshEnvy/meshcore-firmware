"""Real shared CLI plus RXEN admission/restoration and existing reset semantics."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from cpp_source import body

ROOT=Path(__file__).resolve().parents[1]

class CompanionRadioDiagnosticsTests(unittest.TestCase):
    def test_production_backend_admission_cli_bounds_and_reset_scope(self):
        wrapper=(ROOT/'src/helpers/radiolib/CustomSX1262Wrapper.h').read_text()
        radio=(ROOT/'src/helpers/radiolib/RadioLibWrappers.cpp').read_text()
        header=(ROOT/'src/helpers/radiolib/RadioLibWrappers.h').read_text()
        dispatcher=(ROOT/'src/Dispatcher.h').read_text()
        tables=(ROOT/'src/helpers/SimpleMeshTables.h').read_text()
        methods='\n'.join(body(wrapper,signature) for signature in (
            'bool supportsRxPowerSavingRfRxDisable() const override',
            'bool setRxPowerSavingRfRxDisabled(bool disabled) override',
            'mesh::RadioParamApplyResult trySetRxPowerSavingRfRxDisabled(bool disabled) override',
            'bool isRxPowerSavingRfRxDisabled() const override'))
        admission='\n'.join(body(radio,signature) for signature in (
            'uint8_t RadioLibWrapper::beginReconfigure()',
            'void RadioLibWrapper::endReconfigure(bool resume_rx)',
            'bool RadioLibWrapper::isInRecvMode() const'))
        with tempfile.TemporaryDirectory(prefix='mesh-companion-radio-diagnostics-') as directory:
            work=Path(directory)
            for name,source in {
                'production_rxen.inc':methods,
                'production_admission.inc':admission,
                'production_radio_reset.inc':body(header,'void resetStats()'),
                'production_dispatcher_reset.inc':body(dispatcher,'void resetStats()'),
                'production_table_reset.inc':body(tables,'void resetStats()'),
            }.items():
                (work/name).write_text(source)
            for rxen,beacons in ((5,0),(5,1),(-1,1),(None,0)):
                with self.subTest(rxen=rxen,beacons=beacons):
                    flags=['-DMESH_ENABLE_TX_BEACONS='+str(beacons)]
                    if rxen is not None:flags.append('-DSX126X_RXEN='+str(rxen))
                    exe=work/('test-'+str(rxen)+'-'+str(beacons))
                    command=[os.environ.get('CXX','g++'),'-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie',
                        '-I'+str(ROOT/'src'),'-I'+str(work),*flags,
                        str(ROOT/'test/fixtures/companion_radio_diagnostics/test.cpp'),'-o',str(exe)]
                    result=subprocess.run(command,capture_output=True,text=True,timeout=60)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    self.assertIn('radio diagnostics cases passed',result.stdout)

if __name__=='__main__':unittest.main()
