#include <helpers/wifi/SerialWifiInterface.h>
#include <helpers/TxtDataHelpers.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>

uint32_t mock_millis = 100;
std::deque<WiFiClient> WiFiServer::incoming;
@CONSTANTS@
#define MAX_CONTACTS 350
#define MAX_GROUP_CHANNELS 40
#define FIRMWARE_VERSION "v1.17.1.9-preview3"
#define FIRMWARE_BUILD_DATE "13 Oct 2026"
struct { const char* getManufacturerName() const { return "Station G2"; } } board;
@COPY@

struct ActualDeviceQuery {
  uint8_t app_target_ver = 0;
  uint32_t _active_ble_pin = 123456;
  struct { bool isRepeatEn() const { return false; } uint8_t path_hash_mode = 2;
           uint32_t ble_pin = 0; } _prefs;
  BaseSerialInterface* _serial;
  void dispatch(uint8_t* cmd_frame, size_t len) {
    uint8_t out_frame[MAX_FRAME_SIZE] = {};
    @DEVICE_QUERY@
  }
};

int main() {
  WiFiServer::resetDiagnostics();
  SerialWifiInterface transport;
  assert(transport.begin(5000)); transport.enable();
  auto socket = std::make_shared<MockSocket>(IPAddress(192, 168, 1, 20));
  socket->write_limit = std::numeric_limits<size_t>::max();
  WiFiServer::incoming.push_back(WiFiClient(socket));
  uint8_t input[MAX_FRAME_SIZE] = {};
  assert(transport.checkRecvFrame(input) == 0 && transport.isConnected());
  ActualDeviceQuery mesh; mesh._serial = &transport;
  for (int i = 0; i < 2; ++i) {
    const uint8_t push[]{0x88, uint8_t(i)};
    assert(transport.writeFrame(push, sizeof(push)) == sizeof(push));
  }
  socket->received = {'<', 2, 0, CMD_DEVICE_QUERY, 3};
  int dispatched_at = -1, reply_at = -1;
  for (int poll = 0; poll < 100; ++poll) {
    const uint8_t push[]{0x88, uint8_t(poll + 2)};
    transport.writeFrame(push, sizeof(push));
    const size_t received = transport.checkRecvFrame(input);
    if (received) {
      assert(dispatched_at < 0 && received == 2 && input[0] == CMD_DEVICE_QUERY);
      dispatched_at = poll;
      mesh.dispatch(input, received);
    }
    for (size_t pos = 0; pos + 3 <= socket->sent.size();) {
      assert(socket->sent[pos] == '>');
      const size_t size = size_t(socket->sent[pos + 1]) | size_t(socket->sent[pos + 2]) << 8;
      assert(pos + 3 + size <= socket->sent.size());
      const uint8_t* payload = socket->sent.data() + pos + 3;
      if (payload[0] == RESP_CODE_DEVICE_INFO) {
        if (reply_at < 0) reply_at = poll;
        assert(size == 82 && payload[1] == FIRMWARE_VER_CODE);
        assert(payload[2] == MAX_CONTACTS / 2 && payload[3] == MAX_GROUP_CHANNELS);
        assert(payload[4] == 0 && payload[5] == 0 && payload[6] == 0 && payload[7] == 0);
        assert(strcmp(reinterpret_cast<const char*>(payload + 20), "Station G2") == 0);
        assert(payload[80] == 0 && payload[81] == 2);
      }
      pos += size + 3;
    }
    ++mock_millis;
  }
  assert(dispatched_at == 0 && reply_at == 1 && mesh.app_target_ver == 3);
  assert(transport.getStatus().validFrames == 1 && socket->send_calls == 100);
  assert(socket->sdk_write_calls == 0 && socket->connected);
  std::puts("PASS: actual DEVICE_QUERY reply under 100 radio-push polls");
}
