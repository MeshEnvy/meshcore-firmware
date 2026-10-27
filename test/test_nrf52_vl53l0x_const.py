"""Qualify flash-resident VL53L0X tables against the reviewed vendor loader."""

import contextlib
import importlib.util
import io
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "test/fixtures/vl53_const"
SPEC = importlib.util.spec_from_file_location("vl53_const", ROOT / "scripts/nrf52_vl53l0x_const.py")
TRIM = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TRIM)


class Node:
    def __init__(self, path):
        self.path = Path(path)

    def srcnode(self):
        return self

    def get_abspath(self):
        return str(self.path)


class Environment:
    def __init__(self, build):
        self.build = Path(build)

    def subst(self, value):
        assert value == "$BUILD_DIR"
        return str(self.build)

    def File(self, path):
        return Path(path)


class Vl53ConstTests(unittest.TestCase):
    def test_private_copy_is_complete_idempotent_and_downloads_are_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "libdeps/Adafruit_VL53L0X/src"
            texts = {}
            for name, (old, _) in TRIM.REPLACEMENTS.items():
                texts[name] = "// synthetic reviewed fixture\n" + old + "\n"
            texts["vl53l0x_def.h"] = "// ordinary sibling header\n"
            texts["core/src/vl53l0x_api_calibration.cpp"] = '#include "../../vl53l0x_api_core.h"\n'
            for name, text in texts.items():
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(text.replace("\n", "\r\n").encode())
            before = {name: (source / name).read_bytes() for name in texts}
            node = Node(source / "core/src/vl53l0x_api_calibration.cpp")
            env = Environment(root / "build")
            hashes = {name: TRIM.digest(texts[name]) for name in TRIM.FILES}
            with patch.object(TRIM, "FILES", hashes):
                output = TRIM.replace_vl53_source(env, node)
                repeated = TRIM.replace_vl53_source(env, node)
            self.assertEqual(output, repeated)
            copied = root / "build/readonly-nrf52-vl53l0x/src"
            self.assertEqual(output, copied / "core/src/vl53l0x_api_calibration.cpp")
            self.assertEqual({name: (source / name).read_bytes() for name in texts}, before)
            for name, text in texts.items():
                if name in TRIM.REPLACEMENTS:
                    old, new = TRIM.REPLACEMENTS[name]
                    self.assertEqual((copied / name).read_text(), text.replace(old, new))
                else:
                    self.assertEqual((copied / name).read_bytes(), before[name])
            # One changed table, loader, or prototype keeps every original TU.
            for changed in TRIM.FILES:
                path = source / changed
                saved = path.read_bytes()
                path.write_bytes(saved + b"// custom revision\n")
                with patch.object(TRIM, "FILES", hashes), contextlib.redirect_stdout(io.StringIO()):
                    self.assertIs(TRIM.replace_vl53_source(env, node), node)
                path.write_bytes(saved)

    def test_real_vendor_tables_produce_identical_register_writes_and_parameters(self):
        original = (FIXTURE / "tuning_loader.inc").read_text()
        mutable = original.replace("VL53L0X_load_tuning_settings", "load_original")
        readonly = original.replace("VL53L0X_load_tuning_settings", "load_readonly").replace(
            *TRIM.REPLACEMENTS["core/src/vl53l0x_api_core.cpp"])
        tables = "\n".join((FIXTURE / name).read_text().replace(*TRIM.REPLACEMENTS[name])
                           .replace('#include "vl53l0x_def.h"', '')
                           for name in ("vl53l0x_tuning.h", "vl53l0x_interrupt_threshold_settings.h"))
        source = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <type_traits>
struct Device {
  unsigned SigmaEstRefArray=0, SigmaEstEffPulseWidth=0, SigmaEstEffAmbWidth=0, targetRefRate=0;
  unsigned calls=0, fail=0;
  std::vector<uint8_t> writes;
};
using VL53L0X_Error = int;
using VL53L0X_DEV = Device*;
#define VL53L0X_ERROR_NONE 0
#define VL53L0X_ERROR_INVALID_PARAMS 1
#define LOG_FUNCTION_START(...)
#define LOG_FUNCTION_END(...)
#define PALDevDataSet(device, member, value) ((device)->member = (value))
#define VL53L0X_MAKEUINT16(low, high) (uint16_t(low) | (uint16_t(high) << 8))
int VL53L0X_WriteMulti(Device* device, uint8_t address, uint8_t* bytes, unsigned count) {
  device->writes.push_back(address); device->writes.push_back(uint8_t(count));
  device->writes.insert(device->writes.end(),bytes,bytes+count);
  return ++device->calls == device->fail ? 2 : 0;
}
@TABLES@
@MUTABLE@
@READONLY@
static_assert(std::is_const<typename std::remove_extent<decltype(DefaultTuningSettings)>::type>::value,
              "default tuning must live in readonly storage");
int main() {
  const struct { const uint8_t* bytes; unsigned size; } inputs[] = {
    {DefaultTuningSettings,sizeof(DefaultTuningSettings)},
    {InterruptThresholdSettings,sizeof(InterruptThresholdSettings)},
  };
  assert(inputs[0].size == 243 && inputs[1].size == 438);
  for (const auto& input:inputs) {
    for(unsigned fail=0;fail<input.size;++fail) {
      std::vector<uint8_t> copy(input.bytes,input.bytes+input.size);
      Device before, after; before.fail=after.fail=fail;
      assert(load_original(&before,copy.data()) == load_readonly(&after,input.bytes));
      assert(before.calls==after.calls && before.writes==after.writes);
      assert(before.SigmaEstRefArray==after.SigmaEstRefArray);
      assert(before.SigmaEstEffPulseWidth==after.SigmaEstEffPulseWidth);
      assert(before.SigmaEstEffAmbWidth==after.SigmaEstEffAmbWidth);
      assert(before.targetRefRate==after.targetRefRate);
      assert(memcmp(copy.data(),input.bytes,input.size)==0);
    }
  }
  // The caller-owned custom tuning API keeps accepting ordinary mutable data.
  uint8_t custom[] = {0xFF,0,0x01,0x23,2,0x10,0xAA,0xBB,0};
  uint8_t snapshot[sizeof(custom)]; memcpy(snapshot,custom,sizeof(custom));
  Device before,after;
  assert(load_original(&before,custom)==load_readonly(&after,custom));
  assert(before.SigmaEstRefArray==after.SigmaEstRefArray && before.writes==after.writes);
  assert(memcmp(custom,snapshot,sizeof(custom))==0);
}
'''.replace("@TABLES@", tables).replace("@MUTABLE@", mutable).replace("@READONLY@", readonly)
        with tempfile.TemporaryDirectory() as directory:
            path, binary = Path(directory) / "loader.cpp", Path(directory) / "loader"
            path.write_text(source, encoding="ascii")
            built = subprocess.run(["g++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                                    "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                                    str(path), "-o", str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_arm_table_bytes_move_to_readonly_without_changing_their_size(self):
        compiler = shutil.which("arm-none-eabi-g++")
        nm = shutil.which("arm-none-eabi-nm")
        if not compiler or not nm:
            self.skipTest("ARM toolchain required")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for readonly in (False, True):
                text = "#include <stdint.h>\n"
                for name in ("vl53l0x_tuning.h", "vl53l0x_interrupt_threshold_settings.h"):
                    table = (FIXTURE / name).read_text().replace('#include "vl53l0x_def.h"', '')
                    if readonly:
                        table = table.replace(*TRIM.REPLACEMENTS[name])
                    text += table + "\n"
                text += '''
extern "C" const uint8_t* default_table() { return DefaultTuningSettings; }
extern "C" const uint8_t* threshold_table() { return InterruptThresholdSettings; }
'''
                path, obj = root / "tables.cpp", root / "tables.o"
                path.write_text(text, encoding="ascii")
                built = subprocess.run([compiler,"-std=c++17","-mcpu=cortex-m4","-mthumb","-Os",
                                        "-c",str(path),"-o",str(obj)],capture_output=True,text=True,timeout=30)
                self.assertEqual(built.returncode,0,built.stdout+built.stderr)
                symbols = subprocess.check_output([nm,"-S","-C",str(obj)],text=True)
                for name,size in (("DefaultTuningSettings",243),("InterruptThresholdSettings",438)):
                    line = next(line for line in symbols.splitlines() if line.endswith(" "+name))
                    _, length, kind, _ = line.split()
                    self.assertEqual(int(length,16),size)
                    self.assertEqual(kind.lower(),"r" if readonly else "d")


if __name__ == "__main__":
    unittest.main()
