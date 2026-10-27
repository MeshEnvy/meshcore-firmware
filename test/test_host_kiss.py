#!/usr/bin/env python3
"""Run production KISS framing, persistence and session logic on the host."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

CODEC = r'''
#include <helpers/HostKissCodec.h>
#include <cassert>
#include <vector>
using mesh::hostkiss::Codec;
std::vector<uint8_t> output(Codec& codec) {
  const uint8_t* bytes;size_t length;std::vector<uint8_t> result;
  while(codec.peekOutput(bytes,length)) {
    const size_t amount=length>7?7:length;
    result.insert(result.end(),bytes,bytes+amount);codec.consumeOutput(amount);
  }
  return result;
}
void feed(Codec& codec,const std::vector<uint8_t>& bytes) {
  for(auto byte:bytes) assert(codec.feed(byte));
}
int main() {
  Codec encoder,decoder;
  for(unsigned length=1;length<=255;++length) {
    std::vector<uint8_t> bytes(length);
    for(unsigned i=0;i<length;++i) bytes[i]=uint8_t(i+length);
    assert(encoder.queueReceive(bytes.data(),bytes.size()));
    feed(decoder,output(encoder));
    const uint8_t* decoded;size_t size;
    assert(decoder.peekTransmit(decoded,size)&&size==bytes.size());
    assert(std::vector<uint8_t>(decoded,decoded+size)==bytes);
    decoder.consumeTransmit();
  }
  const uint8_t escaped[]={0xc0,0xdb,0xc0};
  assert(encoder.queueReceive(escaped,3));
  assert(output(encoder)==std::vector<uint8_t>({0xc0,0,0xdb,0xdc,0xdb,0xdd,0xdb,0xdc,0xc0}));
  // Oversized, malformed escape, empty DATA, unknown command/port and RETURN
  // with payload cannot be transformed into a valid radio packet.
  for(auto bytes:std::vector<std::vector<uint8_t>>{{0xc0,0,0xc0},
      {0xc0,0,1,0xdb,0xc0},{0xc0,0,1,0xdb,0x11,0xc0},
      {0xc0,0x10,1,2,0xc0},{0xc0,1,99,0xc0},{0xc0,0xff,1,0xc0}}) {
    decoder.reset();feed(decoder,bytes);
    const uint8_t* data;size_t size;assert(!decoder.peekTransmit(data,size)&&!decoder.returned());
  }
  std::vector<uint8_t> oversized(260,1);oversized[0]=0xc0;oversized[1]=0;oversized.back()=0xc0;
  decoder.reset();feed(decoder,oversized);
  const uint8_t* data;size_t size;assert(!decoder.peekTransmit(data,size));
  assert(decoder.malformedFrames()>=5);
  feed(decoder,{0xc0,0,17,0xc0,0,18,0xc0});
  assert(decoder.queuedTransmit()==2&&!decoder.canAcceptByte()&&!decoder.feed(0xc0));
  decoder.consumeTransmit();assert(decoder.canAcceptByte());
  assert(decoder.peekTransmit(data,size)&&size==1&&data[0]==18);
  decoder.reset();feed(decoder,{1,2,3,0xc0,0xff,0xc0});
  assert(decoder.returned()&&!decoder.canAcceptByte());decoder.reset();
  assert(!decoder.returned()&&decoder.canAcceptByte());
  assert(encoder.queueReady()&&encoder.queueTxDone(false));
  assert(!encoder.queueReceive(escaped,3)&&encoder.droppedFrames()==1);
  assert(output(encoder)==std::vector<uint8_t>({0xc0,0x0f,1,0xc0,0xc0,0x06,0xf8,0,0xc0}));
  assert(!encoder.queueReceive(nullptr,1)&&!encoder.queueReceive(escaped,0));
  // Fuzz framing under sanitizers with periodic draining/backpressure.
  uint32_t state=17;
  for(unsigned i=0;i<200000;++i) {
    state=state*1664525+1013904223;
    if(!decoder.canAcceptByte()) decoder.reset();
    decoder.feed(uint8_t(state>>24));
    if(i%317==0) decoder.reset();
  }
}
'''

SETTINGS = r'''
#include <helpers/HostKissSettings.h>
#include <helpers/PersistentStoreFormat.h>
#include <cassert>
using namespace mesh::hostkiss;
using Bytes=std::vector<uint8_t>;
constexpr char Path[]="/host_kiss";
Bytes image(Selection selection) {
  Bytes bytes={'H','K','S','1',uint8_t(selection.enabled),uint8_t(selection.transport),0,0,0,0,0,0};
  mesh::storage::writeLE32(bytes.data()+8,mesh::storage::updateCRC32(0xffffffff,bytes.data(),8));return bytes;
}
int main() {
  Selection on{true,Transport::Usb},tcp{true,Transport::Tcp},off{false,Transport::Usb};
  GuestMemoryFS fs;Settings store;
  assert(store.begin(&fs)&&store.healthy()&&store.selection()==off&&fs.writes==0);
  assert(store.save(on)&&store.selection()==on&&fs.files.at(Path)==image(on));
  assert(store.save(tcp)&&store.selection()==tcp&&fs.files.at(Path)==image(tcp));
  Settings reboot;assert(reboot.begin(&fs)&&reboot.selection()==tcp);
  assert(reboot.save(off)&&reboot.selection()==off);
  for(unsigned size=0;size<25;++size) {
    if(size==12)continue;
    GuestMemoryFS broken;broken.files[Path]=image(on);broken.files[Path].resize(size);
    Settings read;assert(!read.begin(&broken)&&read.selection()==off);
    assert(read.save(off));
  }
  for(unsigned byte=0;byte<12;++byte) for(unsigned bit=0;bit<8;++bit) {
    GuestMemoryFS broken;broken.files[Path]=image(tcp);broken.files[Path][byte]^=1<<bit;
    Settings read;assert(!read.begin(&broken)&&read.selection()==off);
  }
  for(unsigned fault=0;fault<7;++fault) {
    GuestMemoryFS broken;broken.files[Path]=image(on);Settings read;assert(read.begin(&broken));
    if(fault==0)broken.fail_open_write=true;
    if(fault==1)broken.short_write=5;
    if(fault==2)broken.corrupt_stage=true;
    if(fault==3)broken.fail_stage_open=true;
    if(fault==4)broken.fail_stage_read=true;
    if(fault==5)broken.fail_publish=true;
    if(fault==6)broken.false_publish=true;
    assert(!read.save(off)&&read.selection()==on);
    assert(broken.files.at(Path)==image(on));
  }
  for(auto old:{off,on,tcp}) {
    GuestMemoryFS backing;backing.files[Path]=image(old);Settings read;assert(read.begin(&backing));
    backing.capture=true;assert(read.save(old==off?tcp:off));
    const auto snapshots=backing.snapshots;assert(snapshots.size()>10);
    for(const auto& snapshot:snapshots) {
      GuestMemoryFS resumed;resumed.files=snapshot;Settings recovered;assert(recovered.begin(&resumed));
      if(snapshot.count(Path)) assert(image(recovered.selection())==snapshot.at(Path));
      else if(snapshot.count("/host_kiss.bak")) assert(image(recovered.selection())==snapshot.at("/host_kiss.bak"));
      else assert(recovered.selection()==off);
    }
  }
  GuestMemoryFS scratch;scratch.files["/host_kiss.tmp"]=image(tcp);
  Settings discarded;assert(discarded.begin(&scratch)&&discarded.selection()==off);
}
'''

RADIO_STUB = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vector>
namespace mesh {
enum class RadioProfileProtocol:uint8_t {MeshCore=0,Meshtext=1,Ethernet=2,Kiss=3};
enum class RadioProfileMode:uint8_t {Off=0,Rx=1,RxTx=2};
struct RadioProfileParams {RadioProfileProtocol protocol=RadioProfileProtocol::MeshCore;};
struct RadioProfileConfig {RadioProfileParams params;RadioProfileMode mode=RadioProfileMode::RxTx;};
struct RadioProfiles {RadioProfileConfig secondary;uint32_t generation[2]={1,1};bool primary_temporary=false,secondary_temporary=false;};
class Radio {public:RadioProfiles p;bool supported=true;bool supportsHostKiss()const{return supported;}RadioProfiles* profiles(){return &p;}};
class Dispatcher {public:
  bool pending=false,allow=true;unsigned submitted=0,cancelled=0;uint32_t gen=0;std::vector<uint8_t> packet;
  bool submitHostKissFrame(const uint8_t* bytes,size_t size,uint32_t generation){
    if(pending||!allow)return false;pending=true;++submitted;gen=generation;packet.assign(bytes,bytes+size);return true;}
  bool cancelHostKissFrame(){++cancelled;if(!pending)return false;pending=false;return true;}
  bool hostKissFramePending()const{return pending;}
};
}
'''
PROFILE_STUB = r'''
#pragma once
#include <Dispatcher.h>
namespace mesh {class RadioProfileCLI {
 public:Radio* radio;RadioProfileConfig saved;bool manual=true,busy=false,overlay=false;uint32_t intent=0;unsigned clear=0;
  explicit RadioProfileCLI(Radio& value):radio(&value){}
  bool manualSecondaryConfigured()const{return manual;}
  const RadioProfileConfig& savedSecondaryConfig()const{return saved;}
  bool secondaryRuntimeAvailable()const{return !busy;}
  uint32_t secondaryIntentGeneration()const{return intent;}
  bool useSecondaryProtocol(RadioProfileProtocol protocol){
    if(busy||!manual)return false;radio->p.secondary.params.protocol=protocol;++radio->p.generation[1];overlay=true;return true;}
  void clearSecondaryProtocol(){++clear;if(overlay){radio->p.secondary=saved;++radio->p.generation[1];overlay=false;}}
};}
'''
USB_STUB = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <deque>
#include <vector>
struct Stream {
  std::deque<uint8_t> input;std::vector<uint8_t> output;size_t write_budget=128;
  int available(){return int(input.size());}int peek(){return input.empty()?-1:input.front();}
  int read(){if(input.empty())return -1;int byte=input.front();input.pop_front();return byte;}
  size_t write(const uint8_t* bytes,size_t length){if(length>write_budget)length=write_budget;output.insert(output.end(),bytes,bytes+length);return length;}
};
inline Stream serial;
inline uint32_t fake_millis=0;
inline uint32_t millis(){return fake_millis;}
namespace mesh {
inline bool binary_owned=false;
inline bool binary_ready=true;
inline Stream& usbCompanionPort(){return serial;}
inline void setUsbBinaryStreamOwned(bool value){binary_owned=value;}
inline bool isUsbBinaryStreamReady(){return binary_ready;}
}
'''

NETWORK_STUB = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <deque>
#include <vector>
#include <memory>
#include <map>
#include <cerrno>
#define WIFI_STA 1
#define WIFI_AP 2
#define WL_CONNECTED 3
#define MSG_DONTWAIT 0x40
struct Socket {bool connected=true;std::deque<uint8_t> input;std::vector<uint8_t> output;size_t write_budget=128;};
inline std::map<int,std::shared_ptr<Socket>> sockets;
inline std::deque<int> accepts;
inline int next_socket=1;
inline int newSocket(){int id=next_socket++;sockets[id]=std::make_shared<Socket>();return id;}
class IPAddress {uint32_t value_;public:IPAddress(uint32_t value=0):value_(value){}operator uint32_t()const{return value_;}};
struct Wifi {int mode=0,status_value=0;uint32_t address=0;
  int getMode()const{return mode;}int status()const{return status_value;}IPAddress localIP()const{return IPAddress(address);}};
inline Wifi WiFi;
class WiFiClient {int id_=0;public:
  WiFiClient()=default;explicit WiFiClient(int id):id_(id){}
  explicit operator bool()const{return id_&&sockets.at(id_)->connected;}
  bool connected()const{return bool(*this);}int fd()const{return id_;}
  void stop(){if(id_)sockets.at(id_)->connected=false;id_=0;}void setNoDelay(bool){}
};
inline uint32_t bound_address=0;inline uint16_t bound_port=0;inline unsigned listens=0;
class WiFiServer {uint32_t address_=0;uint16_t port_=0;bool started_=false;
 public:WiFiServer(uint16_t port,int):port_(port){}WiFiServer(IPAddress address,uint16_t port,int):address_(address),port_(port){}
  void begin(){started_=true;bound_address=address_;bound_port=port_;++listens;}
  void end(){started_=false;while(!accepts.empty()){sockets.at(accepts.front())->connected=false;accepts.pop_front();}}
  explicit operator bool()const{return started_;}
  WiFiClient available(){if(accepts.empty())return {};int id=accepts.front();accepts.pop_front();return WiFiClient(id);}
};
inline int send(int fd,const void* data,size_t length,int flags){
  if(flags!=MSG_DONTWAIT)__builtin_trap();auto sock=sockets.at(fd);
  if(!sock->connected){errno=EPIPE;return -1;}if(length>sock->write_budget)length=sock->write_budget;
  const auto* bytes=static_cast<const uint8_t*>(data);sock->output.insert(sock->output.end(),bytes,bytes+length);return int(length);
}
inline int recv(int fd,void* data,size_t length,int flags){
  if(flags!=MSG_DONTWAIT||length!=1)__builtin_trap();auto sock=sockets.at(fd);
  if(!sock->connected)return 0;if(sock->input.empty()){errno=EAGAIN;return -1;}
  *static_cast<uint8_t*>(data)=sock->input.front();sock->input.pop_front();return 1;
}
'''

TCP_RUNTIME = r'''
#include <helpers/HostKiss.h>
#include <Dispatcher.h>
#include <RadioProfileCLI.h>
#include <UsbLogging.h>
#include <WiFi.h>
#include <cassert>
#include <cstring>
using namespace mesh;
char reply[240];
void command(const char* text){assert(hostkiss::handleCommand(text,reply,sizeof(reply),true));}
void input(int socket,std::initializer_list<uint8_t> bytes){auto& q=sockets.at(socket)->input;q.insert(q.end(),bytes);}
int main(){
  GuestMemoryFS fs;Radio radio;RadioProfileCLI profiles(radio);Dispatcher dispatcher;
  hostkiss::begin(&fs,radio,profiles,dispatcher);
  command("set kiss.transport tcp");assert(strstr(reply,"OK"));
  command("set kiss on");assert(strstr(reply,"OK"));
  WiFi.mode=WIFI_AP;WiFi.status_value=WL_CONNECTED;WiFi.address=123;
  hostkiss::loop();assert(listens==0);
  command("get kiss");assert(strstr(reply,"waiting-wifi"));
  WiFi.mode=WIFI_STA|WIFI_AP;hostkiss::loop();assert(listens==1&&bound_address==123&&bound_port==5004);
  command("get kiss");assert(strstr(reply,"listening"));
  const int first=newSocket();accepts.push_back(first);hostkiss::loop();
  assert(hostkiss::radioActive()&&!hostkiss::usbActive()&&!binary_owned);
  assert(sockets.at(first)->output==std::vector<uint8_t>({0xc0,0x0f,1,0xc0}));
  const int rejected=newSocket();accepts.push_back(rejected);hostkiss::loop();
  assert(!sockets.at(rejected)->connected&&sockets.at(first)->connected);
  input(first,{0xc0,0,1,2,3,0xc0});hostkiss::loop();
  assert(dispatcher.submitted==1&&dispatcher.packet==std::vector<uint8_t>({1,2,3}));
  dispatcher.pending=false;hostkiss::sendComplete(true);hostkiss::loop();hostkiss::loop();
  assert(sockets.at(first)->output==std::vector<uint8_t>({0xc0,0x0f,1,0xc0,0xc0,6,0xf8,1,0xc0,0xc0,0x0f,1,0xc0}));
  // Backpressure preserves every escaped output byte across partial sends.
  sockets.at(first)->output.clear();sockets.at(first)->write_budget=3;
  uint8_t packet[255];memset(packet,0xc0,sizeof(packet));
  hostkiss::receive(packet,sizeof(packet),1,radio.p.generation[1],0,0);
  for(unsigned i=0;i<200;++i)hostkiss::loop();
  const auto& bytes=sockets.at(first)->output;
  assert(bytes.size()==513&&bytes[0]==0xc0&&bytes[1]==0&&bytes.back()==0xc0);
  for(unsigned i=2;i<512;i+=2)assert(bytes[i]==0xdb&&bytes[i+1]==0xdc);
  // The profile change drops both the active stream and pending accepts.
  const int stale=newSocket();accepts.push_back(stale);input(stale,{0xc0,0,99,0xc0});
  ++radio.p.generation[1];hostkiss::loop();
  assert(!sockets.at(first)->connected&&!sockets.at(stale)->connected&&!dispatcher.pending);
  assert(dispatcher.submitted==1&&!hostkiss::radioActive());
  // A new explicit runtime profile can start a fresh stream; old RX cannot.
  profiles.overlay=false;radio.p.secondary=profiles.saved;++radio.p.generation[1];
  const int second=newSocket();accepts.push_back(second);hostkiss::loop();
  assert(hostkiss::radioActive()&&sockets.at(second)->connected);
  const unsigned before=dispatcher.submitted;
  input(second,{0xc0,0xff,0xc0});hostkiss::loop();
  assert(!sockets.at(second)->connected&&!hostkiss::radioActive()&&dispatcher.submitted==before);
  // Losing station WiFi tears down the raw radio lease and listener.
  const int third=newSocket();accepts.push_back(third);hostkiss::loop();assert(hostkiss::radioActive());
  WiFi.mode=WIFI_AP;hostkiss::loop();assert(!hostkiss::radioActive()&&!sockets.at(third)->connected);
  command("set kiss off");assert(strstr(reply,"OK"));
  WiFi.mode=WIFI_STA;command("set kiss on");hostkiss::loop();
  // A single connection survives the two-second settings handoff. No
  // pre-READY DATA, including a partial escaped frame, can reach the radio.
  profiles.busy=true;const int waiting=newSocket();accepts.push_back(waiting);
  input(waiting,{0xc0,0,91,0xdb});hostkiss::loop();
  assert(sockets.at(waiting)->connected&&!hostkiss::radioActive());
  assert(sockets.at(waiting)->output.empty());command("get kiss");assert(strstr(reply,"waiting-radio"));
  fake_millis=1999;hostkiss::loop();assert(sockets.at(waiting)->output.empty());
  profiles.busy=false;fake_millis=2000;hostkiss::loop();assert(sockets.at(waiting)->output.empty());
  input(waiting,{0xdc,0xc0});hostkiss::loop();
  assert(sockets.at(waiting)->connected&&hostkiss::radioActive());
  assert(sockets.at(waiting)->output==std::vector<uint8_t>({0xc0,0x0f,1,0xc0}));
  assert(dispatcher.submitted==before);
  input(waiting,{0,71,0xc0});hostkiss::loop();assert(dispatcher.submitted==before+1&&dispatcher.packet[0]==71);
  command("set kiss off");assert(!sockets.at(waiting)->connected&&!dispatcher.pending);
  // Initial conflicts wait automatically, but RETURN can cancel that wait.
  command("set kiss on");profiles.manual=false;hostkiss::loop();
  const int conflict=newSocket();accepts.push_back(conflict);hostkiss::loop();
  assert(sockets.at(conflict)->connected&&!hostkiss::radioActive());
  input(conflict,{0xc0,0xff,0xc0});hostkiss::loop();assert(!sockets.at(conflict)->connected);
  const int resolves=newSocket();accepts.push_back(resolves);hostkiss::loop();
  profiles.manual=true;hostkiss::loop();assert(sockets.at(resolves)->connected&&hostkiss::radioActive());
  command("set kiss off");
  // A stable, fresh connection preserves immediate first DATA from a generic
  // KISS client which does not wait for the READY extension.
  command("set kiss on");hostkiss::loop();const int immediate=newSocket();
  input(immediate,{0xc0,0,72,0xc0});accepts.push_back(immediate);hostkiss::loop();
  assert(dispatcher.submitted==before+2&&dispatcher.packet==std::vector<uint8_t>({72}));
  command("set kiss off");
}
'''

RUNTIME = r'''
#include <helpers/HostKiss.h>
#include <helpers/HostKissSettings.h>
#include <Dispatcher.h>
#include <RadioProfileCLI.h>
#include <UsbLogging.h>
#include <cassert>
#include <cstring>
using namespace mesh;
void input(std::initializer_list<uint8_t> bytes){serial.input.insert(serial.input.end(),bytes);}
void poll(){hostkiss::loop();hostkiss::serviceUsb();}
char reply[240];
void command(const char* text,bool local=true){assert(hostkiss::handleCommand(text,reply,sizeof(reply),local));}
int main() {
  GuestMemoryFS fs;Radio radio;RadioProfileCLI profiles(radio);Dispatcher dispatcher;
  hostkiss::begin(&fs,radio,profiles,dispatcher);
  command("get kiss");assert(strstr(reply,"> off"));
  input({0xc0,0,1,0xc0});assert(!hostkiss::serviceUsb()&&serial.available()==4);
  serial.input.clear();
  command("set kiss on",false);assert(strstr(reply,"local connection"));
  command("set kiss on");assert(strstr(reply,"OK"));
  hostkiss::Settings saved;assert(saved.begin(&fs)&&saved.selection().enabled);
  input({'g','e','t',' '});assert(!hostkiss::serviceUsb()&&serial.available()==4);
  serial.input.clear();
  // First FEND can arrive during the saved radio2 two-second handoff. The
  // same connection becomes ready automatically once the tuple is stable.
  profiles.busy=true;input({0xc0});poll();
  assert(hostkiss::usbActive()&&!hostkiss::radioActive()&&serial.output.empty());
  command("get kiss");assert(strstr(reply,"waiting-radio")&&strstr(reply,"pending radio settings"));
  fake_millis=1999;poll();assert(serial.output.empty());
  profiles.busy=false;fake_millis=2000;poll();
  assert(hostkiss::radioActive()&&serial.output==std::vector<uint8_t>({0xc0,0x0f,1,0xc0}));
  hostkiss::resetUsbSession();serial.output.clear();
  // Drain a complete pre-READY frame and wait for the remainder of a partial
  // escaped frame before announcing READY. Only fresh post-READY DATA sends.
  profiles.busy=true;input({0xc0,0,90,0xc0,0,91,0xdb});poll();
  profiles.busy=false;poll();assert(serial.output.empty()&&!hostkiss::radioActive());
  input({0xdc,0xc0});poll();assert(hostkiss::radioActive());
  assert(dispatcher.submitted==0&&serial.output==std::vector<uint8_t>({0xc0,0x0f,1,0xc0}));
  hostkiss::resetUsbSession();serial.output.clear();
  // Another initial owner can release radio2 without a reconnect.
  radio.p.secondary.params.protocol=RadioProfileProtocol::Meshtext;input({0xc0});poll();
  assert(!hostkiss::radioActive()&&serial.output.empty());
  radio.p.secondary.params.protocol=RadioProfileProtocol::MeshCore;++radio.p.generation[1];poll();
  assert(hostkiss::radioActive());hostkiss::resetUsbSession();serial.output.clear();
  profiles.manual=false;input({0xc0});assert(hostkiss::serviceUsb()&&hostkiss::usbActive()&&binary_owned);
  assert(!hostkiss::radioActive());input({0xff,0xc0});assert(hostkiss::serviceUsb()&&!hostkiss::usbActive());
  profiles.manual=true;input({0xc0});binary_ready=false;
  const size_t before_ready=serial.output.size();
  assert(hostkiss::serviceUsb()&&hostkiss::usbActive()&&binary_owned);
  assert(serial.available()==1&&serial.output.size()==before_ready);
  binary_ready=true;assert(hostkiss::serviceUsb()&&serial.available()==0);
  assert(hostkiss::radioActive()&&radio.p.secondary.params.protocol==RadioProfileProtocol::Kiss);
  assert(profiles.saved.params.protocol==RadioProfileProtocol::MeshCore);
  input({0,7,0xdb,0xdc,0xdb,0xdd,0xc0});poll();
  assert(dispatcher.submitted==1&&dispatcher.packet==std::vector<uint8_t>({7,0xc0,0xdb}));
  input({0,8,0xc0});poll();assert(dispatcher.submitted==1);
  dispatcher.pending=false;hostkiss::sendComplete(true);poll();assert(dispatcher.submitted==2);
  // Cross-profile or old-generation RX can never leak into the host stream.
  poll();poll();
  serial.output.clear();uint8_t rx[]={9};
  hostkiss::receive(rx,1,0,radio.p.generation[1],0,0);
  hostkiss::receive(rx,1,1,radio.p.generation[1]-1,0,0);poll();
  assert(serial.output.empty());
  hostkiss::receive(rx,1,1,radio.p.generation[1],0,0);poll();
  assert(serial.output==std::vector<uint8_t>({0xc0,0,9,0xc0}));
  // A failed settings write preserves the active session and lease.
  fs.fail_open_write=true;command("set kiss off");
  assert(strstr(reply,"save failed")&&hostkiss::usbActive()&&hostkiss::radioActive());
  fs.fail_open_write=false;
  // Profile invalidation cancels stale TX but quarantines USB binary input.
  radio.p.secondary.params.protocol=RadioProfileProtocol::Meshtext;
  ++radio.p.generation[1];const unsigned cleared=profiles.clear;
  poll();assert(hostkiss::usbActive()&&!hostkiss::radioActive());
  assert(profiles.clear==cleared&&radio.p.secondary.params.protocol==RadioProfileProtocol::Meshtext);
  const unsigned submitted=dispatcher.submitted;
  radio.p.secondary.params.protocol=RadioProfileProtocol::MeshCore;++radio.p.generation[1];poll();
  assert(!hostkiss::radioActive()); // An established invalidated session cannot reacquire.
  input({0xc0,0,99,0xc0});poll();assert(dispatcher.submitted==submitted);
  input({0xc0,0xff,0xc0});assert(hostkiss::serviceUsb());
  assert(!hostkiss::usbActive()&&!binary_owned&&!dispatcher.pending);
  hostkiss::sendComplete(true); // late completion cannot enter a new session
  profiles.overlay=false;radio.p.secondary=profiles.saved;++radio.p.generation[1];
  input({0xc0});poll();input({0,17,0xc0});poll();assert(dispatcher.submitted==submitted+1);
  hostkiss::resetUsbSession();assert(!hostkiss::usbActive()&&!binary_owned&&!dispatcher.pending);
  // A partially written READY is already observable; a later profile change
  // quarantines it rather than manufacturing READY on another generation.
  serial.output.clear();serial.write_budget=1;input({0xc0});poll();
  assert(serial.output.size()==1&&!hostkiss::radioActive());
  ++radio.p.generation[1];poll();serial.write_budget=128;poll();
  assert(!hostkiss::radioActive()&&hostkiss::usbActive());
  input({0xc0,0xff,0xc0});poll();assert(!hostkiss::usbActive());
  // Stable radio2 also preserves a complete first DATA before the peer reads
  // READY, as ordinary KISS hosts with flow control disabled require.
  const unsigned before_immediate=dispatcher.submitted;
  input({0xc0,0,33,0xc0});poll();
  assert(dispatcher.submitted==before_immediate+1&&dispatcher.packet==std::vector<uint8_t>({33}));
  hostkiss::resetUsbSession();
  // Off must be durable, releases the overlay and cannot consume binary input.
  command("set kiss off");assert(strstr(reply,"OK")&&!hostkiss::radioActive());
  assert(saved.begin(&fs)&&!saved.selection().enabled);
  input({0xc0,0,8,0xc0});assert(!hostkiss::serviceUsb());serial.input.clear();
  hostkiss::begin(&fs,radio,profiles,dispatcher);command("get kiss");assert(strstr(reply,"> off"));
  command("set kiss.transport tcp");assert(strstr(reply,"requires ESP32"));
  assert(!hostkiss::handleCommand("get kissing",reply,sizeof(reply),true));
}
'''


class HostKissTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which("g++") or shutil.which("clang++")
        if not cls.compiler:
            raise unittest.SkipTest("host C++ compiler required")
        cls.work = tempfile.TemporaryDirectory(prefix="mesh-host-kiss-")
        cls.path = Path(cls.work.name)
        cls.mocks = cls.path / "mocks"
        shutil.copytree(ROOT / "test/fixtures/guest_login/mocks", cls.mocks)
        for path in cls.mocks.rglob("*"):
            if path.is_file():
                path.write_text(path.read_text().replace("/guest_login", "/host_kiss"), encoding="ascii")

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def build_run(self, name, source, backend=None, runtime=False):
        path = self.path / (name + ".cpp")
        path.write_text(source, encoding="ascii")
        binary = self.path / name
        flags = ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                 "-Wno-misleading-indentation", "-Wno-sign-compare"]
        if os.name != "nt":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
        sources = [str(path)]
        includes = ["-I", str(self.mocks), "-I", str(ROOT / "src")]
        if backend:
            flags += ["-DMESH_ENABLE_HOST_KISS=1", "-D" + backend + "_PLATFORM"]
            if backend == "ESP32":
                flags += ["-DESP32"]
            sources.append(str(ROOT / "src/helpers/HostKissSettings.cpp"))
            if backend in ("NRF52", "STM32"):
                sources.append(str(ROOT / "src/helpers/AtomicFileWriter.cpp"))
        if runtime:
            runtime_path = self.path / "HostKiss.cpp"
            shutil.copyfile(ROOT / "src/helpers/HostKiss.cpp", runtime_path)
            sources.append(str(runtime_path))
            includes += ["-I", str(ROOT / "src/helpers")]
            (self.mocks / "Dispatcher.h").write_text(RADIO_STUB, encoding="ascii")
            (self.mocks / "RadioProfileCLI.h").write_text(PROFILE_STUB, encoding="ascii")
            (self.mocks / "UsbLogging.h").write_text(USB_STUB, encoding="ascii")
            if backend == "ESP32":
                (self.mocks / "WiFi.h").write_text(NETWORK_STUB, encoding="ascii")
                (self.mocks / "lwip").mkdir(exist_ok=True)
                (self.mocks / "lwip/sockets.h").write_text('#include <WiFi.h>\n', encoding="ascii")
        result = subprocess.run([self.compiler, *flags, *includes, *sources, "-o", str(binary)],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_framing_limits_escaping_flow_and_malformed_input(self):
        self.build_run("codec", CODEC)

    def test_saved_settings_transaction_and_reboot_boundaries(self):
        for backend in ("ESP32", "RP2040", "NRF52", "STM32"):
            with self.subTest(backend=backend):
                self.build_run("settings_" + backend, SETTINGS, backend)

    def test_usb_session_ownership_stale_frames_and_toggle(self):
        self.build_run("runtime", RUNTIME, "NRF52", runtime=True)

    def test_tcp_station_binding_backpressure_sessions_and_toggle(self):
        self.build_run("tcp_runtime", TCP_RUNTIME, "ESP32", runtime=True)

    def test_disabled_header_needs_no_platform_types(self):
        self.build_run("disabled", r'''
#include <helpers/HostKiss.h>
int main(){int a=0;mesh::hostkiss::begin(&a,a,a,a);
  return mesh::hostkiss::serviceUsb()||mesh::hostkiss::radioActive();}
''')


if __name__ == "__main__":
    unittest.main()
