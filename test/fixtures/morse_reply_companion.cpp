#include <helpers/TempRadioReplyBarrier.h>
namespace mesh { struct Packet { bool radio_reply=false; }; }
struct ContactInfo {
  struct { uint8_t pub_key[32]={}; } id;
  bool allowed=true;
  bool isRemoteCLIAllowed() const { return allowed; }
};
static uint32_t millis() { return 100; }
#define PUB_KEY_SIZE 32
#define TXT_TYPE_CLI_COMMAND 1
struct MyMesh {
  mesh::MorseReplyHandoff _morse_reply;
  bool _morse_reply_admission=false;
  mesh::TempRadioReplyBarrier _primary_radio_reply;
  bool _primary_radio_reply_admission=false;
  TestRadio radio;
  unsigned active=0, forwarded=0;
  uint32_t handled_timestamp=0;
  MyMesh() { radio.rx_supported=true; }
  void markConnectionActive(const ContactInfo&) { ++active; }
  void queueMessage(const ContactInfo&,uint8_t,mesh::Packet*,uint32_t,const void*,size_t,const char*) { ++forwarded; }
  bool handleCommand(const char* text,uint32_t timestamp,char* reply) {
    handled_timestamp=timestamp;
    return mesh::handleMorseReplyCommand(_morse_reply,&radio,text,reply,160,millis())
        || mesh::handleMorseCommand(&radio,text,reply,160);
  }
  void onCLICommandRecv(const ContactInfo&,mesh::Packet*,uint32_t,const char*,char*);
  void onCLIReplyPrepared(const ContactInfo&,mesh::Packet*);
  void onCLIReplyQueued(const ContactInfo&,mesh::Packet*,bool);
};
@METHODS@
int main() {
  ContactInfo actor, other; actor.id.pub_key[0]=1; other.id.pub_key[0]=2;
  mesh::Packet reply_packet, copy;
  char reply[160]={};
  for (unsigned failure=0; failure<3; ++failure) {
    MyMesh node;
    node.onCLICommandRecv(actor,nullptr,0,"morse listen 1",reply);
    assert(node.handled_timestamp==1 && node._morse_reply_admission && node._morse_reply.pending());
    assert(node.active==1 && !node.radio.rx_starts);
    node.onCLIReplyPrepared(other,&reply_packet);
    assert(!node._morse_reply.ownsReply(&reply_packet));
    if (failure==0) {
      node.onCLIReplyPrepared(actor,&reply_packet);
      assert(reply_packet.radio_reply && node._morse_reply.ownsReply(&reply_packet));
      node._morse_reply.copy(&reply_packet,&copy);
      node.onCLIReplyQueued(actor,&reply_packet,true);
      assert(!node._morse_reply_admission);
      node._morse_reply.complete(&reply_packet);
      assert(!node._morse_reply.service(&node.radio,100,true,true,902100,25));
      node._morse_reply.complete(&copy);
      assert(node._morse_reply.service(&node.radio,100,true,true,902100,25));
      assert(node.radio.rx_starts==1 && node.radio.rx_duration==60000);
    } else {
      node.onCLIReplyPrepared(actor,failure==1 ? nullptr : &reply_packet);
      node.onCLIReplyQueued(actor,failure==1 ? nullptr : &reply_packet,false);
      assert(!node._morse_reply_admission && !node._morse_reply.pending() && !node.radio.rx_starts);
    }
  }
  MyMesh denied; actor.allowed=false;
  denied.onCLICommandRecv(actor,nullptr,0,"morse listen 1",reply);
  assert(denied.forwarded==1 && !denied.handled_timestamp && !denied._morse_reply.pending());
}
