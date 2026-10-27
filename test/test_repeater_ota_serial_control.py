"""Real serial source framing keeps repeater CLI outside binary responses."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include <helpers/ota/MotaSourceSerial.h>
#include <helpers/ota/MotaSeederProto.h>
using namespace mesh::ota;
namespace mesh {
bool kiss_owned=false;
bool isUsbBinaryStreamOwned(){return kiss_owned;}
}
struct Wire:Stream {
  std::deque<uint8_t> input;
  std::vector<uint8_t> next_response;
  int writes=0,reads=0;
  int available()override{return input.size();}
  int read()override{++reads;if(input.empty())return -1;auto b=input.front();input.pop_front();return b;}
  size_t write(const uint8_t* data,size_t n)override{
    assert(n>=4&&data[0]=='M'&&data[1]=='S');++writes;
    input.insert(input.end(),next_response.begin(),next_response.end());next_response.clear();return n;
  }
  void push(const std::string& s){input.insert(input.end(),s.begin(),s.end());}
};
static std::vector<uint8_t> frame(const std::vector<uint8_t>& payload){
  std::vector<uint8_t> result={'m','s',3,0};
  result.insert(result.end(),payload.begin(),payload.end());
  uint8_t sum=0;for(auto b:result)sum^=b;result.push_back(sum);return result;
}
static std::string controls(SerialMotaSource& source){
  std::string output;
  for(unsigned guard=0;source.availableControlBytes()&&guard<2048;++guard){
    int value=source.readControlByte();if(value==-1)break;if(value>=0)output+=char(value);
  }
  return output;
}
static bool kissClaimAllowed(SerialMotaSource& control_source){
  @KISS_GATE@
  return kiss_can_read();
}
int main(){
  const std::string command="ota.peer cmd "+std::string(64,'1')+" ota key add "+std::string(64,'A')+"\r";
  const std::string dangerous="\rota.peer cmd "+std::string(64,'2')+" reboot\r\nota folder off\r";
  std::vector<uint8_t> payload={0xc0,0,0x41,0xc0};
  payload.insert(payload.end(),dangerous.begin(),dangerous.end());
  const auto response=frame(payload);
  for(size_t prefix=0;prefix<response.size();++prefix){
    g_mock_millis=0;Wire wire;
    SerialMotaSource source(wire,MotaStreamWritePolicy::NoFlush,5);
    source.enableSharedTextControl();
    assert(kissClaimAllowed(source));
    wire.next_response.assign(response.begin(),response.begin()+prefix);
    std::vector<uint8_t> target(payload.size());
    assert(!source.read(0,0,target.data(),target.size()));
    assert(source.hasPendingResponse());
    assert(!kissClaimAllowed(source));
    for(size_t i=prefix;i<response.size();++i){
      wire.input.push_back(response[i]);
      assert(controls(source).empty());
      if(i+1<response.size())assert(!kissClaimAllowed(source));
    }
    assert(!source.hasPendingResponse());
    assert(kissClaimAllowed(source));
    wire.push(command);
    assert(controls(source)==command);
  }
  // A complete long CLI request that arrives just before a source transaction
  // is retained intact while the binary count/read exchange finishes.
  g_mock_millis=0;Wire wire;
  SerialMotaSource source(wire,MotaStreamWritePolicy::NoFlush,5);
  source.enableSharedTextControl();
  wire.push(command);
  wire.next_response=frame({42});
  uint8_t value=0;assert(source.read(0,0,&value,1)&&value==42);
  assert(!kissClaimAllowed(source)); // queued complete CLI still belongs to console
  assert(controls(source)==command);
  assert(kissClaimAllowed(source));
  // ASCII records can arrive after the binary request has started. Passwords
  // and command arguments containing the response magic remain whole text,
  // including when the console already consumed the beginning of the line.
  const std::string login="ota.peer login "+std::string(64,'1')+" aims ms mms spaces\r\n";
  for(size_t split=0;split<login.size();++split){
    g_mock_millis=0;Wire racing;
    SerialMotaSource shared(racing,MotaStreamWritePolicy::NoFlush,5);shared.enableSharedTextControl();
    racing.push(login.substr(0,split));
    const auto prefix=controls(shared);
    if(split && login[split-1]!='\r' && login[split-1]!='\n')assert(!kissClaimAllowed(shared));
    racing.next_response.assign(login.begin()+split,login.end());
    const auto binary=frame({44});
    racing.next_response.insert(racing.next_response.end(),binary.begin(),binary.end());
    assert(shared.read(0,0,&value,1)&&value==44);
    assert(prefix+controls(shared)==login);
    assert(kissClaimAllowed(shared));
  }
  // Over-capacity controls invalidate the existing prefix and discard to its
  // delimiter, so the suffix never becomes a second mutating command.
  wire.push("ota.peer cmd "+std::string(700,'X')+"\r");
  wire.next_response=frame({43});
  assert(source.read(0,0,&value,1)&&value==43);
  auto discarded=controls(source);
  assert(!discarded.empty()&&discarded[0]==0);
  assert(discarded.find('X')==std::string::npos);
  wire.push("ota.peer result\r");
  assert(controls(source)=="ota.peer result\r");
  source.resetSessionState();assert(!source.hasPendingResponse());
  // An attached shared USB folder pauses immediately while KISS owns the
  // port; neither stale-drain reads nor a new MS request may touch its frame.
  Wire owned;SerialMotaSource paused(owned,MotaStreamWritePolicy::NoFlush,600);
  paused.enableSharedTextControl();owned.input={0xc0,0,0x41,0xc0};
  mesh::kiss_owned=true;const auto before_clock=g_mock_millis;
  assert(!paused.read(0,0,&value,1));
  assert(!owned.reads&&!owned.writes&&owned.input.size()==4&&g_mock_millis==before_clock);
  assert(!paused.hasPendingResponse());
  owned.input.clear();mesh::kiss_owned=false;owned.next_response=frame({45});
  assert(paused.read(0,0,&value,1)&&value==45); // same attached source resumes
  // Dedicated UART/BLE sources do not share the physical KISS stream.
  Wire dedicated;SerialMotaSource separate(dedicated,MotaStreamWritePolicy::NoFlush,5);
  dedicated.next_response=frame({46});mesh::kiss_owned=true;
  assert(separate.read(0,0,&value,1)&&value==46);
  mesh::kiss_owned=false;
}
'''


class RepeaterOtaSerialControlTest(unittest.TestCase):
    def test_late_binary_frames_and_long_interleaved_controls(self):
        with tempfile.TemporaryDirectory(prefix='meshcore-repeater-mota-control-') as folder:
            cpp = Path(folder)/'test.cpp'
            binary = Path(folder)/('test.exe' if os.name == 'nt' else 'test')
            main = (ROOT/'examples/simple_repeater/main.cpp').read_text()
            start = main.index('const auto kiss_can_read = [&]()')
            end = main.index('\n  };', start)+len('\n  };')
            cpp.write_text(HARNESS.replace('@KISS_GATE@', main[start:end]))
            sanitizers = [] if os.name == 'nt' else [
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie']
            built=subprocess.run([
                'g++','-std=c++17','-DESP32_PLATFORM=1','-DMESH_ENABLE_DIRECT_OTA_ADMIN=1',
                '-DMESH_OTA_FOLDER_SERIAL_SHARED_CONSOLE=1',
                '-DMESH_ENABLE_HOST_KISS=1','-DARDUINO=1',
                *sanitizers,
                '-I'+str(ROOT/'test/mocks'),'-I'+str(ROOT/'src'),str(cpp),
                str(ROOT/'src/helpers/ota/MotaSourceSerial.cpp'),'-o',str(binary)],
                capture_output=True,text=True)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            ran=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)


if __name__=='__main__':
    unittest.main()
