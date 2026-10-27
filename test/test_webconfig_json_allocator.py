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
            'static void webConfigRegulatorsSnapshot(JsonObject) {}\nstruct AsyncWebServerRequest {')
        source = source.replace('  Callbacks* _cb = &callbacks;',
            '  Callbacks* _cb = &callbacks;\n  char _wifi_password[65]="saved-secret";\n'
            '  unsigned _wifi_power_save=0;\n')
        return source

    def response_failures(self):
        source = self.config()
        stream = extract_braced(source, 'struct AsyncResponseStream') + ';'
        source = source.replace(stream, r'''
struct AsyncResponseStream : String {
  static inline size_t limit=SIZE_MAX;
  static inline bool cookie_ok=true;
  static inline unsigned destroyed=0;
  int code=200;
  ~AsyncResponseStream() { ++destroyed; }
  size_t write(const uint8_t* data,size_t count) {
    const size_t room=limit>size()?limit-size():0;
    const size_t accepted=count<room?count:room;
    append(reinterpret_cast<const char*>(data),accepted);return accepted;
  }
  size_t write(uint8_t byte) { return write(&byte,1); }
  size_t available() const { return size(); }
  bool addHeader(const char*,const char*) { return cookie_ok; }
  void setCode(int value) { code=value; }
};''')
        source = source.replace('uint32_t millis() { return 1; }',
            'uint32_t fixture_time=1;\nuint32_t millis() { return fixture_time; }')
        source = source.replace('  int indexOf(', '  int toInt() const { return atoi(c_str()); }\n  int indexOf(')
        source = source.replace('void printf(const char*, Args...) {}',
            'void printf(const char*, Args...) {} void println(const char*) {}')
        source = source.replace('struct AsyncWebServerRequest {',
            'struct Parameter { String text="0123456789abcdef"; const String& value() const { return text; } };\n'
            'struct AsyncWebServerRequest {\n  Parameter parameter;\n'
            '  bool hasParam(const char* key) const { return !strcmp(key,"reqid"); }\n'
            '  Parameter* getParam(const char*) { return &parameter; }')
        source = source.replace('  struct Callbacks {',
            '  struct Callbacks {\n  bool supportsCliTerminal() const { return true; }')
        source = source.replace('  void handleConfigGet(',
            '  bool _cli_enabled=true, _admin_pwd_set=true;\n'
            '  uint32_t _reboot_at=0, _ap_rekey_after=0, _ap_rekey_generation=0;\n'
            '  bool resultReceiptAllowedLocked(AsyncWebServerRequest*,const char*,int,uint32_t) { return false; }\n'
            '  void handleConfigResult(AsyncWebServerRequest*);\n'
            '  void handleCliPost(AsyncWebServerRequest*);\n'
            '  void handleCliResult(AsyncWebServerRequest*);\n'
            '  void handleConfigGet(')
        source += '\n#include <helpers/CLICommandUtils.h>\n' + '\n'.join(
            extract_braced(SERVER, signature) for signature in (
                'static inline bool wcCliEchoesSecret(', 'static inline bool wcIsDeferredReboot(',
                'static const char* wcCliUnavailable(', 'void WebConfigServer::handleCliPost(',
                'void WebConfigServer::handleConfigResult(', 'void WebConfigServer::handleCliResult('))
        return source

    def test_actual_posts_reject_partial_receipts_before_batch_admission(self):
        self.compile_run(self.response_failures()+r'''
int main() {
  assert(mesh::esp32::managementPassword::publish("password"));
  for(bool cli:{false,true}) {
    for(size_t limit:{size_t(0),size_t(7),SIZE_MAX}) {
      WebConfigServer server;server.callbacks.node.capabilities=WebConfigServer::CAP_LOST_REPLY;
      AsyncResponseStream::limit=limit;
      const unsigned freed=AsyncResponseStream::destroyed;
      AsyncWebServerRequest post(cli
          ? "{\"reqid\":\"0123456789abcdef\",\"cmds\":[\"get name\"]}"
          : writeBody("\"yes\""));
      if(cli)server.handleCliPost(&post);else server.handleConfigPost(&post);
      assert(AsyncResponseStream::destroyed==freed+1);
      if(limit==SIZE_MAX) {
        assert(post.status==202&&server._batch_state==WebConfigBatch::State::Pending);
        assert(server._batch_count==1);
        JsonDocument ack;assert(!deserializeJson(ack,post.output));
        assert(!strcmp(ack["reqid"],"0123456789abcdef"));
      } else {
        assert(post.status==503&&server._batch_state==WebConfigBatch::State::Idle);
        assert(server._batch_count==0&&!server._batch_reqid[0]);
        assert(!server._batch_auth_generation&&!server._batch_reboot_armed);
      }
    }
  }
}
''')

    def test_actual_results_do_not_arm_reboot_or_rekey_until_complete_body(self):
        self.compile_run(self.response_failures()+r'''
int main() {
  assert(mesh::esp32::managementPassword::publish("password"));
  for(bool cli:{false,true})for(size_t limit:{size_t(0),size_t(7)}) {
    fixture_time=1;
    WebConfigServer server;server._batch_kind=cli?WebConfigServer::BATCH_CLI:WebConfigServer::BATCH_CONFIG;
    server._batch_state=WebConfigBatch::State::Done;server._batch_count=server._batch_next=1;
    strcpy(server._batch_reqid,"0123456789abcdef");
    strcpy(server._batch[0].key,"name");strcpy(server._batch[0].reply,"OK");
    server._batch_reboot=server._batch_reauth_required=true;server._ap_rekey_generation=7;
    AsyncResponseStream::limit=limit;
    const unsigned freed=AsyncResponseStream::destroyed;
    AsyncWebServerRequest failed;
    if(cli)server.handleCliResult(&failed);else server.handleConfigResult(&failed);
    assert(failed.status==503&&AsyncResponseStream::destroyed==freed+1);
    assert(!server._batch_reauth_delivered&&!server._batch_reboot_armed);
    assert(!server._ap_rekey_after&&!server._reboot_at);
    assert(server._batch_state==WebConfigBatch::State::Done);
    AsyncResponseStream::limit=SIZE_MAX;
    AsyncWebServerRequest retry;
    if(cli)server.handleCliResult(&retry);else server.handleConfigResult(&retry);
    assert(retry.status==200&&server._batch_reauth_delivered&&server._batch_reboot_armed);
    assert(server._ap_rekey_after==WebConfigBatch::scheduleAt(1,3000));
    assert(server._reboot_at==WebConfigBatch::confirmRebootAt(1));
    JsonDocument result;assert(!deserializeJson(result,retry.output));
    assert(!strcmp(result["state"],"done")&&result["results"].size()==1);
    const auto reboot=server._reboot_at,rekey=server._ap_rekey_after;
    fixture_time=500;
    AsyncWebServerRequest reread;
    if(cli)server.handleCliResult(&reread);else server.handleConfigResult(&reread);
    assert(reread.status==200&&server._reboot_at==reboot&&server._ap_rekey_after==rekey);
  }
}
''')

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

    def test_actual_login_keeps_session_when_body_or_cookie_allocation_fails(self):
        source = login_fixture.WebConfigLoginThrottleTest.handler_fixture(self).split('static const char* kLocked', 1)[0]
        source = source.replace('struct WebConfigServer {', 'struct WebConfigServer {'+ALIASES)
        stream = extract_braced(source, 'struct AsyncResponseStream') + ';'
        source = source.replace(stream, r'''
struct AsyncResponseStream : AsyncWebServerResponse {
  static inline size_t limit=SIZE_MAX;
  static inline unsigned cookie_failure=0;
  static inline unsigned destroyed=0;
  std::string body;
  ~AsyncResponseStream() { ++destroyed; }
  size_t available() const { return body.size(); }
  size_t write(const uint8_t* bytes,size_t count) {
    const size_t accepted=count<limit?count:limit;
    body.append(reinterpret_cast<const char*>(bytes),accepted);return accepted;
  }
  bool addHeader(const char* key,const char* value) {
    if(cookie_failure==1)return true; // library can report success after String OOM
    return AsyncWebServerResponse::addHeader(key,cookie_failure==2?"wcs=partial":value);
  }
};''')
        self.compile_run(source+r'''
int main() {
  WebConfigServer portal;
  strcpy(portal._session_token,"0123456789abcdef0123456789abcdef");
  portal._session_last_seen=123;portal._session_generation=456;
  for(int failure=0;failure<4;++failure) {
    AsyncResponseStream::limit=failure==0?0:failure==1?7:SIZE_MAX;
    AsyncResponseStream::cookie_failure=failure<2?0:failure-1;
    const unsigned freed=AsyncResponseStream::destroyed;
    AsyncWebServerRequest request;request.body="{\"password\":\"existing-secret\"}";
    request._tempObject=request.body.data();portal.handleLogin(&request);
    assert(request.status==503&&AsyncResponseStream::destroyed==freed+1);
    assert(!strcmp(portal._session_token,"0123456789abcdef0123456789abcdef"));
    assert(portal._session_last_seen==123&&portal._session_generation==456);
  }
  AsyncResponseStream::limit=SIZE_MAX;AsyncResponseStream::cookie_failure=0;
  AsyncWebServerRequest retry;retry.body="{\"password\":\"existing-secret\"}";
  retry._tempObject=retry.body.data();portal.handleLogin(&retry);
  assert(retry.status==200&&retry.reply=="{\"ok\":true}");
  assert(strcmp(portal._session_token,"0123456789abcdef0123456789abcdef"));
  assert(portal._session_last_seen==now_ms);
  assert(retry.response.headers.at("Set-Cookie").find(portal._session_token)!=std::string::npos);
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
