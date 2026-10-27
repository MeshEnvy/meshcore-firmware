"""Execute Companion's guarded Binary modes and decrypted RX admission offline."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_direct_ota_admin_client import STUB
from test_replay_reset_integration import extract_braced


ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <helpers/CompanionDirectAdminGuard.h>
#define CMD_SEND_PATH_DISCOVERY_REQ 52
#define ERR_CODE_NOT_FOUND 1
#define ERR_CODE_BAD_STATE 2
struct ContactInfo {
  mesh::Identity id;
  uint8_t secret[32];
  ContactInfo(){memset(id.pub_key,0x11,32);memset(secret,0x44,32);}
  const uint8_t* getSharedSecret(const mesh::LocalIdentity&)const{return secret;}
};
struct Timer { uint32_t now=100;uint32_t getMillis()const{return now;} };
struct BaseChatMesh:mesh::Mesh {
  int data_calls=0,path_calls=0;
  uint8_t getContactTxRadio(const mesh::Identity&)const{return 2;}
  void onPeerDataRecv(mesh::Packet*,uint8_t,int,const uint8_t*,uint8_t*,size_t){++data_calls;}
  bool onPeerPathRecv(mesh::Packet*,int,const uint8_t*,uint8_t*,uint8_t,uint8_t,uint8_t*,uint8_t){++path_calls;return true;}
};
struct MyMesh:BaseChatMesh {
  Timer timer;Timer* _ms=&timer;ContactInfo contact;
  uint8_t cmd_frame[64]{};int response=-1;
  mesh::CompanionDirectAdminGuard _ota_direct_admin;
  ContactInfo* lookupContactByPubKey(const uint8_t* key,int){return !memcmp(key,contact.id.pub_key,32)?&contact:nullptr;}
  void writeOKFrame(){response=0;}
  void writeErrFrame(int value){response=value;}
  void dispatch(size_t len){
    response=-1;
#if FULL
    @BINARY@
#else
    (void)len;
#endif
  }
  void mode(int value){cmd_frame[0]=52;cmd_frame[1]=value;memcpy(cmd_frame+2,contact.id.pub_key,32);dispatch(34);}
  bool command(const char* command,uint32_t sender_timestamp,char* reply){
    const size_t reply_capacity=160;
#if FULL
    @GETTER@
#else
    (void)command;(void)sender_timestamp;(void)reply;(void)reply_capacity;
#endif
    return false;
  }
  void onPeerDataRecv(mesh::Packet*,uint8_t,int,const uint8_t*,uint8_t*,size_t);
  uint8_t getContactTxRadio(const mesh::Identity&)const;
  bool onPeerPathRecv(mesh::Packet*,int,const uint8_t*,uint8_t*,uint8_t,uint8_t,uint8_t*,uint8_t);
  void rawRoute(mesh::Packet* pkt){
#if FULL
    @RAW_ROUTE@
#else
    (void)pkt;
#endif
  }
};
@CALLBACKS@
int main(){
  MyMesh m;char reply[160]{};
  m.mode(1);
#if FULL
  assert(m.response==0&&m.calls==0); // capability negotiation has no RF
  assert(m.getContactTxRadio(m.contact.id)==mesh::RADIO_TX_PRIMARY);
  mesh::Identity another;assert(m.getContactTxRadio(another)==2);
  mesh::Packet ota;ota.payload_type=12;m.rawRoute(&ota);assert(ota.tx_radio==mesh::RADIO_TX_PRIMARY);
  mesh::Packet unrelated;unrelated.payload_type=2;m.rawRoute(&unrelated);assert(unrelated.tx_radio==0);
  assert(m.command("get ota.direct",0,reply)&&strstr(reply,"active=1 busy=0"));
  assert(!m.command("get ota.direct",1,reply));
  uint8_t body[13]{};mesh::Packet packet;packet.direct=false;
  uint8_t other[32];memset(other,0x77,32);
  m.onPeerDataRecv(&packet,PAYLOAD_TYPE_RESPONSE,0,m.contact.secret,body,13);assert(m.data_calls==0);
  m._ota_direct_admin.beginLogin(m.contact.id,m.timer.now);
  m.onPeerDataRecv(&packet,PAYLOAD_TYPE_RESPONSE,0,m.contact.secret,body,13);assert(m.data_calls==1);
  packet.path_len=1;
  m.onPeerDataRecv(&packet,PAYLOAD_TYPE_RESPONSE,0,m.contact.secret,body,13);assert(m.data_calls==1);
  m.onPeerDataRecv(&packet,PAYLOAD_TYPE_RESPONSE,0,other,body,13);assert(m.data_calls==2);
  packet.path_len=0;
  m.onPeerDataRecv(&packet,PAYLOAD_TYPE_TXT_MSG,0,m.contact.secret,body,13);assert(m.data_calls==2);
  m.mode(2);assert(m.response==0&&m.type==PAYLOAD_TYPE_PATH&&m.packet.tx_radio==mesh::RADIO_TX_PRIMARY);
  assert(m.command("get ota.direct",0,reply)&&strstr(reply,"busy=1"));
  m.pending=false;assert(m.command("get ota.direct",0,reply)&&strstr(reply,"busy=0"));
  m.onPeerDataRecv(&packet,PAYLOAD_TYPE_RESPONSE,0,m.contact.secret,body,13);assert(m.data_calls==2);
  packet.direct=true;
  m.onPeerDataRecv(&packet,PAYLOAD_TYPE_TXT_MSG,0,m.contact.secret,body,13);assert(m.data_calls==3);
  assert(!m.onPeerPathRecv(&packet,0,m.contact.secret,body,1,0,body,0));assert(m.path_calls==0);
  assert(m.onPeerPathRecv(&packet,0,m.contact.secret,body,0,0,body,0));assert(m.path_calls==1);
  m.mode(3);assert(m.response==0&&!m._ota_direct_admin.active(m.timer.now));
  assert(m.getContactTxRadio(m.contact.id)==2);
  ota.tx_radio=0;m.rawRoute(&ota);assert(ota.tx_radio==0);
  packet.direct=false;packet.path_len=2;
  m.onPeerDataRecv(&packet,PAYLOAD_TYPE_TXT_MSG,0,m.contact.secret,body,13);assert(m.data_calls==4);
  m.timer.now=0xFFFFFF00;m.mode(1);assert(m._ota_direct_admin.active(m.timer.now+1));
  assert(!m._ota_direct_admin.active(m.timer.now+mesh::CompanionDirectAdminGuard::LEASE_MS));
  m.mode(2);assert(m.response==ERR_CODE_BAD_STATE);
  m.cmd_frame[1]=1;m.cmd_frame[2]^=1;m.dispatch(34);assert(m.response==ERR_CODE_NOT_FOUND);
#else
  assert(m.response==-1&&m.calls==0); // reserved stock mode never sends RF
  assert(!m.command("get ota.direct",0,reply));
#endif
}
'''


class CompanionDirectAdminGuardTest(unittest.TestCase):
    def test_guard_and_production_companion_wiring(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("a host C++17 compiler is required")
        source = (ROOT / "examples/companion_radio/MyMesh.cpp").read_text()
        binary = extract_braced(source, "if (cmd_frame[0] == CMD_SEND_PATH_DISCOVERY_REQ\n             && len == 2 + PUB_KEY_SIZE").replace("#endif", "")
        getter = extract_braced(source, 'if (sender_timestamp == 0 && !strcmp(command, "get ota.direct"))')
        callbacks = "\n".join(extract_braced(source, signature) for signature in (
            "uint8_t MyMesh::getContactTxRadio(", "void MyMesh::onPeerDataRecv(", "bool MyMesh::onPeerPathRecv("))
        raw = extract_braced(source, "if (pkt->getPayloadType() == PAYLOAD_TYPE_OTA\n            && _ota_direct_admin.active")
        self.assertIn("_ota_direct_admin.beginLogin(recipient->id", source)
        self.assertIn("&& len >= 2 + PUB_KEY_SIZE && cmd_frame[1] == 0)", source)
        mesh = (ROOT / "src/Mesh.cpp").read_text()
        start = mesh.index("Packet* Mesh::createPathReturn(const uint8_t* dest_hash,")
        end = mesh.index("\nPacket* Mesh::createDatagram(", start)
        stub = STUB.replace("@PATH_BUILDER@", mesh[start:end])
        stub = stub.replace("#define PAYLOAD_TYPE_PATH 8", "#define PAYLOAD_TYPE_PATH 8\n#define PAYLOAD_TYPE_OTA 12")
        stub = stub.replace("bool direct=true;", "uint8_t payload_type=0;uint8_t getPayloadType()const{return payload_type;}\n  bool direct=true;")
        with tempfile.TemporaryDirectory(prefix="meshcore-companion-direct-") as folder:
            folder = Path(folder)
            (folder / "Mesh.h").write_text(stub)
            for full in (0, 1):
                code = HARNESS.replace("@BINARY@", binary).replace("@GETTER@", getter)
                code = code.replace("@RAW_ROUTE@", raw)
                code = code.replace("@CALLBACKS@", callbacks if full else "")
                (folder / "test.cpp").write_text(code)
                executable = folder / "guard.exe"
                result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                         f"-DFULL={full}", "-I" + str(folder), "-I" + str(ROOT / "src"),
                                         str(folder / "test.cpp"), "-o", str(executable)],
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
