#!/usr/bin/env python3
"""Keep local cleanup while omitting unreachable nRF52 process-exit callbacks."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("nrf52_flash_trim", ROOT / "scripts/nrf52_flash_trim.py")
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class Nrf52ProgramLifetimeTest(unittest.TestCase):
    def test_automatic_delete_and_explicit_shutdown_remain_and_exit_callback_is_not_registered(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("native G++ unavailable")
        source = r'''
#include <cassert>
extern "C" int __aeabi_atexit(void*, void (*)(void*), void*);
extern "C" int __cxa_atexit(void (*)(void*), void*, void*);
extern "C" void __cxa_finalize(void*);
unsigned closed = 0, destroyed = 0, callback = 0;
void cleanup(void* count) { ++*static_cast<unsigned*>(count); }
struct Resource {
  void close() { ++closed; }
  ~Resource() { close(); ++destroyed; }
};
int main() {
  { Resource automatic; }
  auto* allocated = new Resource;
  delete allocated;
  assert(closed == 2 && destroyed == 2);
  Resource explicit_close;
  explicit_close.close();
  assert(closed == 3 && destroyed == 2);
  unsigned dso = 0;
  assert(__aeabi_atexit(&callback, cleanup, &dso) == 0);
  __cxa_finalize(&dso);
  assert(callback == 0);
  // Positive control: the finalizer really can run a registered callback.
  assert(__cxa_atexit(cleanup, &callback, &dso) == 0);
  __cxa_finalize(&dso);
  assert(callback == 1);
}
'''
        with tempfile.TemporaryDirectory(prefix="mesh-nrf52-lifetime-") as directory:
            directory = Path(directory)
            main, binary = directory / "main.cpp", directory / "checks"
            main.write_text(source, encoding="ascii")
            subprocess.run([compiler, "-std=c++11", "-Os", "-flto", "-DNRF52_PLATFORM=1",
                            "-DMESH_NRF52_NO_PROGRAM_EXIT=1", str(main),
                            str(ROOT / "src/Nrf52ProgramLifetime.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_unknown_and_absent_framework_main_keep_standard_registration(self):
        self.assertFalse(policy.supported_program_lifetime(None))
        with tempfile.TemporaryDirectory(prefix="mesh-nrf52-lifetime-core-") as directory:
            self.assertFalse(policy.supported_program_lifetime(directory))
            main = Path(directory) / "cores/nRF5/main.cpp"
            main.parent.mkdir(parents=True)
            main.write_text("int main() { return 0; }\n", encoding="ascii")
            self.assertFalse(policy.supported_program_lifetime(directory))

    def test_current_pinned_core_is_the_reviewed_nonreturning_main_when_installed(self):
        framework = Path.home() / ".platformio/packages/framework-arduinoadafruitnrf52"
        if not framework.is_dir():
            self.skipTest("pinned nRF52 framework not installed in this host job")
        if not policy.supported_program_lifetime(framework):
            self.skipTest("custom or newer core retains ordinary exit registration")
        main = (framework / "cores/nRF5/main.cpp").read_text()
        self.assertIn("while (1)", main)
        self.assertIn("vTaskStartScheduler();", main)
        self.assertIn("NVIC_SystemReset();", main)


if __name__ == "__main__":
    unittest.main()
