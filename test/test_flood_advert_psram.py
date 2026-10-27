"""Keep every advert-history decision while moving its complete table to PSRAM."""
from pathlib import Path
import os
import resource
import signal
import subprocess
import tempfile
import unittest

from test_flood_advert_rtc import HARNESS

ROOT = Path(__file__).resolve().parents[1]

HEAP = r'''
#pragma once
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#define HEAP_CHECK(value) do { if (!(value)) std::abort(); } while (0)
enum { MALLOC_CAP_INTERNAL=1, MALLOC_CAP_DMA=2,
       MALLOC_CAP_SPIRAM=4, MALLOC_CAP_8BIT=8 };
namespace heap_test {
struct Block { size_t bytes; unsigned caps; };
struct Heap {
  bool psram = true, internal = true;
  size_t dma_free = 100000, dma_largest = 100000;
  unsigned external_calls = 0, internal_calls = 0, frees = 0;
  size_t last_bytes = 0;
  std::map<void*, Block> blocks;
};
inline Heap& heap() { static Heap value; return value; }
}
inline void* heap_caps_malloc(size_t size, unsigned caps) {
  using namespace heap_test;
  Heap& state = heap();
  state.last_bytes = size;
  const bool external = (caps & MALLOC_CAP_SPIRAM) != 0;
  if (external) ++state.external_calls; else ++state.internal_calls;
  if (external ? !state.psram : !state.internal) return nullptr;
  if (!external) {
    HEAP_CHECK(caps & MALLOC_CAP_INTERNAL);
    HEAP_CHECK(size <= state.dma_largest && size <= state.dma_free - 16384);
    state.dma_free -= size;
  }
  void* memory = std::malloc(size);
  HEAP_CHECK(memory);
  std::memset(memory, 0xa5, size);
  state.blocks[memory] = {size, caps};
  return memory;
}
inline void* heap_caps_calloc(size_t count, size_t size, unsigned caps) {
  void* memory = heap_caps_malloc(count * size, caps);
  if (memory) std::memset(memory, 0, count * size);
  return memory;
}
inline void heap_caps_free(void* memory) {
  if (!memory) return;
  using namespace heap_test;
  Heap& state = heap();
  const auto found = state.blocks.find(memory);
  HEAP_CHECK(found != state.blocks.end());
  if (!(found->second.caps & MALLOC_CAP_SPIRAM))
    state.dma_free += found->second.bytes;
  state.blocks.erase(found);
  ++state.frees;
  std::free(memory);
}
inline size_t heap_caps_get_free_size(unsigned caps) {
  HEAP_CHECK(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  return heap_test::heap().dma_free;
}
inline size_t heap_caps_get_largest_free_block(unsigned caps) {
  HEAP_CHECK(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  return heap_test::heap().dma_largest;
}
'''

WRAPPER = r'''
int main(int argc, char** argv) {
  auto& heap = heap_test::heap();
  const char* mode = argc > 1 ? argv[1] : "trace";
  const bool fallback = !std::strcmp(mode, "fallback");
  const bool lifetime = !std::strcmp(mode, "lifetime");
  if (fallback || !std::strcmp(mode, "no-memory")
      || !std::strcmp(mode, "reserve") || !std::strcmp(mode, "fragmented"))
    heap.psram = false;
  if (!std::strcmp(mode, "no-memory")) heap.internal = false;
  if (!std::strcmp(mode, "reserve")) heap.dma_free = 16384 + 17920 - 1;
  if (!std::strcmp(mode, "fragmented")) heap.dma_largest = 17920 - 1;
  char trace[] = "trace";
  char life[] = "lifetime";
  char* args[] = {argv[0], lifetime ? life : trace, nullptr};
  const size_t dma_before = heap.dma_free;
  const int result = traceMain(2, args);
  CHECK(heap.blocks.empty() && heap.dma_free == dma_before);
#if MESH_REPEATER_PSRAM_TABLES
  CHECK(heap.last_bytes == 128 * sizeof(mesh::FloodAdvertLimiter::Entry));
  CHECK(heap.external_calls == (lifetime ? 3u : 1u));
  CHECK(heap.internal_calls == (fallback ? 1u : 0u));
  CHECK(heap.frees == (lifetime ? 3u : 1u));
#else
  CHECK(heap.external_calls == 0 && heap.internal_calls == 0 && heap.frees == 0);
#endif
  return result;
}
'''


class FloodAdvertPsramTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="advert-psram-")
        cls.work = Path(cls.temp.name)
        (cls.work / "esp_heap_caps.h").write_text(HEAP)
        (cls.work / "sdkconfig.h").write_text(
            "#define CONFIG_IDF_TARGET_ESP32S3 1\n"
            "#define CONFIG_SPIRAM_USE_MALLOC 1\n")
        cls.compiler = [os.environ.get("CXX", "g++")]
        cls.binaries = {}

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def build(self, enabled, collisions=False):
        key = (enabled, collisions)
        if key in self.binaries:
            return self.binaries[key]
        name = "probe-%d-%d" % key
        source = self.work / (name + ".cpp")
        binary = self.work / name
        code = HARNESS.replace("@BANKS@", "").replace("int main(", "int traceMain(")
        code = code.replace(
            "#define MAKE_LIMITER(name) mesh::StaticFloodAdvertLimiter<> name\n",
            "#define MAKE_LIMITER(name) \\\n"
            " const unsigned name##_calls = heap_test::heap().external_calls + heap_test::heap().internal_calls; \\\n"
            " mesh::StaticFloodAdvertLimiter<> name; \\\n"
            " CHECK(heap_test::heap().external_calls + heap_test::heap().internal_calls == name##_calls); \\\n"
            " CHECK(name.begin()); \\\n"
            " const unsigned name##_initialized = heap_test::heap().external_calls + heap_test::heap().internal_calls; \\\n"
            " CHECK(name.begin()); \\\n"
            " CHECK(heap_test::heap().external_calls + heap_test::heap().internal_calls == name##_initialized)\n")
        # Release builds of the limiter are exercised while harness assertions
        # stay active. The trace also checks exact full-key prefix collisions.
        code = code.rstrip()[:-1] + " return 0;\n}\n"
        if collisions:
            code = code.replace("key[0] = uint8_t(i); key[1] = uint8_t(i >> 8);",
                                "key[0] = uint8_t(i / 2); key[1] = uint8_t(i >> 8);")
        source.write_text("#include <esp_heap_caps.h>\n" + code + WRAPPER)
        args = ["-std=c++11", "-Os", "-DNDEBUG", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-pie", "-no-pie", "-DESP32_PLATFORM=1", "-DESP_PLATFORM=1",
                "-DBOARD_HAS_PSRAM=1", "-DSTATION_G3_ESP32=1",
                "-DMESH_CLIENT_REPEATER_ONLY=1",
                "-DMESH_REPEATER_PSRAM_TABLES=" + str(enabled),
                "-I", str(self.work), "-isystem", str(ROOT / "src"),
                "-isystem", str(ROOT / "test/mocks")]
        result = subprocess.run(
            self.compiler + args + [str(source),
                str(ROOT / "src/helpers/FloodAdvertLimiter.cpp"), "-o", str(binary)],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.binaries[key] = binary
        return binary

    def run_probe(self, enabled, mode="trace", collisions=False):
        return subprocess.run([str(self.build(enabled, collisions)), mode],
                              capture_output=True, text=True, timeout=20)

    def test_full_capacity_quota_duplicate_badlist_and_rollover_traces_match(self):
        for collisions in (False, True):
            with self.subTest(collisions=collisions):
                baseline = self.run_probe(0, collisions=collisions)
                self.assertEqual(baseline.returncode, 0, baseline.stderr)
                self.assertIn("limited=128", baseline.stdout)
                for mode in ("trace", "fallback"):
                    result = self.run_probe(1, mode, collisions)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, baseline.stdout)

    def test_owner_recreation_clears_history_and_releases_every_allocation(self):
        baseline = self.run_probe(0, "lifetime")
        result = self.run_probe(1, "lifetime")
        self.assertEqual(baseline.returncode, 0, baseline.stderr)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, baseline.stdout)

    def test_exhausted_or_reserved_internal_memory_stops_startup(self):
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        binary = self.build(1)
        for mode in ("no-memory", "reserve", "fragmented"):
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), mode], capture_output=True,
                    text=True, preexec_fn=no_core, timeout=20)
                self.assertEqual(result.returncode, -signal.SIGABRT, result.stderr)
                self.assertIn("Cannot allocate advert source history (17920 bytes)",
                              result.stderr)
                self.assertNotIn("runtime error:", result.stderr)
                self.assertNotIn("AddressSanitizer", result.stderr)

    def test_selected_owner_retains_no_full_internal_fallback(self):
        source = self.work / "layout.cpp"
        source.write_text('#include <helpers/FloodAdvertLimiter.h>\n'
            'static_assert(sizeof(mesh::StaticFloodAdvertLimiter<>) < 256, '
            '"advert owner must not retain a full inline table");\n')
        result = subprocess.run(self.compiler + ["-std=c++11", "-fsyntax-only",
            "-DESP32_PLATFORM=1", "-DESP_PLATFORM=1", "-DBOARD_HAS_PSRAM=1",
            "-DSTATION_G3_ESP32=1", "-DMESH_CLIENT_REPEATER_ONLY=1",
            "-DMESH_REPEATER_PSRAM_TABLES=1", "-I", str(self.work),
            "-isystem", str(ROOT / "src"), "-isystem", str(ROOT / "test/mocks"),
            str(source)], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
