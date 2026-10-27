#!/usr/bin/env python3
"""Check late external ownership through the real startup and storage methods."""
from pathlib import Path
import importlib.util
import os
import re
import resource
import shlex
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def block(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


PREAMBLE = r'''
#include <helpers/CompanionPsramMesh.h>
#include <helpers/CompanionFrameLimits.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>
#define MESH_DEBUG_PRINTLN(...) ((void)0)
#define MAX_CONTACTS 350
#define MAX_ANON_CONTACTS 8
#define ONE_KEY_DM_COMPACT_HELD 0
#define ONE_KEY_DM_SHARED_OFFLINE_QUEUE 0
#define MAX_HELD_ONE_KEY_DMS 15
#define ONE_KEY_DM_ID_SIZE 8
#define PUB_KEY_SIZE 32
#define MAX_TEXT_LEN 160
#define MESH_CONTACT_CACHE 0
static void check(bool value) { if (!value) std::abort(); }
static bool board_ready = false, present = true;
static int fail_calloc = 0, allocated = 0, constructed = 0, destroyed = 0;
static int construct_action = 0;
static std::vector<size_t> sizes;
static std::string diagnostic;
bool psramFound() { return present; }
void* heap_caps_calloc(size_t count, size_t size, unsigned caps) {
    check(board_ready && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++allocated; sizes.push_back(count * size);
    if (fail_calloc) { --fail_calloc; return nullptr; }
    return std::calloc(count, size);
}
void* heap_caps_malloc(size_t size, unsigned caps) {
    check(board_ready && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++allocated; sizes.push_back(size);
    return std::malloc(size);
}
void heap_caps_free(void* value) { std::free(value); }
namespace mesh {
struct Packet { uint8_t bytes[280]; };
struct Log {
    void println(const char* text) { diagnostic += text; diagnostic += '\n'; }
};
static Log logger;
Log& usbLoggingPort() { return logger; }
}
struct Board { void begin() { board_ready = true; } } board;
void delay(unsigned) {
    std::printf("halted allocations=%d constructors=%d %s", allocated, constructed,
                diagnostic.c_str());
    std::exit(77);
}
using ContactIndex = uint16_t;
struct ContactInfo { uint8_t bytes[184]; };
struct BaseChatMesh {
    ContactInfo* contacts;
    ContactInfo contacts_fallback[24];
    ContactIndex* sort_array;
    ContactIndex sort_array_fallback[24];
    int contact_capacity, num_contacts;
    BaseChatMesh() : contacts(contacts_fallback), sort_array(sort_array_fallback),
                    contact_capacity(24), num_contacts(8) {}
    ~BaseChatMesh() {
        if (contacts != contacts_fallback) heap_caps_free(contacts);
        if (sort_array != sort_array_fallback) heap_caps_free(sort_array);
    }
    void onContactReferenceChanged(ContactInfo*, ContactInfo*) {}
    bool initializeContactStorage();
};
struct Dummy {};
Dummy radio_driver, fast_rng, rtc_clock, tables, store, ui_task;
struct MyMesh;
MyMesh& companionMeshInstance();
static bool initializeCompanionMeshStorage();
struct MyMesh : BaseChatMesh {
@FRAME@;
    Frame* offline_queue;
    Frame offline_queue_fallback[16];
    int offline_queue_capacity, offline_queue_head, offline_queue_len;
@HELD@;
    HeldOneKeyDM held_dms[MAX_HELD_ONE_KEY_DMS];
    uint8_t untouched[128];
    MyMesh(Dummy&, Dummy&, Dummy&, Dummy&, Dummy&, Dummy*) {
        check(board_ready); ++constructed;
        // These deliberately uninitialized fields used to receive static-zero
        // initialization. The real late owner must preserve that guarantee.
        for (uint8_t byte : untouched) check(byte == 0);
        for (const auto& held : held_dms)
            for (uint8_t byte : held.packet.bytes) check(byte == 0);
        offline_queue = offline_queue_fallback;
        offline_queue_capacity = 16;
        offline_queue_head = offline_queue_len = 0;
        if (construct_action == 1) (void)companionMeshInstance();
        if (construct_action == 2) (void)initializeCompanionMeshStorage();
    }
    ~MyMesh() {
        ++destroyed;
        if (offline_queue != offline_queue_fallback) heap_caps_free(offline_queue);
    }
    int getOfflineQueueCapacity() const;
    Frame& offlineQueueFrameAt(int);
    void initializeOfflineQueue();
    bool addToOfflineQueue(const uint8_t*, int);
    int peekOfflineQueue(uint8_t*);
    void popOfflineQueue();
};
@CONSTANTS@
@CONTACT_METHOD@
@METHODS@
#define the_mesh companionMeshInstance()
@FACTORY@
static void setupStorage() {
@SETUP@
}
static_assert(!std::is_copy_constructible<mesh::CompanionPsramMesh<MyMesh>>::value,
              "A singleton owner must not be copied");
static_assert(!std::is_move_constructible<mesh::CompanionPsramMesh<MyMesh>>::value,
              "A published mesh must not move");
int main(int argc, char** argv) {
    check(allocated == 0 && constructed == 0 && !board_ready);
    std::string mode = argc > 1 ? argv[1] : "success";
    if (mode == "preinit") { (void)the_mesh; return 0; }
    if (mode == "missing") present = false;
    if (mode == "allocation-failure") fail_calloc = 1;
    if (mode == "partial-access") construct_action = 1;
    if (mode == "recursive-begin") construct_action = 2;
    setupStorage();
    MyMesh* address = &the_mesh;
    check(constructed == 1 && allocated == 1 && sizes[0] == sizeof(MyMesh));
    check(the_mesh.initializeContactStorage());
    the_mesh.initializeOfflineQueue();
    check(the_mesh.contact_capacity == 358);
    check(the_mesh.getOfflineQueueCapacity() == 512);
    for (int i = 0; i < 350; ++i) the_mesh.contacts[i + 8].bytes[0] = uint8_t(i);
    for (int i = 0; i < 15; ++i) {
        std::memset(the_mesh.held_dms[i].sender_key, i + 1, PUB_KEY_SIZE);
        std::memset(the_mesh.held_dms[i].packet.bytes, i + 1, 280);
    }
    for (int i = 0; i < 512; ++i) {
        uint8_t bytes[MAX_FRAME_SIZE]; std::memset(bytes, i, sizeof(bytes));
        bytes[0] = 0xFE;
        check(the_mesh.addToOfflineQueue(bytes, sizeof(bytes)));
    }
    uint8_t extra[MAX_FRAME_SIZE] = {0xFE};
    check(!the_mesh.addToOfflineQueue(extra, sizeof(extra)));
    int before = allocated;
    check(initializeCompanionMeshStorage());
    check(&the_mesh == address && allocated == before && constructed == 1);
    for (int i = 0; i < 350; ++i) check(the_mesh.contacts[i + 8].bytes[0] == uint8_t(i));
    for (int i = 0; i < 15; ++i) {
        check(the_mesh.held_dms[i].sender_key[31] == i + 1);
        check(the_mesh.held_dms[i].packet.bytes[279] == i + 1);
    }
    for (int i = 0; i < 512; ++i) {
        uint8_t bytes[MAX_FRAME_SIZE];
        check(the_mesh.peekOfflineQueue(bytes) == MAX_FRAME_SIZE && bytes[0] == 0xFE);
        for (int j = 1; j < MAX_FRAME_SIZE; ++j) check(bytes[j] == uint8_t(i));
        the_mesh.popOfflineQueue();
    }
    check(the_mesh.offline_queue_len == 0 && the_mesh.offline_queue_head == 0);
    std::printf("512 frames, 15 held DMs, 350 contacts; one complete external object; zero state retained\n");
}
'''


class CompanionPsramMeshTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='companion-psram-mesh-')
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        (self.work / 'sdkconfig.h').write_text('#pragma once\n')
        (self.work / 'esp_heap_caps.h').write_text(
            '#pragma once\n#include <stddef.h>\n#define MALLOC_CAP_SPIRAM 0x400\n'
            '#define MALLOC_CAP_8BIT 0x4\nvoid* heap_caps_calloc(size_t,size_t,unsigned);\n'
            'void* heap_caps_malloc(size_t,unsigned);\nvoid heap_caps_free(void*);\n')
        (self.work / 'esp32-hal-psram.h').write_text('#pragma once\nbool psramFound();\n')
        self.compiler = shlex.split(os.environ.get('CXX', 'g++'))
        self.defines = ['COMPANION_MESH_PSRAM_STORAGE=1', 'ESP32_PLATFORM=1', 'BOARD_HAS_PSRAM=1',
                        'COMPANION_RADIO_FULL=1', 'INDICATOR_TRANSPORT_RENDER_PROFILE=1',
                        'MESH_PRIMARY_ESPNOW=1', 'CONFIG_IDF_TARGET_ESP32S3=1', 'CONFIG_SPIRAM=1']

    def fixture(self):
        header = (ROOT / 'examples/companion_radio/MyMesh.h').read_text()
        source = (ROOT / 'examples/companion_radio/MyMesh.cpp').read_text()
        main = (ROOT / 'examples/companion_radio/main.cpp').read_text()
        start = main.index('#if COMPANION_MESH_PSRAM_STORAGE\nstatic mesh::')
        factory = main[start:main.index('\n#else', start)] + '\n#endif\n'
        start = main.index('  board.begin();\n', main.index('void setup()'))
        setup = main[start:main.index('\n#endif', start)] + '\n#endif\n'
        methods = '\n'.join(block(source, name) for name in (
            'bool MyMesh::Frame::isChannelMsg()', 'int MyMesh::getOfflineQueueCapacity()',
            'MyMesh::Frame& MyMesh::offlineQueueFrameAt(', 'void MyMesh::initializeOfflineQueue()',
            'bool MyMesh::addToOfflineQueue(', 'int MyMesh::peekOfflineQueue(', 'void MyMesh::popOfflineQueue()'))
        constants = '\n'.join(re.search(r'^#define ' + name + r'\s+.*$', source, re.M).group()
                              for name in ('RESP_CODE_CHANNEL_MSG_RECV', 'RESP_CODE_CHANNEL_MSG_RECV_V3', 'RESP_CODE_CHANNEL_DATA_RECV'))
        defaults = header[header.index('#ifndef OFFLINE_QUEUE_SIZE'):header.index('\nstatic_assert(OFFLINE_QUEUE_SIZE')]
        contact = block((ROOT / 'src/helpers/BaseChatMesh.cpp').read_text(), 'bool BaseChatMesh::initializeContactStorage()')
        return (PREAMBLE.replace('@FRAME@', block(header, '  struct Frame {'))
                .replace('@HELD@', block(header, '  struct HeldOneKeyDM {'))
                .replace('@CONSTANTS@', constants).replace('@CONTACT_METHOD@', contact)
                .replace('@METHODS@', methods).replace('@FACTORY@', factory).replace('@SETUP@', setup)
                .replace('#define MESH_DEBUG_PRINTLN(...) ((void)0)',
                         '#define MESH_DEBUG_PRINTLN(...) ((void)0)\n' + defaults))

    def compile_fixture(self):
        source = self.work / 'test.cpp'; source.write_text(self.fixture())
        binary = self.work / 'test'
        result = subprocess.run(self.compiler + ['-std=c++17', '-Os', '-DNDEBUG',
            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-fno-pie', '-no-pie', *['-D' + d for d in self.defines],
            '-I', str(self.work), '-I', str(ROOT / 'src'), str(source), '-o', str(binary)],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return binary

    @staticmethod
    def no_core_dump():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    def test_real_factory_zero_state_full_storage_and_repeat_initialization(self):
        result = subprocess.run([str(self.compile_fixture())], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('512 frames, 15 held DMs, 350 contacts', result.stdout)

    def test_absent_psram_and_allocation_failure_halt_before_partial_startup(self):
        binary = self.compile_fixture()
        for mode, allocations, diagnostic in (
                ('missing', 0, 'requires working PSRAM'),
                ('allocation-failure', 1, 'insufficient PSRAM')):
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 77, result.stdout + result.stderr)
                self.assertIn('halted allocations=%d constructors=0' % allocations, result.stdout)
                self.assertIn(diagnostic, result.stdout)

    def test_access_before_or_during_construction_and_recursive_begin_abort_with_ndebug(self):
        binary = self.compile_fixture()
        for mode in ('preinit', 'partial-access', 'recursive-begin'):
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), mode], capture_output=True, text=True,
                                        preexec_fn=self.no_core_dump, timeout=15)
                self.assertEqual(result.returncode, -signal.SIGABRT, result.stdout + result.stderr)

    def test_default_header_and_global_constructor_have_no_code_or_sdk_change(self):
        source = self.work / 'default.cpp'
        source.write_text('#include <helpers/CompanionPsramMesh.h>\nint main() {}\n')
        for defines in ([], ['-DCOMPANION_MESH_PSRAM_STORAGE=0']):
            result = subprocess.run(self.compiler + ['-std=c++11', '-E', '-P', *defines,
                '-I', str(ROOT / 'src'), str(source)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), 'int main() {}')
        main = (ROOT / 'examples/companion_radio/main.cpp').read_text()
        start = main.index('#if COMPANION_MESH_PSRAM_STORAGE\nstatic mesh::')
        fallback = main[main.index('\n#else\n', start) + len('\n#else\n'):]
        expected = ('MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store\n'
                    '   #ifdef DISPLAY_CLASS\n      , &ui_task\n   #endif\n);')
        self.assertTrue(fallback.startswith(expected + '\n#endif'))
        setup = main[main.index('void setup()'):]
        ordered = ['  board.begin();', 'initializeCompanionMeshStorage()', 'display.begin()', 'radio_init()', 'the_mesh.begin(']
        positions = [setup.index(token) for token in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertLess(setup.index('initializeCompanionMeshStorage()'), setup.index('loadStationProtocolForBoot()'))

    def test_nonqualified_or_unconfigured_storage_combinations_fail_closed(self):
        source = self.work / 'policy.cpp'; source.write_text('#include <helpers/CompanionPsramMesh.h>\n')
        for excluded in ('BOARD_HAS_PSRAM', 'COMPANION_RADIO_FULL', 'INDICATOR_TRANSPORT_RENDER_PROFILE',
                         'MESH_PRIMARY_ESPNOW', 'CONFIG_IDF_TARGET_ESP32S3', 'CONFIG_SPIRAM'):
            with self.subTest(excluded=excluded):
                defines = [d for d in self.defines if not d.startswith(excluded + '=')]
                result = subprocess.run(self.compiler + ['-std=c++11', '-fsyntax-only',
                    *['-D' + d for d in defines], '-I', str(self.work), '-I', str(ROOT / 'src'), str(source)],
                    capture_output=True, text=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)
        result = subprocess.run(self.compiler + ['-std=c++11', '-fsyntax-only',
            *['-D' + d for d in self.defines], '-DOTA_SHARED_COMPANION_QUEUE=1',
            '-I', str(self.work), '-I', str(ROOT / 'src'), str(source)], capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)

    def test_exact_memory_guard_and_indicator_only_recipe_are_unchanged(self):
        spec = importlib.util.spec_from_file_location('indicator_ram_guard', ROOT / 'scripts/check_firmware_ram.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        defines = dict(COMPANION_RADIO_FULL=1, DISPLAY_CLASS='SCIndicatorDisplay', UI_BUFFER_COLOR_DEPTH=4,
                       INDICATOR_TRANSPORT_RENDER_PROFILE=1, BOARD_HAS_PSRAM=1,
                       MESH_PRIMARY_ESPNOW=1,  # Match the ESP-NOW-only Indicator variant.
                       COMPANION_EXCLUSIVE_WIFI_BLE=1, WIFI_SSID='placeholder', BLE_PIN_CODE=123456,
                       ENABLE_OTA=1, WIFI_OTA_SEEDER=1, COMPANION_FEATURE_NETWORK_TERMINAL=1,
                       MAX_CONTACTS=350, MAX_GROUP_CHANNELS=40, UI_MSG_PREVIEW_SIZE=161, OTA_SEEDER_ONLY=1)
        baseline = module.requirements('ESP32_PLATFORM', defines, 'SenseCapIndicator-ESPNow_comp_radio_usb')
        candidate = module.requirements('ESP32_PLATFORM', {**defines, 'COMPANION_MESH_PSRAM_STORAGE': 1},
                                        'SenseCapIndicator-ESPNow_comp_radio_usb')
        self.assertEqual(candidate, baseline)
        self.assertEqual(candidate['required_heap_bytes'], 251168)
        self.assertEqual(candidate['required_contiguous_bytes'], 116224)
        script = (ROOT / 'build_legacy.sh').read_text()
        marker = script.index(' -DCOMPANION_MESH_PSRAM_STORAGE=1')
        case_start = script.rfind('  case "${env_name,,}" in', 0, marker)
        case_end = script.index('\n  esac', marker) + len('\n  esac')
        case = script[case_start:case_end]
        self.assertEqual(script.count('-DCOMPANION_MESH_PSRAM_STORAGE=1'), 1)
        for target, expected in (('SenseCapIndicator-ESPNow_companion_radio_full', True),
                                  ('SenseCapIndicator-LoRa_companion_radio_full', False),
                                  ('SenseCapIndicator-LoRa-N16R2_companion_radio_full', False),
                                  ('Generic_ESPNOW_companion_radio_full', False),
                                  ('RAK_4631_companion_radio_full', False)):
            with self.subTest(target=target):
                program = 'env_name=' + shlex.quote(target) + '\nPLATFORMIO_BUILD_FLAGS=""\n' + case + '\nprintf "%s" "$PLATFORMIO_BUILD_FLAGS"\n'
                result = subprocess.run(['bash', '-c', program], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual('-DCOMPANION_MESH_PSRAM_STORAGE=1' in result.stdout, expected)


if __name__ == '__main__':
    unittest.main()
