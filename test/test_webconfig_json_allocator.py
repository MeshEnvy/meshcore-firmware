#!/usr/bin/env python3
"""Exercise real JSON workspaces and authenticated handlers under allocation failure."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

import test_webconfig_lost_reply as config_fixture
import test_webconfig_login_throttle as login_fixture
from test_replay_reset_integration import extract_braced

ROOT = Path(__file__).resolve().parents[1]
SERVER = (ROOT / 'src/helpers/esp32/WebConfigServer.cpp').read_text()
HEADER = (ROOT / 'src/helpers/esp32/WebConfigServer.h').read_text()
ALIASES = HEADER.split('class WebConfigServer {', 1)[1].split('public:', 1)[0]
HEAP = r'''
#pragma once
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <map>
enum { MALLOC_CAP_INTERNAL=1, MALLOC_CAP_DMA=2, MALLOC_CAP_SPIRAM=4, MALLOC_CAP_8BIT=8 };
namespace heap_test {
struct Block { size_t size; bool psram; };
inline std::map<void*,Block> blocks;
inline size_t dma_free=65536,dma_largest=32768;
inline bool psram=true;
inline int successful_before_failure=-1;
inline unsigned psram_calls=0,internal_calls=0;
inline void reset() {
  assert(blocks.empty()); dma_free=65536;dma_largest=32768;
  psram=true;successful_before_failure=-1;psram_calls=internal_calls=0;
}
}
inline void* heap_caps_malloc(size_t size,unsigned caps) {
  const bool external=caps&MALLOC_CAP_SPIRAM;
  if(external)++heap_test::psram_calls;else ++heap_test::internal_calls;
  if((external&&!heap_test::psram)||heap_test::successful_before_failure==0)return nullptr;
  if(!external&&(size>heap_test::dma_largest||size>heap_test::dma_free))return nullptr;
  void* pointer=malloc(size);if(!pointer)return nullptr;
  if(heap_test::successful_before_failure>0)--heap_test::successful_before_failure;
  heap_test::blocks[pointer]={size,external};
  if(!external)heap_test::dma_free-=size;
  return pointer;
}
inline void heap_caps_free(void* pointer) {
  if(!pointer)return;
  const auto found=heap_test::blocks.find(pointer);assert(found!=heap_test::blocks.end());
  if(!found->second.psram)heap_test::dma_free+=found->second.size;
  heap_test::blocks.erase(found);free(pointer);
}
inline size_t heap_caps_get_free_size(unsigned) { return heap_test::dma_free; }
inline size_t heap_caps_get_largest_free_block(unsigned) { return heap_test::dma_largest; }
inline size_t heap_caps_get_allocated_size(void* pointer) { return heap_test::blocks.at(pointer).size; }
'''
PRELUDE = '#define ESP_PLATFORM 1\n#define BOARD_HAS_PSRAM 1\n#include <helpers/esp32/WebConfigJsonAllocator.h>\n'


class WebConfigJsonAllocatorTests(unittest.TestCase):
    def compile_run(self, source, *, psram=True):
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        with tempfile.TemporaryDirectory(prefix='webconfig-json-') as folder:
            work = Path(folder)
            (work / 'esp_heap_caps.h').write_text(HEAP)
            cpp, binary = work / 'test.cpp', work / 'test'
            cpp.write_text((PRELUDE if psram else '#include <helpers/esp32/WebConfigJsonAllocator.h>\n') + source)
            sanitizers = [] if os.name == 'nt' else [
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie']
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra',
                *sanitizers,
                '-I'+str(work), '-I'+str(ROOT/'src'), '-I'+str(json_header.parent),
                str(cpp), '-o', str(binary)], capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stdout+run.stderr)

    def test_psram_allocations_realloc_failure_and_shrink_preserve_dma_reserve(self):
        self.compile_run(r'''
int main() {
  using namespace heap_test;
  auto* allocator=mesh::webconfig::JsonAllocator::instance();
  reset();
  auto* first=static_cast<char*>(allocator->allocate(4096));assert(first&&internal_calls==0);
  memset(first,'x',4096);const auto free_before=dma_free;
  auto* larger=static_cast<char*>(allocator->reallocate(first,8192));
  assert(larger&&dma_free==free_before&&internal_calls==0);
  for(size_t i=0;i<4096;++i)assert(larger[i]=='x');
  psram=false;dma_free=dma_largest=0;
  const auto calls=psram_calls;
  assert(allocator->reallocate(larger,32)==larger&&psram_calls==calls);
  assert(!allocator->reallocate(larger,16384));
  assert(blocks.count(larger)&&larger[4095]=='x');
  assert(!allocator->reallocate(larger,0));assert(blocks.empty());
  reset();psram=false;dma_free=16384+4096;dma_largest=4096;
  first=static_cast<char*>(allocator->allocate(4096));assert(first&&dma_free==16384);
  assert(!allocator->allocate(1));allocator->deallocate(first);
  reset();psram=false;dma_free=16384+4096-1;dma_largest=4096;
  assert(!allocator->allocate(4096)&&internal_calls==0);
  dma_free=65536;dma_largest=4095;
  assert(!allocator->allocate(4096)&&internal_calls==0);
  reset();
  { mesh::webconfig::DynamicJsonDocument large(6656);
    mesh::webconfig::StaticJsonDocument<96> small;
    assert(large.capacity()==6656&&small.capacity()==96);
    small["state"]="pending";
    for(unsigned i=0;i<200;++i)large.add(i);
    assert(!small.overflowed()&&!large.overflowed()&&internal_calls==0);
  }
  assert(blocks.empty());
}
''')

    def config(self):
        source = config_fixture.LostReplyWebConfigTest.fixture(self).split('int main() {', 1)[0]
        source = source.replace('struct WebConfigServer {', 'struct WebConfigServer {'+ALIASES)
        start = source.index('void WebConfigServer::handleConfigGet(')
        end = source.index('\nvoid WebConfigServer::handleConfigPost(', start)
        source = source[:start] + extract_braced(SERVER, 'void WebConfigServer::handleConfigGet(') + source[end:]
        source = source.replace('struct AsyncWebServerRequest {',
            'struct AsyncResponseStream : String { void addHeader(const char*,const char*) {} };\n'
            'static void webConfigRegulatorsSnapshot(JsonObject) {}\nstruct AsyncWebServerRequest {')
        source = source.replace('  Client socket;', '  AsyncResponseStream response;\n  Client socket;')
        source = source.replace('  void send(int code,',
            '  AsyncResponseStream* beginResponseStream(const char*) { return &response; }\n'
            '  void send(AsyncResponseStream* result) { status=200;output=*result; }\n  void send(int code,')
        source = source.replace('  Callbacks* _cb = &callbacks;',
            '  Callbacks* _cb = &callbacks;\n  char _wifi_password[65]="saved-secret";\n'
            '  unsigned _wifi_power_save=0;\n')
        return source

    def test_actual_config_handlers_use_psram_and_reject_no_memory_before_queue_admission(self):
        self.compile_run(self.config()+r'''
int main() {
  using namespace heap_test;
  assert(mesh::esp32::managementPassword::publish("password"));
  reset();
  { WebConfigServer server;AsyncWebServerRequest get;server.handleConfigGet(&get);
    assert(get.status==200&&psram_calls&&internal_calls==0);
    assert(get.output.find("saved-secret")==String::npos);
    JsonDocument response;assert(!deserializeJson(response,get.output));
    assert(response["radio"]["freq"].is<double>());
  }
  reset();psram=false;dma_free=16384;
  { WebConfigServer server;AsyncWebServerRequest get;server.handleConfigGet(&get);
    assert(get.status==503&&get.output=="{\"error\":\"not enough memory\"}");
    assert(!internal_calls&&server._batch_state==WebConfigBatch::State::Idle);
  }
  // Fail every successive allocation, including construction of the accepted
  // receipt. A failed JSON workspace cannot publish a partly parsed batch.
  bool succeeded=false;
  for(int permitted=0;permitted<30&&!succeeded;++permitted) {
    reset();dma_free=16384;successful_before_failure=permitted;
    { WebConfigServer server;server.callbacks.node.capabilities=WebConfigServer::CAP_LOST_REPLY;
      AsyncWebServerRequest post(writeBody("\"yes\""));server.handleConfigPost(&post);
      if(post.status==202) {
        succeeded=true;assert(server._batch_state==WebConfigBatch::State::Pending);
        assert(server._batch_count==1&&!strcmp(server._batch[0].cmd,"set lost.reply yes"));
      } else {
        assert(post.status==503&&server._batch_state==WebConfigBatch::State::Idle);
        assert(server._batch_count==0);
      }
      assert(!internal_calls);
    }
  }
  assert(succeeded);reset();
  { WebConfigServer server;AsyncWebServerRequest post(writeBody("\"yes\""));
    post.header.cookies="";psram=false;dma_free=0;server.handleConfigPost(&post);
    assert(post.status==401&&psram_calls==0&&internal_calls==0);
  }
}
''')

    def test_actual_login_no_memory_does_not_count_as_wrong_password(self):
        source = login_fixture.WebConfigLoginThrottleTest.handler_fixture(self).split('static const char* kLocked', 1)[0]
        source = source.replace('struct WebConfigServer {', 'struct WebConfigServer {'+ALIASES)
        self.assertIn('DeserializationError::NoMemory', source)
        self.compile_run(source+r'''
int main() {
  using namespace heap_test;
  reset();WebConfigServer portal;
  for(unsigned i=0;i<12;++i) {
    AsyncWebServerRequest request;request.body="{\"password\":\"existing-secret\"}";
    request._tempObject=request.body.data();psram=false;dma_free=16384;
    portal.handleLogin(&request);assert(request.status==503&&internal_calls==0);
  }
  psram=true;
  AsyncWebServerRequest request;request.body="{\"password\":\"existing-secret\"}";
  request._tempObject=request.body.data();portal.handleLogin(&request);
  assert(request.status==200&&portal._session_token[0]);
}
''')

    def test_non_psram_keeps_original_arduinojson_document_types(self):
        self.compile_run(r'''
#include <cassert>
#include <type_traits>
static_assert(std::is_same<mesh::webconfig::DynamicJsonDocument,ArduinoJson::DynamicJsonDocument>::value,"");
static_assert(std::is_same<mesh::webconfig::StaticJsonDocument<96>,ArduinoJson::StaticJsonDocument<96>>::value,"");
int main() {
  mesh::webconfig::DynamicJsonDocument doc(6656);doc["key"]="value";
  auto* allocator=mesh::webconfig::JsonAllocator::instance();
  void* memory=allocator->allocate(16);assert(memory);
  memory=allocator->reallocate(memory,32);assert(memory);allocator->deallocate(memory);
  return doc.overflowed();
}
''', psram=False)


if __name__ == '__main__':
    unittest.main()
