#include "MyMesh.h"
#include <algorithm>

#include <helpers/lomessage/Split.h>

#ifdef ESP32
#include <WiFi.h>
#include <cctype>
#include <helpers/esp32/LotatoCliCtx.h>
#include <helpers/esp32/LotatoDebug.h>
#include <helpers/esp32/LotatoIngestTtl.h>
#include <helpers/esp32/LotatoNodeStore.h>
#include <helpers/esp32/LotatoSerialCli.h>
#include <helpers/locommand/Command.h>
#include <helpers/locommand/Router.h>
#include <helpers/lofi/Lofi.h>
#include <helpers/lomessage/Buffer.h>
#include <losettings/ConfigHub.h>
#endif

#ifndef LOTATO_DBG_LN
#define LOTATO_DBG_LN(...) ((void)0)
#endif

namespace {
/** `Mesh::onRecvPacket` decrypts into `uint8_t[MAX_PACKET_PAYLOAD]`; `data[len]` is only safe if len < MAX. */
inline void null_terminate_mesh_data(uint8_t* data, size_t len) {
  if (!data) return;
  if (len < (size_t)MAX_PACKET_PAYLOAD) {
    data[len] = 0;
  } else if ((size_t)MAX_PACKET_PAYLOAD > 0) {
    data[MAX_PACKET_PAYLOAD - 1] = 0;
  }
}
}  // namespace

/** Admin TXT_MSG path: `Mesh::onRecvPacket` already has `data[MAX_PACKET_PAYLOAD]` on the stack — keep reply
 *  scratch out of that frame to avoid blowing the loop stack (manifests as random Guru / flash-cache faults). */
static uint8_t s_on_peer_mesh_cli_temp[5 + MyMesh::kCliReplyCap];

#ifdef ESP32

MyMesh* g_mesh_for_scan = nullptr;

static char s_on_peer_mesh_cli_snap[MyMesh::kCliReplyCap];

/** True while a long-running async CLI op holds the session; mesh commands are rejected while set. */
static bool s_async_cli_busy = false;
/** Routing snapshot set at command-receive time; persists for async completion push. */
static struct {
  bool    valid;
  int     acl_idx;
  uint8_t out_path[MAX_PATH_SIZE];
  uint8_t out_path_len;
  uint8_t path_hash_size;
} s_scan_reply_target = {};

void my_mesh_set_async_cli_busy(bool busy) { s_async_cli_busy = busy; }

extern "C" void lofi_async_busy(bool busy) { s_async_cli_busy = busy; }

namespace {
bool s_scan_from_serial = false;
bool s_connect_from_serial = false;

void scan_complete_cb(void*, const char* text) {
  if (s_scan_reply_target.valid && g_mesh_for_scan) {
    g_mesh_for_scan->enqueueTxtCliReply(
      s_scan_reply_target.acl_idx, s_scan_reply_target.out_path_len, s_scan_reply_target.out_path,
      s_scan_reply_target.path_hash_size, 0, text);
    s_scan_reply_target.valid = false;
  }
  if (s_scan_from_serial) {
    s_scan_from_serial = false;
    lotato_serial_print_mesh_cli_reply(text);
  }
}

void connect_complete_cb(void*, bool ok, const char* detail) {
  char msg[96];
  if (ok) {
    snprintf(msg, sizeof(msg), "OK - WiFi connected (%s)", detail ? detail : "");
  } else {
    snprintf(msg, sizeof(msg), "Err - WiFi connect failed (%s)", detail ? detail : "?");
  }
  if (s_scan_reply_target.valid && g_mesh_for_scan) {
    g_mesh_for_scan->enqueueTxtCliReply(
      s_scan_reply_target.acl_idx, s_scan_reply_target.out_path_len, s_scan_reply_target.out_path,
      s_scan_reply_target.path_hash_size, 0, msg);
    s_scan_reply_target.valid = false;
  }
  if (s_connect_from_serial) {
    s_connect_from_serial = false;
    lotato_serial_print_mesh_cli_reply(msg);
  }
}
}  // namespace
#endif

/* ------------------------------ Config -------------------------------- */

#ifndef LORA_FREQ
  #define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
  #define LORA_BW 250
#endif
#ifndef LORA_SF
  #define LORA_SF 10
#endif
#ifndef LORA_CR
  #define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER 20
#endif

#ifndef ADVERT_NAME
  #define ADVERT_NAME "Lotato repeater"
#endif
#ifndef ADVERT_LAT
  #define ADVERT_LAT 0.0
#endif
#ifndef ADVERT_LON
  #define ADVERT_LON 0.0
#endif

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY 300
#endif

#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY 200
#endif

#define FIRMWARE_VER_LEVEL       2

#define REQ_TYPE_GET_STATUS         0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05
#define REQ_TYPE_GET_NEIGHBOURS     0x06
#define REQ_TYPE_GET_OWNER_INFO     0x07     // FIRMWARE_VER_LEVEL >= 2

#define RESP_SERVER_LOGIN_OK        0 // response to ANON_REQ

#define ANON_REQ_TYPE_REGIONS      0x01
#define ANON_REQ_TYPE_OWNER        0x02
#define ANON_REQ_TYPE_BASIC        0x03   // just remote clock

#define CLI_REPLY_DELAY_MILLIS      600

#define LAZY_CONTACTS_WRITE_DELAY    5000

void MyMesh::putNeighbour(const mesh::Identity &id, uint32_t timestamp, float snr) {
#if MAX_NEIGHBOURS // check if neighbours enabled
  // find existing neighbour, else use least recently updated
  uint32_t oldest_timestamp = 0xFFFFFFFF;
  NeighbourInfo *neighbour = &neighbours[0];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    // if neighbour already known, we should update it
    if (id.matches(neighbours[i].id)) {
      neighbour = &neighbours[i];
      break;
    }

    // otherwise we should update the least recently updated neighbour
    if (neighbours[i].heard_timestamp < oldest_timestamp) {
      neighbour = &neighbours[i];
      oldest_timestamp = neighbour->heard_timestamp;
    }
  }

  // update neighbour info
  neighbour->id = id;
  neighbour->advert_timestamp = timestamp;
  neighbour->heard_timestamp = getRTCClock()->getCurrentTime();
  neighbour->snr = (int8_t)(snr * 4);
#endif
}

uint8_t MyMesh::handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood) {
  ClientInfo* client = NULL;
  if (data[0] == 0) {   // blank password, just check if sender is in ACL
    client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    if (client == NULL) {
    #if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Login, sender not in ACL");
    #endif
    }
  }
  if (client == NULL) {
    uint8_t perms;
    if (strcmp((char *)data, _prefs.password) == 0) { // check for valid admin password
      perms = PERM_ACL_ADMIN;
    } else if (strcmp((char *)data, _prefs.guest_password) == 0) { // check guest password
      perms = PERM_ACL_GUEST;
    } else {
#if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Invalid password: %s", data);
#endif
      return 0;
    }

    client = acl.putClient(sender, 0);  // add to contacts (if not already known)
    if (sender_timestamp <= client->last_timestamp) {
      MESH_DEBUG_PRINTLN("Possible login replay attack!");
      return 0;  // FATAL: client table is full -OR- replay attack
    }

    MESH_DEBUG_PRINTLN("Login success!");
    client->last_timestamp = sender_timestamp;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->permissions &= ~0x03;
    client->permissions |= perms;
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

    if (perms != PERM_ACL_GUEST) {   // keep number of FS writes to a minimum
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    }
  }

  if (is_flood) {
    client->out_path_len = OUT_PATH_UNKNOWN;  // need to rediscover out_path
  }

  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  reply_data[4] = RESP_SERVER_LOGIN_OK;
  reply_data[5] = 0;  // Legacy: was recommended keep-alive interval (secs / 16)
  reply_data[6] = client->isAdmin() ? 1 : 0;
  reply_data[7] = client->permissions;
  getRNG()->random(&reply_data[8], 4);   // random blob to help packet-hash uniqueness
  reply_data[12] = FIRMWARE_VER_LEVEL;  // New field

  return 13;  // reply length
}

uint8_t MyMesh::handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data & 63;
    reply_path_hash_size = (*data >> 6) + 1;
    data++;

    memcpy(reply_path, data, ((uint8_t)reply_path_len) * reply_path_hash_size);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)

    return 8 + region_map.exportNamesTo((char *) &reply_data[8], sizeof(reply_data) - 12, REGION_DENY_FLOOD);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data & 63;
    reply_path_hash_size = (*data >> 6) + 1;
    data++;

    memcpy(reply_path, data, ((uint8_t)reply_path_len) * reply_path_hash_size);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    sprintf((char *) &reply_data[8], "%s\n%s", _prefs.node_name, _prefs.owner_info);

    return 8 + strlen((char *) &reply_data[8]);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data & 63;
    reply_path_hash_size = (*data >> 6) + 1;
    data++;

    memcpy(reply_path, data, ((uint8_t)reply_path_len) * reply_path_hash_size);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    reply_data[8] = 0;  // features
#ifdef WITH_RS232_BRIDGE
    reply_data[8] |= 0x01;  // is bridge, type UART
#elif WITH_ESPNOW_BRIDGE
    reply_data[8] |= 0x03;  // is bridge, type ESP-NOW
#endif
    if (_prefs.disable_fwd) {   // is this repeater currently disabled
      reply_data[8] |= 0x80;  // is disabled
    }
    // TODO:  add some kind of moving-window utilisation metric, so can query 'how busy' is this repeater
    return 9;   // reply length
  }
  return 0;
}

int MyMesh::handleRequest(ClientInfo *sender, uint32_t sender_timestamp, uint8_t *payload, size_t payload_len) {
  // uint32_t now = getRTCClock()->getCurrentTimeUnique();
  // memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  memcpy(reply_data, &sender_timestamp, 4); // reflect sender_timestamp back in response packet (kind of like a 'tag')

  if (payload[0] == REQ_TYPE_GET_STATUS) {  // guests can also access this now
    RepeaterStats stats;
    stats.batt_milli_volts = board.getBattMilliVolts();
    stats.curr_tx_queue_len = _mgr->getOutboundTotal();
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv = radio_driver.getPacketsRecv();
    stats.n_packets_sent = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs = uptime_millis / 1000;
    stats.n_sent_flood = getNumSentFlood();
    stats.n_sent_direct = getNumSentDirect();
    stats.n_recv_flood = getNumRecvFlood();
    stats.n_recv_direct = getNumRecvDirect();
    stats.err_events = _err_flags;
    stats.last_snr = (int16_t)(radio_driver.getLastSNR() * 4);
    stats.n_direct_dups = ((SimpleMeshTables *)getTables())->getNumDirectDups();
    stats.n_flood_dups = ((SimpleMeshTables *)getTables())->getNumFloodDups();
    stats.total_rx_air_time_secs = getReceiveAirTime() / 1000;
    stats.n_recv_errors = radio_driver.getPacketsRecvErrors();
    memcpy(&reply_data[4], &stats, sizeof(stats));

    return 4 + sizeof(stats); //  reply_len
  }
  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t perm_mask = ~(payload[1]); // NEW: first reserved byte (of 4), is now inverse mask to apply to permissions

    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);

    // query other sensors -- target specific
    if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
      perm_mask = 0x00;  // just base telemetry allowed
    }
    sensors.querySensors(perm_mask, telemetry);

	// This default temperature will be overridden by external sensors (if any)
    float temperature = board.getMCUTemperature();
    if(!isnan(temperature)) { // Supported boards with built-in temperature sensor. ESP32-C3 may return NAN
      telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature); // Built-in MCU Temperature
    }

    uint8_t tlen = telemetry.getSize();
    memcpy(&reply_data[4], telemetry.getBuffer(), tlen);
    return 4 + tlen; // reply_len
  }
  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    uint8_t res1 = payload[1];   // reserved for future  (extra query params)
    uint8_t res2 = payload[2];
    if (res1 == 0 && res2 == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < acl.getNumClients() && ofs + 7 <= sizeof(reply_data) - 4; i++) {
        auto c = acl.getClientByIdx(i);
        if (c->permissions == 0) continue;  // skip deleted entries
        memcpy(&reply_data[ofs], c->id.pub_key, 6); ofs += 6;  // just 6-byte pub_key prefix
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }
  if (payload[0] == REQ_TYPE_GET_NEIGHBOURS) {
    uint8_t request_version = payload[1];
    if (request_version == 0) {

      // reply data offset (after response sender_timestamp/tag)
      int reply_offset = 4;

      // get request params
      uint8_t count = payload[2]; // how many neighbours to fetch (0-255)
      uint16_t offset;
      memcpy(&offset, &payload[3], 2); // offset from start of neighbours list (0-65535)
      uint8_t order_by = payload[5]; // how to order neighbours. 0=newest_to_oldest, 1=oldest_to_newest, 2=strongest_to_weakest, 3=weakest_to_strongest
      uint8_t pubkey_prefix_length = payload[6]; // how many bytes of neighbour pub key we want
      // we also send a 4 byte random blob in payload[7...10] to help packet uniqueness

      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS count=%d, offset=%d, order_by=%d, pubkey_prefix_length=%d", count, offset, order_by, pubkey_prefix_length);

      // clamp pub key prefix length to max pub key length
      if(pubkey_prefix_length > PUB_KEY_SIZE){
        pubkey_prefix_length = PUB_KEY_SIZE;
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS invalid pubkey_prefix_length=%d clamping to %d", pubkey_prefix_length, PUB_KEY_SIZE);
      }

      // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
      int16_t neighbours_count = 0;
#if MAX_NEIGHBOURS
      NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
      for (int i = 0; i < MAX_NEIGHBOURS; i++) {
        auto neighbour = &neighbours[i];
        if (neighbour->heard_timestamp > 0) {
          sorted_neighbours[neighbours_count] = neighbour;
          neighbours_count++;
        }
      }

      // sort neighbours based on order
      if (order_by == 0) {
        // sort by newest to oldest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting newest to oldest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp > b->heard_timestamp; // desc
        });
      } else if (order_by == 1) {
        // sort by oldest to newest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting oldest to newest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp < b->heard_timestamp; // asc
        });
      } else if (order_by == 2) {
        // sort by strongest to weakest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting strongest to weakest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr > b->snr; // desc
        });
      } else if (order_by == 3) {
        // sort by weakest to strongest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting weakest to strongest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr < b->snr; // asc
        });
      }
#endif

      // build results buffer
      int results_count = 0;
      int results_offset = 0;
      uint8_t results_buffer[130];
      for(int index = 0; index < count && index + offset < neighbours_count; index++){
        
        // stop if we can't fit another entry in results
        int entry_size = pubkey_prefix_length + 4 + 1;
        if(results_offset + entry_size > sizeof(results_buffer)){
          MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS no more entries can fit in results buffer");
          break;
        }

#if MAX_NEIGHBOURS
        // add next neighbour to results
        auto neighbour = sorted_neighbours[index + offset];
        uint32_t heard_seconds_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
        memcpy(&results_buffer[results_offset], neighbour->id.pub_key, pubkey_prefix_length); results_offset += pubkey_prefix_length;
        memcpy(&results_buffer[results_offset], &heard_seconds_ago, 4); results_offset += 4;
        memcpy(&results_buffer[results_offset], &neighbour->snr, 1); results_offset += 1;
        results_count++;
#endif

      }

      // build reply
      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS neighbours_count=%d results_count=%d", neighbours_count, results_count);
      memcpy(&reply_data[reply_offset], &neighbours_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_buffer, results_offset); reply_offset += results_offset;

      return reply_offset;
    }
  } else if (payload[0] == REQ_TYPE_GET_OWNER_INFO) {
    sprintf((char *) &reply_data[4], "%s\n%s\n%s", FIRMWARE_VERSION, _prefs.node_name, _prefs.owner_info);
    return 4 + strlen((char *) &reply_data[4]);
  }
  return 0; // unknown command
}

mesh::Packet *MyMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = _cli.buildAdvertData(ADV_TYPE_REPEATER, app_data);

  return createAdvert(self_id, app_data, app_data_len);
}

File MyMesh::openAppend(const char *fname) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(fname, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(fname, "a");
#else
  return _fs->open(fname, "a", true);
#endif
}

static uint8_t max_loop_minimal[] =  { 0, /* 1-byte */  4, /* 2-byte */  2, /* 3-byte */  1 };
static uint8_t max_loop_moderate[] = { 0, /* 1-byte */  2, /* 2-byte */  1, /* 3-byte */  1 };
static uint8_t max_loop_strict[] =   { 0, /* 1-byte */  1, /* 2-byte */  1, /* 3-byte */  1 };

bool MyMesh::isLooped(const mesh::Packet* packet, const uint8_t max_counters[]) {
  uint8_t hash_size = packet->getPathHashSize();
  uint8_t hash_count = packet->getPathHashCount();
  uint8_t n = 0;
  const uint8_t* path = packet->path;
  while (hash_count > 0) {      // count how many times this node is already in the path
    if (self_id.isHashMatch(path, hash_size)) n++;
    hash_count--;
    path += hash_size;
  }
  return n >= max_counters[hash_size];
}

bool MyMesh::allowPacketForward(const mesh::Packet *packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood() && packet->getPathHashCount() >= _prefs.flood_max) return false;
  if (packet->isRouteFlood() && recv_pkt_region == NULL) {
    MESH_DEBUG_PRINTLN("allowPacketForward: unknown transport code, or wildcard not allowed for FLOOD packet");
    return false;
  }
  if (packet->isRouteFlood() && _prefs.loop_detect != LOOP_DETECT_OFF) {
    const uint8_t* maximums;
    if (_prefs.loop_detect == LOOP_DETECT_MINIMAL) {
      maximums = max_loop_minimal;
    } else if (_prefs.loop_detect == LOOP_DETECT_MODERATE) {
      maximums = max_loop_moderate;
    } else {
      maximums = max_loop_strict;
    }
    if (isLooped(packet, maximums)) {
      MESH_DEBUG_PRINTLN("allowPacketForward: FLOOD packet loop detected!");
      return false;
    }
  }
  return true;
}

const char *MyMesh::getLogDateTime() {
  static char tmp[32];
  uint32_t now = getRTCClock()->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d - %d/%d/%d U", dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(),
          dt.year());
  return tmp;
}

void MyMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
#if MESH_PACKET_LOGGING
  Serial.print(getLogDateTime());
  Serial.print(" RAW: ");
  mesh::Utils::printHex(Serial, raw, len);
  Serial.println();
#endif
}

void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 1) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": RX, len=%d (type=%d, route=%s, payload_len=%d) SNR=%d RSSI=%d score=%d", len,
               pkt->getPayloadType(), pkt->isRouteDirect() ? "D" : "F", pkt->payload_len,
               (int)_radio->getLastSNR(), (int)_radio->getLastRSSI(), (int)(score * 1000));

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTx(mesh::Packet *pkt, int len) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 0) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX, len=%d (type=%d, route=%s, payload_len=%d)", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTxFail(mesh::Packet *pkt, int len) {
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX FAIL!, len=%d (type=%d, route=%s, payload_len=%d)\n", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);
      f.close();
    }
  }
}

int MyMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

uint32_t MyMesh::getRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}
uint32_t MyMesh::getDirectRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.direct_tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}

bool MyMesh::filterRecvFloodPacket(mesh::Packet* pkt) {
  // just try to determine region for packet (apply later in allowPacketForward())
  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) {
      recv_pkt_region = NULL;
    } else {
      recv_pkt_region =  &region_map.getWildcard();
    }
  } else {
    recv_pkt_region = NULL;
  }
  // do normal processing
  return false;
}

void MyMesh::onAnonDataRecv(mesh::Packet *packet, const uint8_t *secret, const mesh::Identity &sender,
                            uint8_t *data, size_t len) {
  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) { // received an initial request by a possible admin
                                                           // client (unknown at this stage)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    null_terminate_mesh_data(data, len);
    uint8_t reply_len;

    reply_path_len = -1;
    if (data[4] == 0 || data[4] >= ' ') {   // is password, ie. a login request
      reply_len = handleLoginReq(sender, secret, timestamp, &data[4], packet->isRouteFlood());
    } else if (data[4] == ANON_REQ_TYPE_REGIONS && packet->isRouteDirect()) {
      reply_len = handleAnonRegionsReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_OWNER && packet->isRouteDirect()) {
      reply_len = handleAnonOwnerReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_BASIC && packet->isRouteDirect()) {
      reply_len = handleAnonClockReq(sender, timestamp, &data[5]);
    } else {
      reply_len = 0;  // unknown/invalid request type
    }

    if (reply_len == 0) return;   // invalid request

    if (packet->isRouteFlood()) {
      // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
      mesh::Packet* path = createPathReturn(sender, secret, packet->path, packet->path_len,
                                            PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
      if (path) sendFlood(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    } else if (reply_path_len < 0) {
      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
      if (reply) sendFlood(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    } else {
      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
      uint8_t path_len = ((reply_path_hash_size - 1) << 6) | (reply_path_len & 63);
      if (reply) sendDirect(reply, reply_path,  path_len, SERVER_RESPONSE_DELAY);
    }
  }
}

int MyMesh::searchPeersByHash(const uint8_t *hash) {
  int n = 0;
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i; // store the INDEXES of matching contacts (for subsequent 'peer' methods)
    }
  }
  return n;
}

void MyMesh::getPeerSharedSecret(uint8_t *dest_secret, int peer_idx) {
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    // lookup pre-calculated shared_secret
    memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
  } else {
    MESH_DEBUG_PRINTLN("getPeerSharedSecret: Invalid peer idx: %d", i);
  }
}

static bool isShare(const mesh::Packet *packet) {
  if (packet->hasTransportCodes()) {
    return packet->transport_codes[0] == 0 && packet->transport_codes[1] == 0;  // codes { 0, 0 } means 'send to nowhere'
  }
  return false;
}

void MyMesh::onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                          const uint8_t *app_data, size_t app_data_len) {
  mesh::Mesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len); // chain to super impl

  AdvertDataParser parser(app_data, app_data_len);

  // if this a zero hop advert (and not via 'Share'), add it to neighbours
  if (packet->path_len == 0 && !isShare(packet)) {
    if (parser.isValid() && parser.getType() == ADV_TYPE_REPEATER) { // just keep neigbouring Repeaters
      putNeighbour(id, timestamp, packet->getSNR());
    }
  }

#ifdef ESP32
  // persist all valid adverts (any hop count) and POST when cooloff has elapsed
  if (parser.isValid()) {
    int32_t lat = parser.hasLatLon() ? parser.getIntLat() : 0;
    int32_t lon = parser.hasLatLon() ? parser.getIntLon() : 0;
    const char* name = parser.hasName() ? parser.getName() : "";
    uint8_t atype = parser.getType();
    char id_hex[9]; // first 4 bytes as hex
    static const char* hexd = "0123456789abcdef";
    for (int _i = 0; _i < 4; _i++) {
      id_hex[_i*2]   = hexd[id.pub_key[_i] >> 4];
      id_hex[_i*2+1] = hexd[id.pub_key[_i] & 0xf];
    }
    id_hex[8] = '\0';
    LOTATO_DBG_LN("advert: !%s name=\"%.32s\" type=%u hops=%u ts=%lu gps=%s",
                        id_hex, name, (unsigned)atype, (unsigned)packet->path_len,
                        (unsigned long)timestamp, parser.hasLatLon() ? "yes" : "no");
    int slot = _node_store.put(id.pub_key, name, atype, timestamp, lat, lon);
    LOTATO_DBG_LN("advert: !%s stored slot=%d (total=%d)", id_hex, slot, _node_store.count());
  }
#endif
}

void MyMesh::onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx, const uint8_t *secret,
                            uint8_t *data, size_t len) {
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("onPeerDataRecv: invalid peer idx: %d", i);
    return;
  }
  ClientInfo* client = acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_REQ) { // request (from a Known admin client!)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    if (timestamp > client->last_timestamp) { // prevent replay attacks
      int reply_len = handleRequest(client, timestamp, &data[4], len - 4);
      if (reply_len == 0) return; // invalid command

      client->last_timestamp = timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      if (packet->isRouteFlood()) {
        // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
        mesh::Packet *path = createPathReturn(client->id, secret, packet->path, packet->path_len,
                                              PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
        if (path) sendFlood(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      } else {
        mesh::Packet *reply =
            createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
        if (reply) {
          if (client->out_path_len != OUT_PATH_UNKNOWN) { // we have an out_path, so send DIRECT
            sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
          } else {
            sendFlood(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  } else if (type == PAYLOAD_TYPE_TXT_MSG && len > 5 && client->isAdmin()) { // a CLI command
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4); // timestamp (by sender's RTC clock - which could be wrong)
    uint8_t flags = (data[4] >> 2);        // message attempt number, and other flags

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: unsupported text type received: flags=%02x", (uint32_t)flags);
    } else if (sender_timestamp >= client->last_timestamp) { // prevent replay attacks
      bool is_retry = (sender_timestamp == client->last_timestamp);
      client->last_timestamp = sender_timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      // len can be > original length, but 'text' will be padded with zeroes
      null_terminate_mesh_data(data, len);
#ifdef ESP32
      lotato_dbg_mesh_txt_rx(sender_timestamp, data[4], flags, len, is_retry ? 1 : 0, (const char*)&data[5]);
#endif

      if (flags == TXT_TYPE_PLAIN) { // for legacy CLI, send Acks
        uint32_t ack_hash; // calc truncated hash of the message timestamp + text + sender pub_key, to prove
                           // to sender that we got it
        mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 5 + strlen((char *)&data[5]), client->id.pub_key,
                            PUB_KEY_SIZE);

        mesh::Packet *ack = createAck(ack_hash);
        if (ack) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFlood(ack, TXT_ACK_DELAY, packet->getPathHashSize());
          } else {
            sendDirect(ack, client->out_path, client->out_path_len, TXT_ACK_DELAY);
          }
        }
      }

      char *command = (char *)&data[5];
      char *reply = (char *)&s_on_peer_mesh_cli_temp[5];
      if (is_retry) {
        *reply = 0;
      } else {
#ifdef ESP32
        if (s_async_cli_busy) {
          strncpy(reply, "Err - busy (operation in progress)", MyMesh::kCliReplyCap - 1);
          reply[MyMesh::kCliReplyCap - 1] = '\0';
          LOTATO_DBG_LN("cli reply: reject (busy) cmd=%.60s", command);
        } else {
          // preset routing snapshot so async ops (e.g. wifi list) can push results later
          s_scan_reply_target.valid          = true;
          s_scan_reply_target.acl_idx        = i;
          s_scan_reply_target.out_path_len   = client->out_path_len;
          memcpy(s_scan_reply_target.out_path, client->out_path, sizeof(s_scan_reply_target.out_path));
          s_scan_reply_target.path_hash_size = packet->getPathHashSize();
          _lotato_txt_route.valid            = true;
          _lotato_txt_route.acl_idx          = i;
          _lotato_txt_route.out_path_len     = client->out_path_len;
          memcpy(_lotato_txt_route.out_path, client->out_path, sizeof(_lotato_txt_route.out_path));
          _lotato_txt_route.path_hash_size   = packet->getPathHashSize();
          strncpy(s_on_peer_mesh_cli_snap, command, sizeof(s_on_peer_mesh_cli_snap) - 1);
          s_on_peer_mesh_cli_snap[sizeof(s_on_peer_mesh_cli_snap) - 1] = '\0';
          handleCommand(sender_timestamp, command, reply);
          // if an async op started it set s_async_cli_busy; leave snapshot valid for completion push
          if (!s_async_cli_busy) s_scan_reply_target.valid = false;
          _lotato_txt_route.valid = false;
          lotato_dbg_trace_cli_exchange("mesh", s_on_peer_mesh_cli_snap, reply);
        }
#else
        handleCommand(sender_timestamp, command, reply);
#endif
      }
#ifdef ESP32
      {
        int will_q = (!is_retry && reply[0] != '\0') ? 1 : 0;
        lotato_dbg_mesh_after_handle(reply, will_q);
      }
#endif
      if (strlen(reply) > 0) {
#ifdef ESP32
        lotato_dbg_mesh_enqueue_short(reply);
#endif
        enqueueTxtCliReply(i, client->out_path_len, client->out_path,
                           packet->getPathHashSize(), sender_timestamp, reply);
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  }
}

bool MyMesh::onPeerPathRecv(mesh::Packet *packet, int sender_idx, const uint8_t *secret, uint8_t *path,
                            uint8_t path_len, uint8_t extra_type, uint8_t *extra, uint8_t extra_len) {
  // TODO: prevent replay attacks
  int i = matching_peer_indexes[sender_idx];

  if (i >= 0 && i < acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("PATH to client, path_len=%d", (uint32_t)path_len);
    auto client = acl.getClientByIdx(i);

    // store a copy of path, for sendDirect()
    client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len);
    client->last_activity = getRTCClock()->getCurrentTime();
  } else {
    MESH_DEBUG_PRINTLN("onPeerPathRecv: invalid peer idx: %d", i);
  }

  // NOTE: no reciprocal path send!!
  return false;
}

#define CTL_TYPE_NODE_DISCOVER_REQ   0x80
#define CTL_TYPE_NODE_DISCOVER_RESP  0x90

void MyMesh::onControlDataRecv(mesh::Packet* packet) {
  uint8_t type = packet->payload[0] & 0xF0;    // just test upper 4 bits
  if (type == CTL_TYPE_NODE_DISCOVER_REQ && packet->payload_len >= 6
      && !_prefs.disable_fwd && discover_limiter.allow(rtc_clock.getCurrentTime())
  ) {
    int i = 1;
    uint8_t  filter = packet->payload[i++];
    uint32_t tag;
    memcpy(&tag, &packet->payload[i], 4); i += 4;
    uint32_t since;
    if (packet->payload_len >= i+4) {   // optional since field
      memcpy(&since, &packet->payload[i], 4); i += 4;
    } else {
      since = 0;
    }

    if ((filter & (1 << ADV_TYPE_REPEATER)) != 0 && _prefs.discovery_mod_timestamp >= since) {
      bool prefix_only = packet->payload[0] & 1;
      uint8_t data[6 + PUB_KEY_SIZE];
      data[0] = CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_REPEATER;   // low 4-bits for node type
      data[1] = packet->_snr;   // let sender know the inbound SNR ( x 4)
      memcpy(&data[2], &tag, 4);     // include tag from request, for client to match to
      memcpy(&data[6], self_id.pub_key, PUB_KEY_SIZE);
      auto resp = createControlData(data, prefix_only ? 6 + 8 : 6 + PUB_KEY_SIZE);
      if (resp) {
        sendZeroHop(resp, getRetransmitDelay(resp)*4);  // apply random delay (widened x4), as multiple nodes can respond to this
      }
    }
  } else if (type == CTL_TYPE_NODE_DISCOVER_RESP && packet->payload_len >= 6) {
    uint8_t node_type = packet->payload[0] & 0x0F;
    if (node_type != ADV_TYPE_REPEATER) {
      return;
    }
    if (packet->payload_len < 6 + PUB_KEY_SIZE) {
      MESH_DEBUG_PRINTLN("onControlDataRecv: DISCOVER_RESP pubkey too short: %d", (uint32_t)packet->payload_len);
      return;
    }

    if (pending_discover_tag == 0 || millisHasNowPassed(pending_discover_until)) {
      pending_discover_tag = 0;
      return;
    }
    uint32_t tag;
    memcpy(&tag, &packet->payload[2], 4);
    if (tag != pending_discover_tag) {
      return;
    }

    mesh::Identity id(&packet->payload[6]);
    if (id.matches(self_id)) {
      return;
    }
    putNeighbour(id, rtc_clock.getCurrentTime(), packet->getSNR());
  }
}

void MyMesh::sendNodeDiscoverReq() {
  uint8_t data[10];
  data[0] = CTL_TYPE_NODE_DISCOVER_REQ; // prefix_only=0
  data[1] = (1 << ADV_TYPE_REPEATER);
  getRNG()->random(&data[2], 4); // tag
  memcpy(&pending_discover_tag, &data[2], 4);
  pending_discover_until = futureMillis(60000);
  uint32_t since = 0;
  memcpy(&data[6], &since, 4);

  auto pkt = createControlData(data, sizeof(data));
  if (pkt) {
    sendZeroHop(pkt);
  }
}

MyMesh::MyMesh(mesh::MainBoard &board, mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng,
               mesh::RTCClock &rtc, mesh::MeshTables &tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      _cli(board, rtc, sensors, acl, &_prefs, this), telemetry(MAX_PACKET_PAYLOAD - 4), region_map(key_store), temp_map(key_store),
      discover_limiter(4, 120),  // max 4 every 2 minutes
      anon_limiter(4, 180)   // max 4 every 3 minutes
#if defined(WITH_RS232_BRIDGE)
      , bridge(&_prefs, WITH_RS232_BRIDGE, _mgr, &rtc)
#endif
#if defined(WITH_ESPNOW_BRIDGE)
      , bridge(&_prefs, _mgr, &rtc)
#endif
{
  last_millis = 0;
  uptime_millis = 0;
  next_local_advert = next_flood_advert = 0;
  dirty_contacts_expiry = 0;
  set_radio_at = revert_radio_at = 0;
  _logging = false;
  region_load_active = false;
#ifdef ESP32
  _lotato_txt_route.valid = false;
#endif

#if MAX_NEIGHBOURS
  memset(neighbours, 0, sizeof(neighbours));
#endif

  // defaults
  memset(&_prefs, 0, sizeof(_prefs));
  _prefs.airtime_factor = 1.0;
  _prefs.rx_delay_base = 0.0f;   // turn off by default, was 10.0;
  _prefs.tx_delay_factor = 0.5f; // was 0.25f
  _prefs.direct_tx_delay_factor = 0.3f; // was 0.2
  StrHelper::strncpy(_prefs.node_name, ADVERT_NAME, sizeof(_prefs.node_name));
  _prefs.node_lat = ADVERT_LAT;
  _prefs.node_lon = ADVERT_LON;
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.advert_interval = 1;        // default to 2 minutes for NEW installs
  _prefs.flood_advert_interval = 12; // 12 hours
  _prefs.flood_max = 64;
  _prefs.interference_threshold = 0; // disabled

  // bridge defaults
  _prefs.bridge_enabled = 1;    // enabled
  _prefs.bridge_delay   = 500;  // milliseconds
  _prefs.bridge_pkt_src = 0;    // logTx
  _prefs.bridge_baud = 115200;  // baud rate
  _prefs.bridge_channel = 1;    // channel 1

  StrHelper::strncpy(_prefs.bridge_secret, "LVSITANOS", sizeof(_prefs.bridge_secret));

  // GPS defaults
  _prefs.gps_enabled = 0;
  _prefs.gps_interval = 0;
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS;

  _prefs.adc_multiplier = 0.0f; // 0.0f means use default board multiplier

#if defined(USE_SX1262) || defined(USE_SX1268)
#ifdef SX126X_RX_BOOSTED_GAIN
  _prefs.rx_boosted_gain = SX126X_RX_BOOSTED_GAIN;
#else
  _prefs.rx_boosted_gain = 1; // enabled by default;
#endif
#endif

  pending_discover_tag = 0;
  pending_discover_until = 0;
}

void MyMesh::begin(FILESYSTEM *fs) {
  mesh::Mesh::begin();
  _fs = fs;
  // load persisted prefs
  _cli.loadPrefs(_fs);
  acl.load(_fs, self_id);
#ifdef ESP32
  LotatoConfig::instance().load();
  lofi::Lofi::instance().begin();
  lotato_ingest_ttl_store().begin();
  _node_store.begin(_fs);
  g_mesh_for_scan = this;
  lofi::Lofi::instance().setScanCompleteCallback(scan_complete_cb, nullptr);
  lofi::Lofi::instance().setConnectCompleteCallback(connect_complete_cb, nullptr);

  losettings::ConfigHub::instance().bindConfigCli(_cli_config);
  _cli_config.setRootBrief("LoSettings keys (ls/get/set/unset)");

  lofi::bindWifiCli(_cli_wifi);
  _cli_wifi.setRootBrief("WiFi STA scan/connect");

  _cli_lotato.add("status", &MyMesh::lotato_h_status, nullptr, nullptr, "show lotato/ingest status");
  _cli_lotato.add("pause", &MyMesh::lotato_h_pause, nullptr, nullptr, "pause ingest (shortcut for config)");
  _cli_lotato.add("resume", &MyMesh::lotato_h_resume, nullptr, nullptr, "resume ingest (shortcut for config)");
  _cli_lotato.add("contacts", &MyMesh::lotato_h_contacts, nullptr, nullptr, "node store visibility summary");
  _cli_lotato.add("flush", &MyMesh::lotato_h_flush, nullptr, nullptr, "clear last-post for visible nodes");
  _cli_lotato.setRootBrief("ingest status / flush / contacts");

  _cli_router.clear();
  _cli_router.add(&_cli_lotato);
  _cli_router.add(&_cli_wifi);
  _cli_router.add(&_cli_config);
#endif
  // TODO: key_store.begin();
  region_map.load(_fs);

#if defined(WITH_BRIDGE)
  if (_prefs.bridge_enabled) {
    bridge.begin();
  }
#endif

  radio_set_params(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_set_tx_power(_prefs.tx_power_dbm);

  radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);
  MESH_DEBUG_PRINTLN("RX Boosted Gain Mode: %s",
                     radio_driver.getRxBoostedGainMode() ? "Enabled" : "Disabled");

  updateAdvertTimer();
  updateFloodAdvertTimer();

  board.setAdcMultiplier(_prefs.adc_multiplier);

#if ENV_INCLUDE_GPS == 1
  applyGpsPrefs();
#endif
}

void MyMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) {
  set_radio_at = futureMillis(2000); // give CLI reply some time to be sent back, before applying temp radio params
  pending_freq = freq;
  pending_bw = bw;
  pending_sf = sf;
  pending_cr = cr;

  revert_radio_at = futureMillis(2000 + timeout_mins * 60 * 1000); // schedule when to revert radio params
}

bool MyMesh::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return LittleFS.format();
#else
#error "need to implement file system erase"
  return false;
#endif
}

void MyMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  mesh::Packet *pkt = createSelfAdvert();
  if (pkt) {
    if (flood) {
      sendFlood(pkt, delay_millis, _prefs.path_hash_mode + 1);
    } else {
      sendZeroHop(pkt, delay_millis);
    }
  } else {
    MESH_DEBUG_PRINTLN("ERROR: unable to create advertisement packet!");
  }
}

void MyMesh::updateAdvertTimer() {
  if (_prefs.advert_interval > 0) { // schedule local advert timer
    next_local_advert = futureMillis(((uint32_t)_prefs.advert_interval) * 2 * 60 * 1000);
  } else {
    next_local_advert = 0; // stop the timer
  }
}

void MyMesh::updateFloodAdvertTimer() {
  if (_prefs.flood_advert_interval > 0) { // schedule flood advert timer
    next_flood_advert = futureMillis(((uint32_t)_prefs.flood_advert_interval) * 60 * 60 * 1000);
  } else {
    next_flood_advert = 0; // stop the timer
  }
}

void MyMesh::dumpLogFile() {
#if defined(RP2040_PLATFORM)
  File f = _fs->open(PACKET_LOG_FILE, "r");
#else
  File f = _fs->open(PACKET_LOG_FILE);
#endif
  if (f) {
    while (f.available()) {
      int c = f.read();
      if (c < 0) break;
      Serial.print((char)c);
    }
    f.close();
  }
}

void MyMesh::setTxPower(int8_t power_dbm) {
  radio_set_tx_power(power_dbm);
}

#if defined(USE_SX1262) || defined(USE_SX1268)
void MyMesh::setRxBoostedGain(bool enable) {
  radio_driver.setRxBoostedGainMode(enable);
}
#endif

void MyMesh::formatNeighborsReply(char *reply) {
  char *dp = reply;

#if MAX_NEIGHBOURS
  // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
  int16_t neighbours_count = 0;
  NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    auto neighbour = &neighbours[i];
    if (neighbour->heard_timestamp > 0) {
      sorted_neighbours[neighbours_count] = neighbour;
      neighbours_count++;
    }
  }

  // sort neighbours newest to oldest
  std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
    return a->heard_timestamp > b->heard_timestamp; // desc
  });

  for (int i = 0; i < neighbours_count && dp - reply < 134; i++) {
    NeighbourInfo *neighbour = sorted_neighbours[i];

    // add new line if not first item
    if (i > 0) *dp++ = '\n';

    char hex[10];
    // get 4 bytes of neighbour id as hex
    mesh::Utils::toHex(hex, neighbour->id.pub_key, 4);

    // add next neighbour
    uint32_t secs_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
    sprintf(dp, "%s:%d:%d", hex, secs_ago, neighbour->snr);
    while (*dp)
      dp++; // find end of string
  }
#endif
  if (dp == reply) { // no neighbours, need empty response
    strcpy(dp, "-none-");
    dp += 6;
  }
  *dp = 0; // null terminator
}

void MyMesh::removeNeighbor(const uint8_t *pubkey, int key_len) {
#if MAX_NEIGHBOURS
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    NeighbourInfo *neighbour = &neighbours[i];
    if (memcmp(neighbour->id.pub_key, pubkey, key_len) == 0) {
      neighbours[i] = NeighbourInfo(); // clear neighbour entry
    }
  }
#endif
}

void MyMesh::formatStatsReply(char *reply) {
  StatsFormatHelper::formatCoreStats(reply, board, *_ms, _err_flags, _mgr);
#ifdef ESP32
  // Append lotato node count to the JSON object.
  int len = strlen(reply);
  if (len > 1 && reply[len - 1] == '}') {
    snprintf(reply + len - 1, 48, ",\"lotato_nodes\":%d}", _node_store.count());
  }
#endif
}

void MyMesh::formatRadioStatsReply(char *reply) {
  StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver, getTotalAirTime(), getReceiveAirTime());
}

void MyMesh::formatPacketStatsReply(char *reply) {
  StatsFormatHelper::formatPacketStats(reply, radio_driver, getNumSentFlood(), getNumSentDirect(), 
                                       getNumRecvFlood(), getNumRecvDirect());
}

void MyMesh::saveIdentity(const mesh::LocalIdentity &new_id) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif
  store.save("_main", new_id);
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
}

void MyMesh::handleCommand(uint32_t sender_timestamp, char *command, char *reply) {
  if (region_load_active) {
#ifdef ESP32
    // During `region load`, non-blank lines are consumed as region map rows with no echo.
    // Allow `lotato …` through so e.g. `lotato flush` still gets a normal reply.
    const char *cmd_scan = command;
    while (*cmd_scan == ' ') cmd_scan++;
    if (!this->_cli_router.matchesAnyRoot(cmd_scan) && !this->_cli_router.matchesGlobalHelp(cmd_scan))
#endif
    {
      if (StrHelper::isBlank(command)) {  // empty/blank line, signal to terminate 'load' operation
        region_map = temp_map;  // copy over the temp instance as new current map
        region_load_active = false;

        sprintf(reply, "OK - loaded %d regions", region_map.getCount());
      } else {
        char *np = command;
        while (*np == ' ') np++;   // skip indent
        int indent = np - command;

        char *ep = np;
        while (RegionMap::is_name_char(*ep)) ep++;
        if (*ep) { *ep++ = 0; }  // set null terminator for end of name

        while (*ep && *ep != 'F') ep++;  // look for (optional) flags

        if (indent > 0 && indent < 8 && strlen(np) > 0) {
          auto parent = load_stack[indent - 1];
          if (parent) {
            auto old = region_map.findByName(np);
            auto nw = temp_map.putRegion(np, parent->id, old ? old->id : 0);  // carry-over the current ID (if name already exists)
            if (nw) {
              nw->flags = old ? old->flags : (*ep == 'F' ? 0 : REGION_DENY_FLOOD);   // carry-over flags from curr

              load_stack[indent] = nw;  // keep pointers to parent regions, to resolve parent_id's
            }
          }
        }
        reply[0] = 0;
      }
      return;
    }
  }

  while (*command == ' ') command++; // skip leading spaces

  if (strlen(command) > 4 && command[2] == '|') { // optional prefix (for companion radio CLI)
    memcpy(reply, command, 3);                    // reflect the prefix back
    reply += 3;
    command += 3;
  }

  // handle ACL related commands
  if (memcmp(command, "setperm ", 8) == 0) {   // format:  setperm {pubkey-hex} {permissions-int8}
    char* hex = &command[8];
    char* sp = strchr(hex, ' ');   // look for separator char
    if (sp == NULL) {
      strcpy(reply, "Err - bad params");
    } else {
      *sp++ = 0;   // replace space with null terminator

      uint8_t pubkey[PUB_KEY_SIZE];
      int hex_len = min(sp - hex, PUB_KEY_SIZE*2);
      if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
        uint8_t perms = atoi(sp);
        if (acl.applyPermissions(self_id, pubkey, hex_len / 2, perms)) {
          dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);   // trigger acl.save()
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - invalid params");
        }
      } else {
        strcpy(reply, "Err - bad pubkey");
      }
    }
  } else if (sender_timestamp == 0 && strcmp(command, "get acl") == 0) {
    Serial.println("ACL:");
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->permissions == 0) continue;  // skip deleted (or guest) entries

      Serial.printf("%02X ", c->permissions);
      mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
      Serial.printf("\n");
    }
    reply[0] = 0;
  } else if (memcmp(command, "region", 6) == 0) {
    reply[0] = 0;

    const char* parts[4];
    int n = mesh::Utils::parseTextParts(command, parts, 4, ' ');
    if (n == 1) {
      region_map.exportTo(reply, 160);
    } else if (n >= 2 && strcmp(parts[1], "load") == 0) {
      temp_map.resetFrom(region_map);   // rebuild regions in a temp instance
      memset(load_stack, 0, sizeof(load_stack));
      load_stack[0] = &temp_map.getWildcard();
      region_load_active = true;
    } else if (n >= 2 && strcmp(parts[1], "save") == 0) {
      _prefs.discovery_mod_timestamp = rtc_clock.getCurrentTime();   // this node is now 'modified' (for discovery info)
      savePrefs();
      bool success = region_map.save(_fs);
      strcpy(reply, success ? "OK" : "Err - save failed");
    } else if (n >= 3 && strcmp(parts[1], "allowf") == 0) {
      auto region = region_map.findByNamePrefix(parts[2]);
      if (region) {
        region->flags &= ~REGION_DENY_FLOOD;
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "Err - unknown region");
      }
    } else if (n >= 3 && strcmp(parts[1], "denyf") == 0) {
      auto region = region_map.findByNamePrefix(parts[2]);
      if (region) {
        region->flags |= REGION_DENY_FLOOD;
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "Err - unknown region");
      }
    } else if (n >= 3 && strcmp(parts[1], "get") == 0) {
      auto region = region_map.findByNamePrefix(parts[2]);
      if (region) {
        auto parent = region_map.findById(region->parent);
        if (parent && parent->id != 0) {
          sprintf(reply, " %s (%s) %s", region->name, parent->name, (region->flags & REGION_DENY_FLOOD) ? "" : "F");
        } else {
          sprintf(reply, " %s %s", region->name, (region->flags & REGION_DENY_FLOOD) ? "" : "F");
        }
      } else {
        strcpy(reply, "Err - unknown region");
      }
    } else if (n >= 3 && strcmp(parts[1], "home") == 0) {
      auto home = region_map.findByNamePrefix(parts[2]);
      if (home) {
        region_map.setHomeRegion(home);
        sprintf(reply, " home is now %s", home->name);
      } else {
        strcpy(reply, "Err - unknown region");
      }
    } else if (n == 2 && strcmp(parts[1], "home") == 0) {
      auto home = region_map.getHomeRegion();
      sprintf(reply, " home is %s", home ? home->name : "*");
    } else if (n >= 3 && strcmp(parts[1], "put") == 0) {
      auto parent = n >= 4 ? region_map.findByNamePrefix(parts[3]) : &region_map.getWildcard();
      if (parent == NULL) {
        strcpy(reply, "Err - unknown parent");
      } else {
        auto region = region_map.putRegion(parts[2], parent->id);
        if (region == NULL) {
          strcpy(reply, "Err - unable to put");
        } else {
          strcpy(reply, "OK");
        }
      }
    } else if (n >= 3 && strcmp(parts[1], "remove") == 0) {
      auto region = region_map.findByName(parts[2]);
      if (region) {
        if (region_map.removeRegion(*region)) {
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - not empty");
        }
      } else {
        strcpy(reply, "Err - not found");
      }
    } else if (n >= 3 && strcmp(parts[1], "list") == 0) {
      uint8_t mask = 0;
      bool invert = false;
      
      if (strcmp(parts[2], "allowed") == 0) {
        mask = REGION_DENY_FLOOD;
        invert = false;  // list regions that DON'T have DENY flag
      } else if (strcmp(parts[2], "denied") == 0) {
        mask = REGION_DENY_FLOOD;
        invert = true;   // list regions that DO have DENY flag
      } else {
        strcpy(reply, "Err - use 'allowed' or 'denied'");
        return;
      }
      
      int len = region_map.exportNamesTo(reply, 160, mask, invert);
      if (len == 0) {
        strcpy(reply, "-none-");
      }
    } else {
      strcpy(reply, "Err - ??");
    }
  } else if (memcmp(command, "discover.neighbors", 18) == 0) {
    const char* sub = command + 18;
    while (*sub == ' ') sub++;
    if (*sub != 0) {
      strcpy(reply, "Err - discover.neighbors has no options");
    } else {
      sendNodeDiscoverReq();
      strcpy(reply, "OK - Discover sent");
    }
#ifdef ESP32
  } else if (_cli_router.matchesAnyRoot(command) || _cli_router.matchesGlobalHelp(command)) {
    LotatoCliCtx lctx{this, sender_timestamp};
    lomessage::Buffer buf(MyMesh::kCliReplyCap * 4);
    if (_cli_router.dispatch(command, buf, &lctx)) {
      lotato_dbg_lotato_dispatch_stats(buf.length(), buf.truncated() ? 1 : 0);
      deliverLotatoReply(sender_timestamp, buf.c_str(), reply);
    }
#endif
  } else{
    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands
  }
}

#ifdef ESP32

static void run_lotato_wifi_scan_cli(lomessage::Buffer& out, MyMesh* mesh, uint32_t sender_ts) {
  g_mesh_for_scan = mesh;
  lofi::Lofi& lf = lofi::Lofi::instance();
  lf.serviceWifiScan();
  if (lf.scanSnapshotCount() > 0 && !s_async_cli_busy) {
    lf.formatScanBody(out);
    return;
  }
  if (s_async_cli_busy) {
    out.append("WiFi scan in progress...");
    return;
  }
  s_scan_from_serial = (sender_ts == 0);
  lf.requestWifiScan();
  out.append("Scanning for WiFi devices...");
}

void MyMesh::lotato_h_status(locommand::Context& ctx) {
  auto* lc = static_cast<LotatoCliCtx*>(ctx.app_ctx);
  MyMesh* self = lc->self;
  LotatoConfig& cfg = LotatoConfig::instance();
  wl_status_t wl = WiFi.status();
  const char* wl_str = (wl == WL_CONNECTED) ? "connected" : "not connected";
  int code = self->_ingestor.lastHttpCode();
  char code_str[12];
  if (code == 0) strcpy(code_str, "none");
  else snprintf(code_str, sizeof(code_str), "%d", code);
  const char* token_str = cfg.apiToken()[0] ? "set" : "(none)";
  const char* url_str = cfg.ingestOrigin()[0] ? cfg.ingestOrigin() : "(none)";
  const char* dbg_str = cfg.debugEnabled() ? "on" : "off";
  const int due = self->_node_store.countDueNodes();
  if (wl == WL_CONNECTED) {
    ctx.out.appendf("WiFi: %s\nSSID: %s\nIP: %s\nNodes: %d\nDue: %d\nPaused: %s\nLast API Response: %s\nURL: %s\nToken: %s\nDebug: %s",
                    wl_str, WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                    self->_node_store.count(), due,
                    self->_ingestor.isPaused() ? "yes" : "no", code_str, url_str, token_str, dbg_str);
  } else {
    ctx.out.appendf("WiFi: %s\nSaved: %s\nNodes: %d\nDue: %d\nPaused: %s\nURL: %s\nToken: %s\nDebug: %s",
                    wl_str, cfg.ssid()[0] ? cfg.ssid() : "(none)",
                    self->_node_store.count(), due,
                    self->_ingestor.isPaused() ? "yes" : "no", url_str, token_str, dbg_str);
  }
}

void MyMesh::lotato_h_pause(locommand::Context& ctx) {
  auto* lc = static_cast<LotatoCliCtx*>(ctx.app_ctx);
  lc->self->_ingestor.setPaused(true);
  ctx.out.append("OK - ingest paused");
}

void MyMesh::lotato_h_resume(locommand::Context& ctx) {
  auto* lc = static_cast<LotatoCliCtx*>(ctx.app_ctx);
  lc->self->_ingestor.setPaused(false);
  ctx.out.append("OK - ingest resumed");
}

void MyMesh::lotato_h_contacts(locommand::Context& ctx) {
  auto* lc = static_cast<LotatoCliCtx*>(ctx.app_ctx);
  LotatoConfig& cfg = LotatoConfig::instance();
  ctx.out.appendf("Nodes: %d\nVisible: %d\nDue: %d\nRefresh: %lus\nVisibility: %lus\nGC: %lus\n",
                  lc->self->_node_store.count(), lc->self->_node_store.countVisibleNodes(),
                  lc->self->_node_store.countDueNodes(), (unsigned long)cfg.ingestRefreshSecs(),
                  (unsigned long)cfg.ingestVisibilitySecs(), (unsigned long)cfg.ingestGcStaleSecs());
}

void MyMesh::lotato_h_flush(locommand::Context& ctx) {
  auto* lc = static_cast<LotatoCliCtx*>(ctx.app_ctx);
  lc->self->_node_store.flushIngestTtlVisible();
  lc->self->_node_store.logFlushTargetsDebug();
  const int due = lc->self->_node_store.countDueNodes();
  LOTATO_DBG_LN("lotato CLI: flush — visible TTL cleared, due=%d", due);
  ctx.out.appendf("OK - due nodes: %d", due);
}

void MyMesh::lotato_h_wifi_status(locommand::Context& ctx) {
  LotatoConfig& cfg = LotatoConfig::instance();
  wl_status_t wl = WiFi.status();
  if (wl == WL_CONNECTED) {
    ctx.out.appendf("WiFi: connected\nSSID: %s\nIP: %s", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
  } else {
    ctx.out.appendf("WiFi: not connected\nSaved: %s\nUse: wifi scan",
                    cfg.ssid()[0] ? cfg.ssid() : "(none)");
  }
}

void MyMesh::lotato_h_wifi_scan(locommand::Context& ctx) {
  auto* lc = static_cast<LotatoCliCtx*>(ctx.app_ctx);
  run_lotato_wifi_scan_cli(ctx.out, lc->self, lc->sender_ts);
}

void MyMesh::lotato_h_wifi_connect(locommand::Context& ctx) {
  auto* lc = static_cast<LotatoCliCtx*>(ctx.app_ctx);
  LotatoConfig& cfg = LotatoConfig::instance();
  if (ctx.argc < 1) {
    ctx.printHelp();
    return;
  }
  const char* tok1 = ctx.argv[0];
  const char* tok2 = (ctx.argc >= 2) ? ctx.argv[1] : "";

  char ssid_to_use[33] = {};
  bool is_index = true;
  for (const char* q = tok1; *q; q++) {
    if (*q < '0' || *q > '9') {
      is_index = false;
      break;
    }
  }
  lofi::Lofi& lf = lofi::Lofi::instance();
  if (is_index && tok1[0] != '\0') {
    int idx = atoi(tok1) - 1;
    int32_t rssi;
    if (idx < 0 || !lf.scanSnapshotEntry(idx, ssid_to_use, &rssi)) {
      ctx.out.appendf("Err - index out of range (1..%d)\nRun: wifi scan first",
                      lf.scanSnapshotCount());
      return;
    }
  } else {
    strncpy(ssid_to_use, tok1, sizeof(ssid_to_use) - 1);
  }

  char pwd_to_use[65] = {};
  if (tok2[0] != '\0') {
    strncpy(pwd_to_use, tok2, sizeof(pwd_to_use) - 1);
    pwd_to_use[sizeof(pwd_to_use) - 1] = '\0';
  } else {
    cfg.getKnownWifiPassword(ssid_to_use, pwd_to_use, sizeof(pwd_to_use));
  }

  cfg.setWifi(ssid_to_use, pwd_to_use);
  lc->self->_ingestor.restartAfterConfigChange();
  s_connect_from_serial = (lc->sender_ts == 0);
  lf.beginConnect(ssid_to_use, pwd_to_use);
  LOTATO_DBG_LN("lotato CLI: wifi connecting ssid=%s modem_sleep=off", ssid_to_use);
  ctx.out.appendf("Connecting to %s...", ssid_to_use);
}

void MyMesh::lotato_h_wifi_forget(locommand::Context& ctx) {
  LotatoConfig& cfg = LotatoConfig::instance();
  if (ctx.argc < 1) {
    ctx.printHelp();
    return;
  }
  if (!cfg.forgetKnownWifi(ctx.argv[0])) {
    ctx.out.append("Err - SSID not in known list\n");
    return;
  }
  ctx.out.append("OK\n");
}

void MyMesh::deliverLotatoReply(uint32_t sender_ts, const char* text, char* reply) {
  reply[0] = '\0';
  if (!text || !text[0]) return;
  size_t n = strlen(text);
  if (n + 1 <= kCliReplyCap) {
    memcpy(reply, text, n + 1);
    return;
  }
  if (sender_ts != 0 && _lotato_txt_route.valid) {
    enqueueTxtCliReply(_lotato_txt_route.acl_idx, _lotato_txt_route.out_path_len, _lotato_txt_route.out_path,
                        _lotato_txt_route.path_hash_size, sender_ts, text);
    return;
  }
  lotato_serial_print_mesh_cli_reply(text);
}

namespace lofi {

static const locommand::ArgSpec k_wifi_connect_args[] = {
    {"n_or_ssid", "string", nullptr, true, "Scan index (1-based) or SSID"},
    {"password", "secret", nullptr, false, "PSK if not already saved"},
};

static const locommand::ArgSpec k_wifi_forget_args[] = {
    {"ssid", "string", nullptr, true, "Network SSID to remove from known list"},
};

void bindWifiCli(locommand::Engine& eng) {
  eng.add("status", &MyMesh::lotato_h_wifi_status, nullptr, nullptr, "STA / saved SSID snapshot");
  eng.add("scan", &MyMesh::lotato_h_wifi_scan, nullptr, nullptr, "scan for APs (async reply)");
  eng.addWithArgs("connect", &MyMesh::lotato_h_wifi_connect, k_wifi_connect_args, 2, nullptr, "connect by index or SSID");
  eng.addWithArgs("forget", &MyMesh::lotato_h_wifi_forget, k_wifi_forget_args, 1, nullptr, "remove SSID from known list");
}

}  // namespace lofi

#endif

/* ── CLI reply FIFO (lomessage::Queue) ─────────────────────────────── */

bool MyMesh::enqueueTxtCliReply(int acl_idx, uint8_t out_path_len, const uint8_t* out_path,
                                uint8_t path_hash_size, uint32_t sender_ts, const char* text) {
  CliReplyRoute ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.sender_ts      = sender_ts;
  ctx.acl_idx        = acl_idx;
  memcpy(ctx.out_path, out_path, sizeof(ctx.out_path));
  ctx.out_path_len   = out_path_len;
  ctx.path_hash_size = path_hash_size;

  lomessage::Options opts;
  opts.max_chunk            = kMaxTxtChunk;
  opts.inter_chunk_delay_ms = CLI_REPLY_DELAY_MILLIS;
  opts.split_flags          = lomessage::CHUNK_ABSORB_LINE_BOUNDARY;

  bool ok = _reply_queue.send(text, &ctx, sizeof(ctx), opts, millis());
  if (ok) {
    LOTATO_DBG_LN("cli reply q: enqueue len=%u acl=%d", (unsigned)strlen(text), acl_idx);
  } else {
    LOTATO_DBG_LN("cli reply q: enqueue FAILED (empty or OOM) acl=%d", acl_idx);
  }
  return ok;
}

lomessage::SendResult MyMesh::sendChunk(const uint8_t* data, size_t len,
                                        size_t chunk_idx, size_t total_chunks,
                                        bool /*is_final*/, void* user_ctx) {
  auto* ctx = static_cast<CliReplyRoute*>(user_ctx);
  if (!ctx || ctx->acl_idx < 0 || ctx->acl_idx >= acl.getNumClients()) {
    LOTATO_DBG_LN("cli reply q: drop stale acl=%d", ctx ? ctx->acl_idx : -1);
    return lomessage::SendResult::Abandon;
  }
  ClientInfo* client = acl.getClientByIdx(ctx->acl_idx);

  if (len == 0 || len > kMaxTxtChunk) {
    LOTATO_DBG_LN("cli reply tx: BAD emit_len=%u (max=%u) — abandon job", (unsigned)len,
                  (unsigned)kMaxTxtChunk);
    return lomessage::SendResult::Abandon;
  }

  uint8_t temp[5 + kMaxTxtChunk];
  uint32_t ts = getRTCClock()->getCurrentTimeUnique();
  if (ts == ctx->sender_ts) ts++;
  memcpy(temp, &ts, 4);
  temp[4] = (TXT_TYPE_CLI_DATA << 2);
  memcpy(&temp[5], data, len);

  uint8_t secret[PUB_KEY_SIZE];
  memcpy(secret, client->shared_secret, PUB_KEY_SIZE);

  mesh::Packet* pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + len);
  bool sent = false;
  if (pkt) {
    if (ctx->out_path_len == OUT_PATH_UNKNOWN) {
      sendFlood(pkt, 0, ctx->path_hash_size);
    } else {
      sendDirect(pkt, ctx->out_path, ctx->out_path_len, 0);
    }
    sent = true;
  }

  LOTATO_DBG_LN("cli reply tx: chunk %u/%u bytes=%u %s acl=%d peer=%02x%02x%02x%02x %s",
    (unsigned)chunk_idx, (unsigned)total_chunks, (unsigned)len,
    (ctx->out_path_len == OUT_PATH_UNKNOWN) ? "flood" : "direct",
    ctx->acl_idx,
    (unsigned)client->id.pub_key[0], (unsigned)client->id.pub_key[1],
    (unsigned)client->id.pub_key[2], (unsigned)client->id.pub_key[3],
    sent ? "ok" : "FAIL");
#ifdef ESP32
  lotato_dbg_cli_tx_chunk(ts, (unsigned)chunk_idx, (unsigned)total_chunks, len,
                          (ctx->out_path_len == OUT_PATH_UNKNOWN) ? "flood" : "direct", ctx->acl_idx, sent);
#endif
  return sent ? lomessage::SendResult::Sent : lomessage::SendResult::Retry;
}

/* ── end CLI reply FIFO ─────────────────────────────────────────────── */

void MyMesh::loop() {
#ifdef WITH_BRIDGE
  bridge.loop();
#endif

  mesh::Mesh::loop();
  _reply_queue.service(millis(), *this);

#ifdef ESP32
  lofi::Lofi::instance().serviceWifiScan();
  // Batch all due nodes from the store into one ingest POST (worker task).
  _ingestor.service(&_node_store, self_id.pub_key);
#endif

  if (next_flood_advert && millisHasNowPassed(next_flood_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    uint32_t delay_millis = 0;
    if (pkt) sendFlood(pkt, delay_millis, _prefs.path_hash_mode + 1);

    updateFloodAdvertTimer(); // schedule next flood advert
    updateAdvertTimer();      // also schedule local advert (so they don't overlap)
  } else if (next_local_advert && millisHasNowPassed(next_local_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendZeroHop(pkt);

    updateAdvertTimer(); // schedule next local advert
  }

  if (set_radio_at && millisHasNowPassed(set_radio_at)) { // apply pending (temporary) radio params
    set_radio_at = 0;                                     // clear timer
    radio_set_params(pending_freq, pending_bw, pending_sf, pending_cr);
    MESH_DEBUG_PRINTLN("Temp radio params");
  }

  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) { // revert radio params to orig
    revert_radio_at = 0;                                        // clear timer
    radio_set_params(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    MESH_DEBUG_PRINTLN("Radio params restored");
  }

  // is pending dirty contacts write needed?
  if (dirty_contacts_expiry && millisHasNowPassed(dirty_contacts_expiry)) {
    acl.save(_fs);
    dirty_contacts_expiry = 0;
  }

  // update uptime
  uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;
}

// To check if there is pending work
bool MyMesh::hasPendingWork() const {
#if defined(WITH_BRIDGE)
  if (bridge.isRunning()) return true;  // bridge needs WiFi radio, can't sleep
#endif
  if (!_reply_queue.empty()) return true;  // CLI reply chunks still queued
#ifdef ESP32
  if (_ingestor.pendingQueueDepth() > 0) return true;  // batch POST in flight / retry backoff
#endif
  return _mgr->getOutboundTotal() > 0;
}
