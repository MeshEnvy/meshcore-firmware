#!/usr/bin/env python3
"""Exercise the actual WS wrapper with SDK allocation/lifetime stubs."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]

HEADERS = {
    "esp_idf_version.h": "#pragma once\n",
    "sdkconfig.h": "#pragma once\n#define CONFIG_WS_BUFFER_SIZE 1024\n",
    "esp_transport.h": """
#pragma once
#include <stdint.h>
struct TestTransport;
typedef TestTransport* esp_transport_handle_t;
extern "C" void* esp_transport_get_context_data(esp_transport_handle_t);
extern "C" int esp_transport_destroy(esp_transport_handle_t);
""",
    "esp_transport_ws.h": '#pragma once\n#include "esp_transport.h"\n',
    "esp_heap_caps.h": """
#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_INTERNAL 4U
#define MALLOC_CAP_DMA 8U
void* heap_caps_malloc(size_t, uint32_t);
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_largest_free_block(uint32_t);
""",
}

HARNESS = r"""
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <stdlib.h>
#include "esp_heap_caps.h"

static void* padded_malloc(size_t);
static void sdk_free(void*);
#define malloc padded_malloc
#define free sdk_free
#include "src/helpers/ESP32WsTransportFix.cpp"
#undef free
#undef malloc

struct TestTransport {
    void* context;
};

#if ESP_IDF_VERSION_MAJOR == 4
using TestWs = transport_ws_t;
#else
struct TestWs {
    char* path;
    char* buffer;
    char* sub_protocol;
    char* user_agent;
    char* headers;
    esp_transport_handle_t parent;
};
#endif

enum Kind { SDK, GENERIC, EXTERNAL, INTERNAL };
struct Allocation {
    void* ptr;
    size_t size;
    Kind kind;
};
static Allocation allocations[32];
static size_t allocation_count;
static unsigned destroy_calls;
static unsigned original_frees;
static unsigned generic_calls;
static unsigned external_calls;
static unsigned internal_calls;
static unsigned free_queries;
static unsigned largest_queries;
static bool fail_sdk;
static bool fail_external;
static bool fail_internal;
static bool fail_generic;
static size_t dma_free = 65536;
static size_t dma_largest = 8192;
static char* original;
static TestTransport parent;

static void* allocate(size_t bytes, Kind kind) {
    assert(allocation_count < 32);
    void* p = std::calloc(1, bytes);
    assert(p);
    allocations[allocation_count++] = {p, bytes, kind};
    return p;
}

static Allocation lookup(void* ptr) {
    for (size_t i = 0; i < allocation_count; ++i) {
        if (allocations[i].ptr == ptr) return allocations[i];
    }
    std::abort();
}

static void sdk_free(void* ptr) {
    if (!ptr) return;
    for (size_t i = 0; i < allocation_count; ++i) {
        if (allocations[i].ptr != ptr) continue;
        if (ptr == original) ++original_frees;
        allocations[i] = allocations[--allocation_count];
        std::free(ptr);
        return;
    }
    std::abort();  // Double free, wrong allocator, or parent destruction.
}

static void* padded_malloc(size_t bytes) {
    ++generic_calls;
    assert(bytes == 1025);
    return fail_generic ? nullptr : allocate(bytes, GENERIC);
}

void* heap_caps_malloc(size_t bytes, uint32_t caps) {
    assert(bytes == 1025);
    if (caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
        ++external_calls;
        return fail_external ? nullptr : allocate(bytes, EXTERNAL);
    }
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    ++internal_calls;
    assert(dma_free >= 16384 + bytes && dma_largest >= bytes);
    return fail_internal ? nullptr : allocate(bytes, INTERNAL);
}

size_t heap_caps_get_free_size(uint32_t caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    ++free_queries;
    return dma_free;
}

size_t heap_caps_get_largest_free_block(uint32_t caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    ++largest_queries;
    return dma_largest;
}

extern "C" void* esp_transport_get_context_data(esp_transport_handle_t t) {
    assert(t && t != &parent);
    return t->context;
}

extern "C" int esp_transport_destroy(esp_transport_handle_t t) {
    assert(t && t != &parent);
    ++destroy_calls;
    TestWs* ws = static_cast<TestWs*>(t->context);
    assert(ws->parent == &parent);
    sdk_free(ws->buffer);
    sdk_free(ws->path);
    sdk_free(ws->sub_protocol);
    sdk_free(ws->user_agent);
    sdk_free(ws->headers);
    sdk_free(ws);
    sdk_free(t);
    return 0;
}

extern "C" esp_transport_handle_t __real_esp_transport_ws_init(esp_transport_handle_t p) {
    assert(p == &parent);
    if (fail_sdk) return nullptr;
    auto* t = static_cast<TestTransport*>(allocate(sizeof(TestTransport), SDK));
    auto* ws = static_cast<TestWs*>(allocate(sizeof(TestWs), SDK));
    t->context = ws;
    ws->parent = p;
    ws->path = static_cast<char*>(allocate(2, SDK));
    ws->sub_protocol = static_cast<char*>(allocate(5, SDK));
    ws->user_agent = static_cast<char*>(allocate(8, SDK));
    ws->headers = static_cast<char*>(allocate(8, SDK));
    ws->buffer = static_cast<char*>(allocate(1024, SDK));
    original = ws->buffer;
    return t;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario(argv[1]);
    bool expect_failure = false;
    Kind expected = GENERIC;
    if (scenario == "sdk_failure") {
        fail_sdk = true;
        expect_failure = true;
    } else if (scenario == "external_success") {
        // External success must not inspect or consume scarce internal memory.
        dma_free = 0;
        dma_largest = 0;
        expected = EXTERNAL;
    } else if (scenario == "internal_boundary") {
        fail_external = true;
        dma_free = 16384 + 1025;
        dma_largest = 1025;
        expected = INTERNAL;
    } else if (scenario == "reserve_denied") {
        fail_external = true;
        dma_free = 16384 + 1024;
        expect_failure = true;
    } else if (scenario == "largest_denied") {
        fail_external = true;
        dma_largest = 1024;
        expect_failure = true;
    } else if (scenario == "both_fail") {
        fail_external = true;
        fail_internal = true;
        expect_failure = true;
    } else if (scenario == "generic_failure") {
        fail_generic = true;
        expect_failure = true;
    } else {
        assert(scenario == "generic_success" || scenario == "idf5_passthrough");
    }

    esp_transport_handle_t t = __wrap_esp_transport_ws_init(&parent);
    if (expect_failure) {
        assert(t == nullptr);
        assert(allocation_count == 0);
        assert(destroy_calls == (fail_sdk ? 0U : 1U));
        assert(original_frees == (fail_sdk ? 0U : 1U));
    } else {
        assert(t && destroy_calls == 0);
        auto* ws = static_cast<TestWs*>(t->context);
        Allocation a = lookup(ws->buffer);
        if (scenario == "idf5_passthrough") {
            assert(ws->buffer == original && a.size == 1024 && a.kind == SDK);
            assert(original_frees == 0);
        } else {
            assert(ws->buffer != original && a.size == 1025 && a.kind == expected);
            assert(original_frees == 1);
            // Exercise the exact off-by-one index the workaround must retain.
            ws->buffer[1024] = '\0';
        }
        esp_transport_destroy(t);
        assert(allocation_count == 0 && destroy_calls == 1 && original_frees == 1);
    }
    if (scenario == "external_success") {
        assert(external_calls == 1 && internal_calls == 0 && generic_calls == 0);
        assert(free_queries == 0 && largest_queries == 0);
    } else if (scenario == "reserve_denied" || scenario == "largest_denied") {
        assert(external_calls == 1 && internal_calls == 0 && generic_calls == 0);
        assert(free_queries == 1 && largest_queries == 1);
    } else if (scenario == "internal_boundary" || scenario == "both_fail") {
        assert(external_calls == 1 && internal_calls == 1 && generic_calls == 0);
        assert(free_queries == 1 && largest_queries == 1);
    } else if (scenario == "generic_success" || scenario == "generic_failure") {
        assert(external_calls == 0 && internal_calls == 0 && generic_calls == 1);
    } else {
        assert(external_calls == 0 && internal_calls == 0 && generic_calls == 0);
    }
}
"""


class WsTransportPsramTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise unittest.SkipTest("C++ compiler unavailable")
        cls.temp = tempfile.TemporaryDirectory(prefix="ws-transport-psram-")
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        for name, contents in HEADERS.items():
            (work / name).write_text(contents, encoding="ascii")
        source = work / "harness.cpp"
        source.write_text(HARNESS, encoding="ascii")
        cls.binaries = {}
        variants = {
            "g3_psram": (4, ["STATION_G3_ESP32", "BOARD_HAS_PSRAM"]),
            "generic_psram": (4, ["BOARD_HAS_PSRAM"]),
            "classic_psram": (4, ["CONFIG_IDF_TARGET_ESP32", "BOARD_HAS_PSRAM"]),
            "s3_psram": (4, ["CONFIG_IDF_TARGET_ESP32S3", "BOARD_HAS_PSRAM"]),
            "g3_internal": (4, ["STATION_G3_ESP32"]),
            "other_internal": (4, []),
            "idf5": (5, ["STATION_G3_ESP32", "BOARD_HAS_PSRAM"]),
        }
        for name, (idf, macros) in variants.items():
            binary = work / name
            command = [
                compiler, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-DESP_PLATFORM", f"-DESP_IDF_VERSION_MAJOR={idf}",
                *[f"-D{macro}" for macro in macros], "-I", str(work), "-I", str(ROOT),
            ]
            if os.name != "nt":
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-pie", "-no-pie"]
            built = subprocess.run([*command, str(source), "-o", str(binary)],
                                   capture_output=True, text=True)
            if built.returncode:
                raise RuntimeError(built.stdout + built.stderr)
            cls.binaries[name] = binary

    def run_scenario(self, variant, scenario):
        result = subprocess.run([str(self.binaries[variant]), scenario],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_board_agnostic_psram_and_guarded_failure_lifecycles(self):
        for variant in ["g3_psram", "generic_psram", "classic_psram", "s3_psram"]:
            for scenario in ["external_success", "internal_boundary", "reserve_denied",
                             "largest_denied", "both_fail", "sdk_failure"]:
                with self.subTest(variant=variant, scenario=scenario):
                    self.run_scenario(variant, scenario)

    def test_other_target_allocation_defaults_and_failure_cleanup(self):
        for variant in ["g3_internal", "other_internal"]:
            for scenario in ["generic_success", "generic_failure", "sdk_failure"]:
                with self.subTest(variant=variant, scenario=scenario):
                    self.run_scenario(variant, scenario)

    def test_idf5_is_unchanged_passthrough(self):
        self.run_scenario("idf5", "idf5_passthrough")
        self.run_scenario("idf5", "sdk_failure")


if __name__ == "__main__":
    unittest.main()
