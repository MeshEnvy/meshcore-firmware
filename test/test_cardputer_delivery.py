"""Execute shared Companion delivery and keyboard transport routing."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from cpp_source import body

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "examples/companion_radio/MyMesh.cpp"

HARNESS = r'''
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <helpers/CompanionChannelAlias.h>
using std::isfinite;
#define MESH_ENABLE_ONE_KEY_DM 1
#define ONE_KEY_DM_SHARED_OFFLINE_QUEUE 0
#define COMPANION_FEATURE_NOTIFICATIONS 0
#define COMPANION_FEATURE_TEXT_TERMINAL 0
#define MAX_FRAME_SIZE 176
#define MAX_GROUP_CHANNELS 40
#define MAX_TEXT_LEN 160
#define PRV_KEY_SIZE 64
#define PUB_KEY_SIZE 32
#define ONE_KEY_DM_ID_SIZE 8
#define TXT_TYPE_PLAIN 0
#define TXT_TYPE_CLI_DATA 1
#define TXT_TYPE_SIGNED_PLAIN 2
#define RESP_CODE_CONTACT_MSG_RECV_V3 0x93
#define RESP_CODE_CONTACT_MSG_RECV 0x82
#define RESP_CODE_CHANNEL_MSG_RECV_V3 0x90
#define RESP_CODE_CHANNEL_MSG_RECV 0x83
#define PUSH_CODE_MSG_WAITING 0x80
static unsigned long millis() { return 1234; }
enum class UIEventType { contactMessage, channelMessage };
namespace mesh {
struct Packet {
  uint8_t path_len = 0;
  float getSNR() const { return 2.0f; }
  bool isRouteFlood() const { return true; }
};
struct GroupChannel { uint8_t secret[32]{}; uint8_t tx_radio = 0; };
}
struct ContactInfo { struct { uint8_t pub_key[32]{}; } id; char name[32] = "Alice"; };
struct ChannelDetails { char name[32]{}; mesh::GroupChannel channel; };
struct Serial {
  bool connected = false;
  bool isConnected() const { return connected; }
  bool isReplyRouteAvailable(Serial*) { return true; }
  void writeFrame(const uint8_t*, int) {}
  void writeFrameToRoute(Serial*, const uint8_t*, int) {}
};
struct UI {
  int received = 0, sounds = 0;
  void setMessageNotificationOverride(bool, bool = false) {}
  void newMsg(uint8_t, const char*, const char*, int, int, const char*, int) { ++received; }
  void notify(UIEventType) { ++sounds; }
};
struct MyMesh {
  Serial serial;
  UI ui;
  Serial* _serial = &serial;
  UI* _ui = &ui;
  int app_target_ver = 3, offline_queue_len = 0;
  uint8_t out_frame[MAX_FRAME_SIZE]{};
  bool accept = true, delivered = false;
  int contact_history = 0, channel_history = 0;
  Serial* private_key_backup_route = nullptr;
  unsigned long private_key_backup_deadline = 0;
  uint8_t private_key_backup_sender[6]{};
  char private_key_backup_nonce[17]{};
  struct Held { uint32_t timestamp; char text[160]; uint8_t id[8], sender_key[32]; } held_dms[1]{};
  uint8_t held_dm_count = 0;
  struct { char node_name[32] = "Cardputer"; } _prefs;
  ChannelDetails configured;
  bool radio2 = true, mt_available = true, transport_accept = true;
  int meshtext_sends = 0, meshcore_sends = 0;
  uint8_t sent_key = 0, sent_radio = 0;
  bool getChannel(int index, ChannelDetails& out) {
    if (index != 9 || !configured.name[0]) return false;
    out = configured; return true;
  }
  bool isAliasRadio2Defined() const { return radio2; }
  bool isMeshtextChannel(const ChannelDetails& c) const {
    return mt_available && parseCompanionChannelAlias(c.name).kind == CompanionChannelKind::Meshtext;
  }
  bool sendMeshtextText(const ChannelDetails& c, const char*, size_t) {
    ++meshtext_sends; sent_key = c.channel.secret[0]; return transport_accept;
  }
  bool sendGroupMessage(uint32_t, mesh::GroupChannel& c, const char*, const char*, size_t) {
    ++meshcore_sends; sent_key = c.secret[0]; sent_radio = c.tx_radio; return transport_accept;
  }
  bool millisHasNowPassed(unsigned long) const { return false; }
  bool addToOfflineQueue(const uint8_t*, int) { if (accept) ++offline_queue_len; return accept; }
  void onUiContactMessage(const ContactInfo&, const char*) { ++contact_history; }
  void onUiChannelMessage(const uint8_t[32], const char*) { ++channel_history; }
  void makeOneKeyDMId(uint8_t* out, uint32_t, const char*) { memset(out, 7, 8); }
  bool wasDeliveredOneKeyDM(const uint8_t*, const uint8_t*) const { return delivered; }
  void rememberDeliveredOneKeyDM(const uint8_t*, const uint8_t*) { delivered = true; }
  int getOfflineQueueCapacity() const { return 256; }
  void markConnectionActive(const ContactInfo&) {}
  void scheduleContactWrite(const ContactInfo&) {}
  void onMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*);
  bool queueMessage(const ContactInfo&, uint8_t, mesh::Packet*, uint32_t,
                    const uint8_t*, int, const char*, bool = false, uint32_t = 0);
  bool queueChannelText(uint8_t, const uint8_t[32], float, uint8_t, uint32_t,
                        const char*, bool, bool = true);
  bool sendUiChannelMessage(int, uint32_t, const char*, size_t);
};
@METHODS@
int main() {
  MyMesh m;
  ContactInfo from;
  mesh::Packet packet;
  m.onMessageRecv(from, &packet, 100, "hello");
  assert(m.offline_queue_len == 1);
#if CARDPUTER_ADV_UI
  assert(m.contact_history == 1 && m.ui.received == 1 && m.ui.sounds == 1);
#else
  assert(m.contact_history == 0 && m.ui.received == 0 && m.ui.sounds == 0);
#endif
  // Intro/held-message duplicate suppression must happen before local history.
  const int history_before_duplicate = m.contact_history;
  const int ui_before_duplicate = m.ui.received;
  m.delivered = true;
  m.onMessageRecv(from, &packet, 100, "hello");
  assert(m.offline_queue_len == 1);
  assert(m.contact_history == history_before_duplicate && m.ui.received == ui_before_duplicate);
  m.delivered = false;
  m.accept = false;
  int prior = m.contact_history;
  assert(!m.queueMessage(from, TXT_TYPE_SIGNED_PLAIN, &packet, 101, nullptr, 0, "room"));
  assert(m.contact_history == prior); // Server must retry an unretained room post.
  m.accept = true;
  assert(m.queueMessage(from, TXT_TYPE_SIGNED_PLAIN, &packet, 101, nullptr, 0, "room"));
#if CARDPUTER_ADV_UI
  assert(m.contact_history == prior + 1);
#endif
  prior = m.contact_history;
  assert(!m.queueMessage(from, TXT_TYPE_CLI_DATA, &packet, 101, nullptr, 0, "KEYBACKUP secret"));
  assert(m.contact_history == prior); // No private export retained locally.
  assert(m.queueMessage(from, TXT_TYPE_CLI_DATA, &packet, 101, nullptr, 0, "normal admin reply"));
  assert(m.contact_history == prior); // Admin replies are not chat history.
  uint8_t key[32] = {0x42};
  m.accept = false;
  assert(!m.queueChannelText(9, key, 1, 0, 101, "channel", false));
  assert(m.channel_history == 0);
  m.accept = true;
  assert(m.queueChannelText(9, key, 1, 0, 101, "channel", false));
#if CARDPUTER_ADV_UI
  assert(m.channel_history == 1);
  // Meshtext enters this same queue once after its transport-level dedup.
  assert(m.queueChannelText(9, key, 1, 0, 101, "Meshtext", false));
  assert(m.channel_history == 2);
  strcpy(m.configured.name, "#mt-longfast");
  m.configured.channel.secret[0] = 71;
  m.configured.channel.tx_radio = 3; // Native MeshCore TX disabled for this alias.
#if MESH_ENABLE_MESHTEXT_CHAT
  assert(m.sendUiChannelMessage(9, 101, "text", 4));
  assert(m.meshtext_sends == 1 && m.meshcore_sends == 0 && m.sent_key == 71);
  m.transport_accept = false;
  assert(!m.sendUiChannelMessage(9, 101, "text", 4));
  assert(m.meshcore_sends == 0); // Rejection never leaks onto the other protocol.
  m.transport_accept = true;
#else
  assert(!m.sendUiChannelMessage(9, 101, "text", 4));
  assert(m.meshcore_sends == 0);
#endif
  m.mt_available = false;
  assert(!m.sendUiChannelMessage(9, 101, "text", 4));
  strcpy(m.configured.name, "#r2-testing");
  m.configured.channel.secret[0] = 72;
  m.configured.channel.tx_radio = 1;
  assert(m.sendUiChannelMessage(9, 101, "text", 4));
  assert(m.sent_key == 72 && m.sent_radio == 1);
  // Resolve the current slot every time; never transmit using a stale snapshot.
  m.configured.channel.secret[0] = 73;
  assert(m.sendUiChannelMessage(9, 101, "text", 4));
  assert(m.sent_key == 73);
  int sends = m.meshcore_sends;
  assert(!m.sendUiChannelMessage(1, 101, "text", 4));
  assert(!m.sendUiChannelMessage(9, 101, nullptr, 4));
  assert(!m.sendUiChannelMessage(9, 101, "", 0));
  assert(!m.sendUiChannelMessage(9, 101, "text", 3));
  std::string oversized(160, 'x');
  assert(!m.sendUiChannelMessage(9, 101, oversized.c_str(), oversized.size()));
  assert(m.meshcore_sends == sends);
#else
  assert(m.channel_history == 0 && m.ui.received == 0);
#endif
}
'''


class CardputerDeliveryTests(unittest.TestCase):
    def run_cpp(self, cardputer=True, meshtext=True):
        source = SOURCE.read_text()
        signatures = [
            "void MyMesh::onMessageRecv(",
            "bool MyMesh::queueMessage(",
            "bool MyMesh::queueChannelText(",
        ]
        if cardputer:
            signatures.append("bool MyMesh::sendUiChannelMessage(")
        code = HARNESS.replace("@METHODS@", "\n".join(body(source, sig) for sig in signatures))
        with tempfile.TemporaryDirectory(prefix="meshcore-cardputer-delivery-") as temp:
            binary = Path(temp) / "delivery"
            built = subprocess.run([
                os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
                f"-DCARDPUTER_ADV_UI={int(cardputer)}", f"-DMESH_ENABLE_MESHTEXT_CHAT={int(meshtext)}",
                "-I", str(ROOT / "src"), "-x", "c++", "-", "-o", str(binary),
            ], input=code, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_cardputer_shared_delivery_and_meshtext(self):
        self.run_cpp()

    def test_missing_meshtext_transport_cannot_leak_native_packets(self):
        self.run_cpp(meshtext=False)

    def test_headless_delivery_keeps_its_existing_behavior(self):
        self.run_cpp(cardputer=False)

    def test_keyboard_transport_does_not_depend_on_text_terminal(self):
        source = re.sub(r"^\s*#\s*include[^\n]*", "", SOURCE.read_text(), flags=re.MULTILINE)
        result = subprocess.run([
            os.environ.get("CXX", "c++"), "-E", "-P", "-x", "c++", "-",
            "-DCARDPUTER_ADV_UI=1", "-DCOMPANION_FEATURE_TEXT_TERMINAL=0",
        ], input=source, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("bool MyMesh::sendUiChannelMessage(", result.stdout)

    def test_cardputer_color_traits_without_standard_ui(self):
        for enabled in (0, 1):
            code = '#include <helpers/ui/DisplayBuildFlags.h>\n'
            code += '#ifdef MESHCORE_HAS_COLOR_DISPLAY\nconstexpr bool color=true;\n#else\nconstexpr bool color=false;\n#endif\n'
            code += '#ifdef MESHCORE_HAS_REAL_DISPLAY\nconstexpr bool real=true;\n#else\nconstexpr bool real=false;\n#endif\n'
            code += f'static_assert(color == {enabled}); static_assert(real == {enabled});\nint main() {{}}\n'
            with tempfile.TemporaryDirectory(prefix="meshcore-cardputer-traits-") as temp:
                result = subprocess.run([
                    os.environ.get("CXX", "c++"), "-std=c++17", f"-DCARDPUTER_ADV_UI={enabled}",
                    "-I", str(ROOT / "src"), "-x", "c++", "-", "-o", str(Path(temp) / "traits"),
                ], input=code, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_fresh_message_wake_and_saved_display_choices(self):
        from test_display_night_mode import compile_run
        compile_run(r'''
#include <helpers/ui/DisplayPowerSettings.h>
#include <helpers/ui/DisplayNightMode.h>
#include <cassert>
using namespace mesh::ui;
int main() {
  FakeFS fs;
  char reply[160];
  assert(!loadDisplayPowerSettings(&fs, true, true));
  assert(displayPowerPrefs().battery.mode == DisplayMode::Automatic);
  assert(displayPowerPrefs().usb.mode == DisplayMode::Automatic);
  assert(displayNightMode() == NightAuto && displayNightDelayMinutes() == 60);
  assert(handleDisplayPowerCommand("set display.mode off", reply, sizeof(reply)));
  assert(!strcmp(reply, "OK"));
  assert(handleDisplayPowerCommand("set display.usb.mode button", reply, sizeof(reply)));
  assert(!strcmp(reply, "OK"));
  assert(handleDisplayPowerCommand("set display.timeout 47", reply, sizeof(reply)));
  assert(!strcmp(reply, "OK"));
  assert(handleDisplayPowerCommand("set display.night on", reply, sizeof(reply)));
  assert(!strcmp(reply, "OK"));
  assert(loadDisplayPowerSettings(&fs, true, true));
  assert(displayPowerPrefs().battery.mode == DisplayMode::Off);
  assert(displayPowerPrefs().usb.mode == DisplayMode::Button);
  assert(displayPowerPrefs().battery.seconds == 47);
  assert(displayNightMode() == NightOn && displayNightActive());
}
''', files=("src/helpers/ui/DisplayPowerSettings.cpp", "src/helpers/ui/DisplayNightMode.cpp"),
                    defines=("-DCARDPUTER_ADV_UI=1",))


if __name__ == "__main__":
    unittest.main()
