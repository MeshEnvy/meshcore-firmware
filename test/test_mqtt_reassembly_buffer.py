#!/usr/bin/env python3
"""Run real MQTT allocation/lifetime and multipart delivery with heap failures."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import test_mqtt_publish_ack as ack

ROOT = Path(__file__).resolve().parents[1]

MEMORY = r'''
#include <map>
#include <string>
#include "helpers/MQTTConnectionAdmission.h"
enum { MALLOC_CAP_INTERNAL=1, MALLOC_CAP_DMA=2, MALLOC_CAP_SPIRAM=4, MALLOC_CAP_8BIT=8 };
struct Allocation { size_t bytes; unsigned caps; };
static std::map<void*,Allocation> allocations;
static std::vector<Allocation> heap_requests;
static bool psram_ok=true, internal_ok=true, ordinary_ok=true;
static size_t dma_free=65536, dma_largest=16384;
static unsigned ordinary_allocs=0, ordinary_frees=0, caps_frees=0;
static int required_destroy_count=0;
static void* ordinary_malloc(size_t bytes) {
  ++ordinary_allocs;
  if(!ordinary_ok)return nullptr;
  void* p=std::malloc(bytes);assert(p);allocations[p]={bytes,0};return p;
}
static void ordinary_free(void* p) {
  assert(destroy_calls>=required_destroy_count);
  assert(allocations.count(p)&&allocations[p].caps==0);
  allocations.erase(p);++ordinary_frees;std::free(p);
}
void* heap_caps_malloc(size_t bytes,unsigned caps) {
  heap_requests.push_back({bytes,caps});
  if((caps&MALLOC_CAP_SPIRAM)?!psram_ok:!internal_ok)return nullptr;
  if(caps&MALLOC_CAP_INTERNAL){assert(bytes<=dma_free&&bytes<=dma_largest);dma_free-=bytes;}
  void* p=std::malloc(bytes);assert(p);allocations[p]={bytes,caps};return p;
}
void heap_caps_free(void* p) {
  assert(destroy_calls>=required_destroy_count);
  assert(allocations.count(p)&&allocations[p].caps);
  if(allocations[p].caps&MALLOC_CAP_INTERNAL)dma_free+=allocations[p].bytes;
  allocations.erase(p);++caps_frees;std::free(p);
}
size_t heap_caps_get_free_size(unsigned caps) {
  assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));return dma_free;
}
size_t heap_caps_get_largest_free_block(unsigned caps) {
  assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));return dma_largest;
}
#define malloc ordinary_malloc
#define free ordinary_free
'''

MAIN = r'''
static void configure(PsychicMqttClient& client,int size=896) {
  client._mqtt_cfg.uri="mqtt://test";
  client._mqtt_cfg.broker.address.uri="mqtt://test";
  client._mqtt_cfg.buffer_size=size;
  client._mqtt_cfg.buffer.size=size;
}
static size_t allocated(){return ordinary_allocs+heap_requests.size();}
static void multipart(PsychicMqttClient& client,const std::string& text) {
  char topic[]="test/data";
  const size_t first=text.size()/3,second=2*text.size()/3;
  for(size_t part=0;part<3;++part) {
    size_t offset=part==0?0:part==1?first:second;
    size_t end=part==0?first:part==1?second:text.size();
    esp_mqtt_event_t event{MQTT_EVENT_DATA,0,false};
    event.total_data_len=text.size();event.current_data_offset=offset;
    event.data_len=end-offset;event.data=const_cast<char*>(text.data()+offset);
    event.topic=topic;event.topic_len=sizeof(topic)-1;
    esp_mqtt_event_handle_t pointer=&event;
    client._onMessage(pointer);
  }
}
int main() {
  {
    PsychicMqttClient client;configure(client);
    // A successful PSRAM allocation needs no internal-heap headroom.
    dma_free=0;dma_largest=0;
    assert(client.connect()==ESP_OK&&client._buffer_capacity==896);
    assert(allocations[client._buffer].bytes==897);
#if defined(BOARD_HAS_PSRAM)
    assert(allocations[client._buffer].caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    assert(ordinary_allocs==0&&heap_requests.size()==1);
#else
    assert(allocations[client._buffer].caps==0&&heap_requests.empty());
#endif
    char* retained=client._buffer;
    size_t count=allocated();
    unsigned received=0;
    std::string expected(896,'Q');
    client.onMessage([&](char* topic,char* payload,int,int,bool){
      assert(!strcmp(topic,"test/data")&&expected==payload);++received;
    });
    multipart(client,expected);assert(received==1&&allocated()==count);
    multipart(client,std::string(897,'X'));assert(received==1&&allocated()==count);
    client.disconnect();
    assert(client._buffer==retained&&allocations.count(retained)&&allocated()==count);
    assert(client.connect()==ESP_OK&&client._buffer==retained&&allocated()==count);
    expected="after restart";multipart(client,expected);assert(received==2);
    // Existing clients retain their original fixed reassembly allocation.
    configure(client,3072);
    assert(client.connect()==ESP_OK&&client._buffer==retained&&client._buffer_capacity==896);
    assert(allocated()==count);
    required_destroy_count=destroy_calls+1; // SDK ownership ends before buffer release.
  }
  assert(allocations.empty()&&destroy_calls==1);
#if defined(BOARD_HAS_PSRAM)
  assert(caps_frees==1&&ordinary_frees==0);
  psram_ok=false;
  // Exact fallback boundary preserves the full DMA reserve after allocation.
  dma_free=MQTTConnectionAdmission::kDmaReserveBytes+897;dma_largest=897;
  {
    PsychicMqttClient client;configure(client);
    assert(client.connect()==ESP_OK&&client._buffer_capacity==896);
    assert(allocations[client._buffer].caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT));
    assert(dma_free==MQTTConnectionAdmission::kDmaReserveBytes);
  }
  assert(allocations.empty()&&caps_frees==2);
  for(unsigned scenario=0;scenario<3;++scenario) {
    dma_free=MQTTConnectionAdmission::kDmaReserveBytes+(scenario==0?896:897);
    dma_largest=scenario==1?896:897;
    internal_ok=scenario!=2;
    PsychicMqttClient client;configure(client);
    const int before=init_calls;
    const size_t requests=heap_requests.size();
    assert(client.connect()==ESP_ERR_NO_MEM);
    assert(client._buffer==nullptr&&client._buffer_capacity==0&&!client.isInitialized());
    assert(init_calls==before&&allocations.empty());
    assert(heap_requests.size()==requests+(scenario==2?2:1));
    psram_ok=true;
    assert(client.connect()==ESP_OK&&client._buffer_capacity==896);
    psram_ok=false;
  }
  internal_ok=true;psram_ok=true;
#else
  assert(ordinary_frees==1&&caps_frees==0);
  {
    ordinary_ok=false;
    PsychicMqttClient client;configure(client);
    const int before=init_calls;
    assert(client.connect()==ESP_ERR_NO_MEM&&!client._buffer&&!client._buffer_capacity);
    assert(init_calls==before&&heap_requests.empty());
    ordinary_ok=true;
    assert(client.connect()==ESP_OK&&client._buffer_capacity==896);
  }
#endif
  assert(allocations.empty());
  // Default and explicitly larger desired capacities still include one NUL.
  for(int desired:{0,3072}) {
    PsychicMqttClient client;configure(client,desired);
    const size_t capacity=desired?desired:1024;
    assert(client.connect()==ESP_OK&&client._buffer_capacity==capacity);
    assert(allocations[client._buffer].bytes==capacity+1);
  }
  assert(allocations.empty());
}
'''


class MqttReassemblyBufferTest(unittest.TestCase):
    def test_real_allocator_lifetime_recovery_and_multipart_capacity(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        event = """struct esp_mqtt_event_t { int event_id, msg_id; bool session_present;
          int total_data_len=0,data_len=0,current_data_offset=0,topic_len=0,retain=0,qos=0;
          char* data=nullptr;char* topic=nullptr;bool dup=false; };"""
        mqtt = ack.MQTT_HEADER.replace(
            "struct esp_mqtt_event_t { int event_id, msg_id; bool session_present; };", event)
        preamble = ack.PREAMBLE.replace(
            "void PsychicMqttClient::_onMessage(esp_mqtt_event_handle_t&) { ++receive_events; }", "")
        source = preamble + MEMORY + "\n".join(ack.method(ack.ADAPTER, signature) for signature in (
            "PsychicMqttClient::PsychicMqttClient()", "PsychicMqttClient::~PsychicMqttClient()",
            "PsychicMqttClient &PsychicMqttClient::onMessage(", "int PsychicMqttClient::subscribe(",
            "esp_err_t PsychicMqttClient::applyConfig()", "esp_err_t PsychicMqttClient::connect()",
            "void PsychicMqttClient::disconnect()", "void PsychicMqttClient::_onMqttEventStatic(",
            "void PsychicMqttClient::_onMqttEvent(", "void PsychicMqttClient::_onConnect(",
            "void PsychicMqttClient::_onDisconnect(", "void PsychicMqttClient::_onSubscribe(",
            "void PsychicMqttClient::_onUnsubscribe(", "void PsychicMqttClient::_onPublish(",
            "size_t PsychicMqttClient::refreshOutboxSize()",
            "void PsychicMqttClient::_onMessage(", "bool PsychicMqttClient::_isTopicMatch(",
        )) + MAIN
        with tempfile.TemporaryDirectory(prefix="meshcore-mqtt-buffer-") as directory:
            directory = Path(directory)
            (directory / "Arduino.h").write_text(ack.ARDUINO_HEADER, encoding="ascii")
            (directory / "mqtt_client.h").write_text(mqtt, encoding="ascii")
            (directory / "esp_crt_bundle.h").write_text("#pragma once\n", encoding="ascii")
            path = directory / "buffer.cpp"
            path.write_text(source, encoding="ascii")
            for idf in (4, 5):
                for psram in (False, True):
                    with self.subTest(idf=idf, psram=psram):
                        binary = directory / "buffer"
                        sanitize = (["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
                                    if sys.platform.startswith("linux") else [])
                        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                   "-Wno-unused-function", "-Wno-unused-variable", *sanitize,
                                   "-DARDUINO_ARCH_ESP32=1", f"-DESP_IDF_VERSION_MAJOR={idf}",
                                   *(["-DBOARD_HAS_PSRAM=1"] if psram else []),
                                   "-I", str(directory), "-I", str(ROOT / "lib/PsychicMqttClient/src"),
                                   "-I", str(ROOT / "src"), str(path), "-o", str(binary)]
                        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
