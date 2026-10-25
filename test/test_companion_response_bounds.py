"""Check production Companion response envelope limits without radio hardware."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import unittest

from test_replay_reset_integration import extract_braced

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <helpers/CompanionStatusResponse.h>
#include <helpers/CompanionDelayedReplies.h>
#define COMPANION_FEATURE_TEXT_TERMINAL 0
#define MESH_DEBUG_PRINTLN(...) ((void)0)
#define RESP_SERVER_LOGIN_OK 0
@CODES@
@FRAME_LIMIT@
struct ContactInfo { struct { uint8_t pub_key[32] = {1}; } id; };
namespace mesh {
struct Packet {
  uint8_t payload[256] = {}, payload_len = 0, path_len = 0;
  float getSNR() const { return 0; }
  int getRSSI() const { return 0; }
};
}
struct Serial : BaseSerialInterface {
  std::vector<std::vector<uint8_t>> frames;
  bool isConnected() const { return true; }
  void enable() override {}
  void disable() override {}
  bool isEnabled() const override { return true; }
  bool isReadBusy() const override { return false; }
  bool isWriteBusy() const override { return false; }
  size_t checkRecvFrame(uint8_t*) override { return 0; }
  size_t writeFrame(const uint8_t* data, size_t n) override {
    assert(n <= MAX_FRAME_SIZE);
    frames.emplace_back(data, data+n);
    return n;
  }
};
struct Clock { uint32_t getMillis() const { return 100; } };
struct MyMesh {
  mesh::CompanionDelayedReplies _delayed_replies;
  Clock clock;
  Clock* _ms=&clock;
  uint8_t before[16], out_frame[MAX_FRAME_SIZE+1], after[16];
  Serial serial;
  Serial* _serial = &serial;
  MyMesh() {
    memset(before, 0xA5, sizeof(before)); memset(after, 0xA5, sizeof(after));
    memset(out_frame, 0x5A, sizeof(out_frame));
  }
  void arm(mesh::CompanionDelayedReplies::Kind kind, const ContactInfo& contact) {
    assert(_delayed_replies.reserveRequest(kind, contact.id.pub_key, &serial, false, 100));
    assert(kind==mesh::CompanionDelayedReplies::Login || _delayed_replies.allowRequestTag(17));
    _delayed_replies.armRequest(17,1000,false,100);
    servicePendingSerialReply();serial.frames.clear();
  }
  void armTrace() {
    assert(_delayed_replies.reserveBinaryTrace(17,23,&serial,100));
    _delayed_replies.armBinaryTrace(1000,100);
    serviceBinaryTraceReply();serial.frames.clear();
  }
  void clearPendingReqs();
  void startConnection(const ContactInfo&, uint16_t) {}
  size_t writePendingSerialFrame(const uint8_t*, size_t, uint32_t);
  void servicePendingSerialReply();
  void servicePendingSerialReply(uint32_t);
  void serviceBinaryTraceReply();
  void serviceBinaryTraceReply(uint32_t);
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t);
  void onControlDataRecv(mesh::Packet*);
  void onRawDataRecv(mesh::Packet*);
  uint8_t rxViaTag(const mesh::Packet*, uint8_t none) const { return none; }
  void onTraceRecv(mesh::Packet*,uint32_t,uint32_t,uint8_t,const uint8_t*,const uint8_t*,uint8_t);
  void clearBinaryTraceReply();
  void checkGuards() const {
    for (uint8_t value:before) assert(value==0xA5);
    for (uint8_t value:after) assert(value==0xA5);
    assert(out_frame[MAX_FRAME_SIZE]==0x5A);
  }
};
@METHODS@
int main() {
  ContactInfo contact;
  for (unsigned kind=0; kind<3; ++kind) for (unsigned n=0; n<256; ++n) {
    MyMesh value;
    uint32_t tag=17;
    uint8_t data[256]={};memcpy(data,&tag,4);
    const mesh::CompanionDelayedReplies::Kind kinds[]={mesh::CompanionDelayedReplies::Status,
        mesh::CompanionDelayedReplies::Telemetry,mesh::CompanionDelayedReplies::Binary};
    value.arm(kinds[kind],contact);
    value.onContactResponse(contact,data,n);
    const unsigned minimum=kind==0 ? mesh::COMPANION_MIN_STATUS_RESPONSE_SIZE : 5;
    const unsigned overhead=kind==2 ? 2 : 4;
    const bool valid=n>=minimum && n+overhead<=MAX_FRAME_SIZE;
    assert(value.serial.frames.size()==(valid ? 1U : 0U));
    if (valid) assert(value.serial.frames[0].size()==n+overhead);
    assert(value._delayed_replies.hasRequest()==!valid);
    value.checkGuards();
  }
  for (unsigned n=0;n<256;++n) {
    MyMesh idle;uint8_t data[256]={};
    idle.onContactResponse(contact,data,n);
    const bool unsolicited=n>=4 && n+2<=MAX_FRAME_SIZE;
    assert(idle.serial.frames.size()==(unsolicited ? 1U : 0U) && !idle._delayed_replies.hasRequest());
    if (unsolicited) {
      const auto& frame=idle.serial.frames[0];
      assert(frame.size()==n+2 && frame[0]==PUSH_CODE_BINARY_RESPONSE);
      assert(frame[1]==0 && memcmp(frame.data()+2,data,n)==0);
    }
    idle.checkGuards();
    for (bool control:{false,true}) {
      MyMesh value;mesh::Packet packet;packet.payload_len=n;
      if (control) value.onControlDataRecv(&packet);else value.onRawDataRecv(&packet);
      assert(value.serial.frames.size()==(n+4<=MAX_FRAME_SIZE ? 1U : 0U));
      value.checkGuards();
    }
  }
  for (bool modern:{false,true}) {
    MyMesh value;value.arm(mesh::CompanionDelayedReplies::Login,contact);
    uint8_t data[13]={};if (!modern) memcpy(data+4,"OK",2);
    value.onContactResponse(contact,data,modern ? 13 : 6);
    assert(value.serial.frames.size()==1 && !value._delayed_replies.hasRequest());value.checkGuards();
  }
  for (unsigned n=0;n<256;++n) for (uint8_t flags=0;flags<4;++flags) {
    MyMesh value;value.armTrace();mesh::Packet packet;uint8_t data[256]={};
    value.onTraceRecv(&packet,17,23,flags,data,data,n);
    const unsigned framed=13+n+(n>>flags);
    assert(value.serial.frames.size()==(framed<=MAX_FRAME_SIZE ? 1U : 0U));
    if (!value.serial.frames.empty()) assert(value.serial.frames[0].size()==framed);
    assert(value._delayed_replies.hasBinaryTrace()==(framed>MAX_FRAME_SIZE));value.checkGuards();
  }
  MyMesh empty;empty.onContactResponse(contact,nullptr,255);empty.checkGuards();
}
'''


class CompanionResponseBoundsTest(unittest.TestCase):
    def test_response_envelopes(self):
        source = (ROOT / 'examples/companion_radio/MyMesh.cpp').read_text(encoding='utf-8')
        codes = '\n'.join(re.findall(r'^#define PUSH_CODE_\w+\s+0x[0-9A-Fa-f]+', source, re.M))
        interface = (ROOT / 'src/helpers/CompanionFrameLimits.h').read_text(encoding='utf-8')
        frame_limit = re.search(r'^#define MAX_FRAME_SIZE\s+\d+', interface, re.M).group(0)
        methods = '\n'.join(extract_braced(source, signature) for signature in (
            'void MyMesh::onContactResponse(', 'void MyMesh::onControlDataRecv(',
            'void MyMesh::onRawDataRecv(', 'void MyMesh::onTraceRecv(',
            'size_t MyMesh::writePendingSerialFrame(', 'void MyMesh::clearPendingReqs(',
            'void MyMesh::servicePendingSerialReply()', 'void MyMesh::servicePendingSerialReply(uint32_t now)',
            'void MyMesh::serviceBinaryTraceReply()', 'void MyMesh::serviceBinaryTraceReply(uint32_t now)',
            'void MyMesh::clearBinaryTraceReply(',
        ))
        with tempfile.TemporaryDirectory(prefix='companion-response-') as directory:
            work = Path(directory)
            cpp = work / 'test.cpp'
            cpp.write_text(HARNESS.replace('@CODES@',codes).replace('@METHODS@',methods)
                           .replace('@FRAME_LIMIT@',frame_limit), encoding='utf-8')
            flags = ['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie'] if sys.platform.startswith('linux') else []
            binary = work / 'test.exe'
            compiled = subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-Wall','-Wextra','-Werror',
                *flags,'-isystem',str(ROOT / "test/mocks"),f'-I{ROOT / "src"}',str(cpp),
                str(ROOT / 'src/helpers/CompanionDelayedReplies.cpp'),
                '-o',str(binary)], capture_output=True,text=True,timeout=60)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            checked = subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
            self.assertEqual(checked.returncode,0,checked.stderr)


if __name__=='__main__':
    unittest.main()
