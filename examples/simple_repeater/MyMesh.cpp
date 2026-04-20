#include "MyMesh.h"
#include <algorithm>

#ifdef ESP32
#include <WiFi.h>
#include <cctype>
#include <helpers/esp32/LotatoDebug.h>
#include <helpers/esp32/LotatoNodeStore.h>

static bool lotato_cli_continuation(const char* after_lotato) {
  unsigned char c = static_cast<unsigned char>(after_lotato[0]);
  return after_lotato[0] == '\0' || isspace(c) != 0 || c == '?';
}

/** True while a long-running async CLI op holds the session; mesh commands are rejected while set. */
static bool s_async_cli_busy = false;
/** Scan timeout: abort if stuck in Scanning phase longer than this. */
static constexpr uint32_t kWifiScanTimeoutMs = 30000;
/** Routing snapshot set at command-receive time; persists for async completion push. */
static struct {
  bool    valid;
  int     acl_idx;
  uint8_t out_path[MAX_PATH_SIZE];
  uint8_t out_path_len;
  uint8_t path_hash_size;
} s_scan_reply_target = {};
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

    data[len] = 0;  // ensure null terminator
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
      data[len] = 0; // need to make a C string again, with null terminator

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

      uint8_t temp[5 + MyMesh::kCliReplyCap];
      char *command = (char *)&data[5];
      char *reply = (char *)&temp[5];
      if (is_retry) {
        *reply = 0;
      } else {
#ifdef ESP32
        if (s_async_cli_busy) {
          strncpy(reply, "Err - busy (operation in progress)", MyMesh::kCliReplyCap - 1);
          reply[MyMesh::kCliReplyCap - 1] = '\0';
          LOTATO_DBG_LN("cli reply: reject (busy) cmd=%.60s", command);
        } else {
          // preset routing snapshot so async ops (e.g. wifi scan) can push results later
          s_scan_reply_target.valid          = true;
          s_scan_reply_target.acl_idx        = i;
          s_scan_reply_target.out_path_len   = client->out_path_len;
          memcpy(s_scan_reply_target.out_path, client->out_path, sizeof(s_scan_reply_target.out_path));
          s_scan_reply_target.path_hash_size = packet->getPathHashSize();
          char mesh_cli_snap[MyMesh::kCliReplyCap];
          strncpy(mesh_cli_snap, command, sizeof(mesh_cli_snap) - 1);
          mesh_cli_snap[sizeof(mesh_cli_snap) - 1] = '\0';
          handleCommand(sender_timestamp, command, reply);
          // if an async op started it set s_async_cli_busy; leave snapshot valid for completion push
          if (!s_async_cli_busy) s_scan_reply_target.valid = false;
          lotato_dbg_trace_cli_exchange("mesh", mesh_cli_snap, reply);
        }
#else
        handleCommand(sender_timestamp, command, reply);
#endif
      }
      if (strlen(reply) > 0) {
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
  _cli_reply_root = _cli_reply_tip = nullptr;

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
  _node_store.begin(_fs);
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
  return SPIFFS.format();
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
    if (!(memcmp(cmd_scan, "lotato", 6) == 0 && lotato_cli_continuation(cmd_scan + 6)))
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
  } else if (memcmp(command, "lotato", 6) == 0 && lotato_cli_continuation(command + 6)) {
    handleLotaToCommand(command + 6, reply);
#endif
  } else{
    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands
  }
}

#ifdef ESP32

static void trim_trailing_ws(char* s) {
  if (!s) return;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
    s[--n] = '\0';
  }
}

static constexpr int kScanMax = 20;
static constexpr int kScanPageSize = 5;

struct ScanEntry { char ssid[33]; int32_t rssi; };
static ScanEntry s_scan[kScanMax];
static int s_scan_count = 0;

static void scan_rssi_bars(int32_t rssi, char out[7]) {
  int lv = rssi >= -50 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
  out[0] = '[';
  for (int b = 0; b < 4; b++) out[1 + b] = b < lv ? '|' : '.';
  out[5] = ']'; out[6] = '\0';
}

static void lotato_resume_sta_saved_credentials() {
  LotatoConfig& cfg = LotatoConfig::instance();
  if (cfg.ssid()[0] == '\0') return;
  const char* pw = cfg.password()[0] ? cfg.password() : nullptr;
  WiFi.begin(cfg.ssid(), pw);
  WiFi.setSleep(WIFI_PS_NONE);
}

enum class LotatoWifiScanPhase : uint8_t { Idle, DisconnectWait, Scanning };
static LotatoWifiScanPhase s_wscan_phase = LotatoWifiScanPhase::Idle;
static uint32_t s_wscan_t0 = 0;
/** Set when an async scan finished; cleared after user views page 1 via bare `lotato scan`. */
static bool s_wscan_results_ready = false;

static void format_scan_page(int page, char* reply);

static void lotato_wifi_scan_fill_from_driver(int n) {
  s_scan_count = 0;
  for (int i = 0; i < n && s_scan_count < kScanMax; i++) {
    String ss = WiFi.SSID(i);
    if (ss.length() == 0) continue;
    int32_t rssi = WiFi.RSSI(i);
    bool found = false;
    for (int j = 0; j < s_scan_count; j++) {
      if (strcmp(s_scan[j].ssid, ss.c_str()) == 0) {
        if (rssi > s_scan[j].rssi) s_scan[j].rssi = rssi;
        found = true;
        break;
      }
    }
    if (!found) {
      snprintf(s_scan[s_scan_count].ssid, sizeof(s_scan[0].ssid), "%s", ss.c_str());
      s_scan[s_scan_count].rssi = rssi;
      s_scan_count++;
    }
  }
  for (int a = 0; a < s_scan_count - 1; a++) {
    for (int b = a + 1; b < s_scan_count; b++) {
      if (s_scan[b].rssi > s_scan[a].rssi) {
        ScanEntry t = s_scan[a];
        s_scan[a] = s_scan[b];
        s_scan[b] = t;
      }
    }
  }
}

void my_mesh_lotato_wifi_scan_poll(MyMesh* mesh) {
  switch (s_wscan_phase) {
    case LotatoWifiScanPhase::Idle:
      break;
    case LotatoWifiScanPhase::DisconnectWait:
      if ((int32_t)(millis() - s_wscan_t0) < 120) break;
      LOTATO_DBG_LN("lotato CLI: wifi async scan radio ready");
      {
        int16_t started = WiFi.scanNetworks(true, false, false, 300);
        // Async success is WIFI_SCAN_RUNNING (-1); failure is WIFI_SCAN_FAILED (-2).
        if (started == WIFI_SCAN_FAILED) {
          LOTATO_DBG_LN("lotato CLI: wifi async scan start failed");
          WiFi.scanDelete();
          lotato_sta_failover_suppress(false);
          lotato_resume_sta_saved_credentials();
          s_wscan_phase = LotatoWifiScanPhase::Idle;
          s_wscan_results_ready = false;
          if (s_scan_reply_target.valid) {
            mesh->enqueueTxtCliReply(s_scan_reply_target.acl_idx, s_scan_reply_target.out_path_len,
                                     s_scan_reply_target.out_path, s_scan_reply_target.path_hash_size,
                                     0, "Err - WiFi scan start failed");
            s_scan_reply_target.valid = false;
          }
          s_async_cli_busy = false;
        } else {
          s_wscan_phase = LotatoWifiScanPhase::Scanning;
        }
      }
      break;
    case LotatoWifiScanPhase::Scanning: {
      // abort if stuck for too long (runaway guard)
      if ((uint32_t)(millis() - s_wscan_t0) > kWifiScanTimeoutMs) {
        LOTATO_DBG_LN("lotato CLI: wifi async scan TIMEOUT after %lums", (unsigned long)kWifiScanTimeoutMs);
        WiFi.scanDelete();
        s_scan_count = 0;
        lotato_sta_failover_suppress(false);
        lotato_resume_sta_saved_credentials();
        s_wscan_phase = LotatoWifiScanPhase::Idle;
        if (s_scan_reply_target.valid) {
          mesh->enqueueTxtCliReply(s_scan_reply_target.acl_idx, s_scan_reply_target.out_path_len,
                                   s_scan_reply_target.out_path, s_scan_reply_target.path_hash_size,
                                   0, "Err - WiFi scan timed out");
          s_scan_reply_target.valid = false;
        }
        s_async_cli_busy = false;
        break;
      }
      int16_t cnt = WiFi.scanComplete();
      if (cnt == WIFI_SCAN_RUNNING) break;
      if (cnt == WIFI_SCAN_FAILED) {
        LOTATO_DBG_LN("lotato CLI: wifi async scan complete failed");
        WiFi.scanDelete();
        s_scan_count = 0;
        s_wscan_results_ready = false;
        if (s_scan_reply_target.valid) {
          mesh->enqueueTxtCliReply(s_scan_reply_target.acl_idx, s_scan_reply_target.out_path_len,
                                   s_scan_reply_target.out_path, s_scan_reply_target.path_hash_size,
                                   0, "Err - WiFi scan failed");
          s_scan_reply_target.valid = false;
        }
      } else {
        lotato_wifi_scan_fill_from_driver((int)cnt);
        WiFi.scanDelete();
        s_wscan_results_ready = true;
        LOTATO_DBG_LN("lotato CLI: wifi async scan done nets=%d", s_scan_count);
        if (s_scan_reply_target.valid) {
          int total_pages = (s_scan_count + kScanPageSize - 1) / kScanPageSize;
          if (total_pages < 1) total_pages = 1;
          for (int pg = 1; pg <= total_pages; pg++) {
            char pgbuf[MyMesh::kCliReplyCap];
            format_scan_page(pg, pgbuf);
            mesh->enqueueTxtCliReply(s_scan_reply_target.acl_idx, s_scan_reply_target.out_path_len,
                                     s_scan_reply_target.out_path, s_scan_reply_target.path_hash_size,
                                     0, pgbuf);
          }
          LOTATO_DBG_LN("lotato scan async: pushed %d page(s) nets=%d", total_pages, s_scan_count);
          s_scan_reply_target.valid = false;
        }
      }
      lotato_sta_failover_suppress(false);
      lotato_resume_sta_saved_credentials();
      s_wscan_phase = LotatoWifiScanPhase::Idle;
      s_async_cli_busy = false;
      break;
    }
  }
}

static void run_lotato_wifi_scan_cli(const char* page_arg, char* reply, MyMesh* mesh) {
  my_mesh_lotato_wifi_scan_poll(mesh);
  if (*page_arg) {
    format_scan_page(atoi(page_arg), reply);
    return;
  }
  if (s_wscan_phase != LotatoWifiScanPhase::Idle) {
    // scan already running — from serial this is reachable; from mesh it is blocked by busy gate
    snprintf(reply, MyMesh::kCliReplyCap, "WiFi scan in progress...");
    return;
  }
  if (s_wscan_results_ready) {
    // legacy serial path: show results on second call
    s_wscan_results_ready = false;
    format_scan_page(1, reply);
    return;
  }
  lotato_sta_failover_suppress(true);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  s_wscan_t0 = millis();
  s_wscan_phase = LotatoWifiScanPhase::DisconnectWait;
  s_async_cli_busy = true;
  LOTATO_DBG_LN("lotato CLI: wifi async scan queued");
  // immediate ack — mesh admin gets auto-pushed results when done; serial gets them on next call
  snprintf(reply, MyMesh::kCliReplyCap, "Scanning for WiFi devices...");
}

static void format_scan_page(int page, char* reply) {
  if (s_scan_count == 0) {
    strcpy(reply, "No scan results. Run `lotato scan` twice (start, then list).");
    return;
  }
  int total_pages = (s_scan_count + kScanPageSize - 1) / kScanPageSize;
  if (page < 1) page = 1;
  if (page > total_pages) page = total_pages;
  int start = (page - 1) * kScanPageSize;
  int o = snprintf(reply, MyMesh::kCliReplyCap, "Pg %d/%d (%d nets):\n", page, total_pages, s_scan_count);
  for (int i = start; i < s_scan_count && i < start + kScanPageSize; i++) {
    char bars[7]; scan_rssi_bars(s_scan[i].rssi, bars);
    // SSID truncated to 14 chars
    char ssid_trunc[17];
    size_t slen = strlen(s_scan[i].ssid);
    if (slen > 14) {
      memcpy(ssid_trunc, s_scan[i].ssid, 12); ssid_trunc[12] = '.'; ssid_trunc[13] = '.'; ssid_trunc[14] = '\0';
    } else {
      strcpy(ssid_trunc, s_scan[i].ssid);
    }
    int added = snprintf(reply + o, MyMesh::kCliReplyCap - (size_t)o, "%d. %s %s\n", i + 1, ssid_trunc, bars);
    if (added <= 0 || o + added >= (int)MyMesh::kCliReplyCap - 1) break;
    o += added;
  }
  reply[MyMesh::kCliReplyCap - 1] = '\0';
}

static void lotato_cli_usage(char* reply) {
  snprintf(reply, MyMesh::kCliReplyCap,
           "lotato status\n"
           "lotato pause\n"
           "lotato resume\n"
           "lotato contacts\n"
           "lotato flush\n"
           "lotato help\n"
           "lotato scan [pg]  (async: run twice to list)\n"
           "lotato wifi scan [pg]\n"
           "lotato wifi <n> [pwd]\n"
           "lotato wifi <ssid> [pwd]\n"
           "lotato endpoint <url>\n"
           "lotato token <val>\n"
           "lotato debug on|off  (bare lotato debug toggles)");
}

void MyMesh::handleLotaToCommand(char* args, char* reply) {
  trim_trailing_ws(args);
  while (*args) {
    unsigned char c = static_cast<unsigned char>(*args);
    if (!isspace(c)) break;
    args++;
  }

  // Before any WiFi.* (can stall when the STA stack is wedged).
  if (strcmp(args, "help") == 0 || strcmp(args, "?") == 0) {
    lotato_cli_usage(reply);
    return;
  }

  LotatoConfig& cfg = LotatoConfig::instance();

  // lotato status
  if (strcmp(args, "status") == 0 || *args == '\0') {
    wl_status_t wl = WiFi.status();
    const char* wl_str = (wl == WL_CONNECTED) ? "connected" : "not connected";
    int code = _ingestor.lastHttpCode();
    char code_str[12];
    if (code == 0) strcpy(code_str, "none");
    else snprintf(code_str, sizeof(code_str), "%d", code);
    const char* token_str = cfg.apiToken()[0] ? "set" : "(none)";
    const char* url_str = cfg.ingestOrigin()[0] ? cfg.ingestOrigin() : "(none)";
    if (wl == WL_CONNECTED) {
      snprintf(reply, MyMesh::kCliReplyCap, "WiFi: %s\nSSID: %.20s\nIP: %s\nNodes: %d\nQueue: %u\nPaused: %s\nHTTP: %s\nURL: %.72s\nToken: %s",
               wl_str, WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
               _node_store.count(), (unsigned)_ingestor.pendingQueueDepth(),
               _ingestor.isPaused() ? "yes" : "no", code_str, url_str, token_str);
    } else {
      snprintf(reply, MyMesh::kCliReplyCap, "WiFi: %s\nSaved: %.20s\nNodes: %d\nQueue: %u\nPaused: %s\nURL: %.72s\nToken: %s",
               wl_str, cfg.ssid()[0] ? cfg.ssid() : "(none)",
               _node_store.count(), (unsigned)_ingestor.pendingQueueDepth(),
               _ingestor.isPaused() ? "yes" : "no", url_str, token_str);
    }

  // lotato pause / resume
  } else if (strcmp(args, "pause") == 0) {
    _ingestor.setPaused(true);
    strcpy(reply, "OK - ingest paused");

  } else if (strcmp(args, "resume") == 0) {
    _ingestor.setPaused(false);
    strcpy(reply, "OK - ingest resumed");

  // lotato endpoint <url>
  } else if (strncmp(args, "endpoint ", 9) == 0) {
    const char* url = args + 9;
    while (*url == ' ') url++;
    cfg.setIngestOrigin(url);
    _ingestor.restartAfterConfigChange();
    LOTATO_DBG_LN("lotato CLI: endpoint set url=%.60s", url);
    snprintf(reply, MyMesh::kCliReplyCap, "OK - endpoint: %.100s", url);

  // lotato token <val>
  } else if (strncmp(args, "token ", 6) == 0) {
    const char* tok = args + 6;
    while (*tok == ' ') tok++;
    cfg.setApiToken(tok);
    _ingestor.restartAfterConfigChange();
    LOTATO_DBG_LN("lotato CLI: token set (len=%u)", (unsigned)strlen(tok));
    strcpy(reply, "OK - token saved");

  // lotato debug on|off  (bare lotato debug toggles)
  } else if (strncmp(args, "debug", 5) == 0 &&
             (args[5] == '\0' || isspace(static_cast<unsigned char>(args[5])))) {
    const char* sub = args + 5;
    while (*sub == ' ') sub++;
    if (*sub == '\0') {
      cfg.toggleDebug();
    } else if (strcmp(sub, "on") == 0) {
      cfg.setDebug(true);
    } else if (strcmp(sub, "off") == 0) {
      cfg.setDebug(false);
    } else {
      snprintf(reply, MyMesh::kCliReplyCap, "Use: lotato debug on|off  (bare lotato debug toggles)");
      return;
    }
    snprintf(reply, MyMesh::kCliReplyCap, "OK - debug %s", cfg.debugEnabled() ? "on" : "off");

  // lotato scan [page]  — alias for lotato wifi scan
  } else if (strcmp(args, "scan") == 0 || strncmp(args, "scan ", 5) == 0) {
    const char* pg_str = (strcmp(args, "scan") == 0) ? "" : (args + 5);
    while (*pg_str == ' ') pg_str++;
    run_lotato_wifi_scan_cli(pg_str, reply, this);

  // lotato wifi scan [page]
  } else if (strncmp(args, "wifi scan", 9) == 0 &&
             (args[9] == '\0' || isspace(static_cast<unsigned char>(args[9])))) {
    const char* pg_str = args + 9;
    while (*pg_str == ' ') pg_str++;
    run_lotato_wifi_scan_cli(pg_str, reply, this);

  // lotato wifi <n> [pwd]  or  lotato wifi <ssid> [pwd]  or  lotato wifi (status)
  } else if (strncmp(args, "wifi", 4) == 0 &&
             (args[4] == '\0' || isspace(static_cast<unsigned char>(args[4])))) {
    const char* sub = args + 4;
    while (*sub == ' ') sub++;

    if (*sub == '\0') {
      // status only
      wl_status_t wl = WiFi.status();
      if (wl == WL_CONNECTED) {
        snprintf(reply, MyMesh::kCliReplyCap, "WiFi: connected\nSSID: %.30s\nIP: %s",
                 WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      } else {
        snprintf(reply, MyMesh::kCliReplyCap, "WiFi: not connected\nSaved: %.30s\nUse: lotato scan (async)",
                 cfg.ssid()[0] ? cfg.ssid() : "(none)");
      }
    } else {
      // parse: first token = index or SSID, second token (optional) = password
      char tok1[64] = {}, tok2[65] = {};
      const char* sp = sub;
      while (*sp && *sp != ' ') sp++;
      size_t t1len = (size_t)(sp - sub);
      if (t1len >= sizeof(tok1)) t1len = sizeof(tok1) - 1;
      memcpy(tok1, sub, t1len); tok1[t1len] = '\0';
      while (*sp == ' ') sp++;
      strncpy(tok2, sp, sizeof(tok2) - 1); tok2[sizeof(tok2) - 1] = '\0';

      // determine SSID: is tok1 a numeric list index?
      char ssid_to_use[33] = {};
      bool is_index = true;
      for (const char* q = tok1; *q; q++) { if (*q < '0' || *q > '9') { is_index = false; break; } }
      if (is_index && tok1[0] != '\0') {
        int idx = atoi(tok1) - 1;
        if (idx >= 0 && idx < s_scan_count) {
          strncpy(ssid_to_use, s_scan[idx].ssid, sizeof(ssid_to_use) - 1);
        } else {
          snprintf(reply, MyMesh::kCliReplyCap, "Err - index out of range (1..%d)\nRun: lotato scan first", s_scan_count);
          return;
        }
      } else {
        strncpy(ssid_to_use, tok1, sizeof(ssid_to_use) - 1);
      }

      // resolve password: use tok2 if provided, else check NVS known list
      char pwd_to_use[65] = {};
      if (tok2[0] != '\0') {
        strcpy(pwd_to_use, tok2);
      } else {
        cfg.getKnownWifiPassword(ssid_to_use, pwd_to_use, sizeof(pwd_to_use));
      }

      cfg.setWifi(ssid_to_use, pwd_to_use);
      _ingestor.restartAfterConfigChange();
      WiFi.disconnect(false, false);
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid_to_use, pwd_to_use);
      WiFi.setSleep(WIFI_PS_NONE);
      LOTATO_DBG_LN("lotato CLI: wifi connecting ssid=%.32s modem_sleep=off", ssid_to_use);
      snprintf(reply, MyMesh::kCliReplyCap, "OK - connecting to %.32s", ssid_to_use);
    }

  // lotato contacts
  } else if (strcmp(args, "contacts") == 0) {
    snprintf(reply, MyMesh::kCliReplyCap, "Nodes: %d/%d\nRepost: %lus\nFile: %s",
             _node_store.count(), LotatoNodeStore::MAX,
             (unsigned long)(LOTATO_NODE_REPOST_MS / 1000),
             LotatoNodeStore::PATH);

  // lotato flush — re-post all known nodes on next sweep
  } else if (strcmp(args, "flush") == 0) {
    _node_store.resetPostTimers();
    _node_store.logFlushTargetsDebug();
    LOTATO_DBG_LN("lotato CLI: flush — reset post timers for %d nodes", _node_store.count());
    snprintf(reply, MyMesh::kCliReplyCap, "OK - will re-post %d nodes", _node_store.count());

  } else {
    lotato_cli_usage(reply);
  }
}
#endif

/* ── CLI reply FIFO ─────────────────────────────────────────────────── */

bool MyMesh::enqueueTxtCliReply(int acl_idx, uint8_t out_path_len, const uint8_t* out_path,
                                uint8_t path_hash_size, uint32_t sender_ts, const char* text) {
  size_t len = strlen(text);
  if (len == 0) return false;

  CliReplyJob* job = new CliReplyJob();
  if (!job) {
    LOTATO_DBG_LN("cli reply q: OOM job acl=%d", acl_idx);
    return false;
  }
  job->text = new char[len + 1];
  if (!job->text) {
    LOTATO_DBG_LN("cli reply q: OOM text acl=%d len=%u", acl_idx, (unsigned)len);
    delete job;
    return false;
  }
  memcpy(job->text, text, len + 1);
  job->total_len      = len;
  job->offset         = 0;
  job->sender_ts      = sender_ts;
  job->next_send_at   = millis();
  job->acl_idx        = acl_idx;
  memcpy(job->out_path, out_path, sizeof(job->out_path));
  job->out_path_len   = out_path_len;
  job->path_hash_size = path_hash_size;
  job->next           = nullptr;

  if (_cli_reply_tip) {
    _cli_reply_tip->next = job;
    _cli_reply_tip = job;
  } else {
    _cli_reply_root = _cli_reply_tip = job;
  }

  size_t chunks = (len + kMaxTxtChunk - 1) / kMaxTxtChunk;
  LOTATO_DBG_LN("cli reply q: enqueue len=%u chunks~%u acl=%d", (unsigned)len, (unsigned)chunks, acl_idx);
  return true;
}

void MyMesh::serviceCliReplyQueue() {
  if (!_cli_reply_root) return;
  CliReplyJob* job = _cli_reply_root;
  if ((long)(millis() - job->next_send_at) < 0) return;

  if (job->acl_idx < 0 || job->acl_idx >= acl.getNumClients()) {
    LOTATO_DBG_LN("cli reply q: drop stale acl=%d", job->acl_idx);
    _cli_reply_root = job->next;
    if (!_cli_reply_root) _cli_reply_tip = nullptr;
    delete[] job->text;
    delete job;
    return;
  }
  ClientInfo* client = acl.getClientByIdx(job->acl_idx);

  size_t remaining = job->total_len - job->offset;
  size_t chunk = (remaining < kMaxTxtChunk) ? remaining : kMaxTxtChunk;
  // trim to last newline within window for cleaner visual splits
  if (chunk == kMaxTxtChunk) {
    for (int j = (int)kMaxTxtChunk - 1; j >= (int)kMaxTxtChunk / 2; j--) {
      if (job->text[job->offset + j] == '\n') { chunk = (size_t)(j + 1); break; }
    }
  }

  size_t total_chunks = (job->total_len + kMaxTxtChunk - 1) / kMaxTxtChunk;
  size_t cur_chunk    = (job->offset / kMaxTxtChunk) + 1;

  uint8_t temp[5 + kMaxTxtChunk];
  uint32_t ts = getRTCClock()->getCurrentTimeUnique();
  if (ts == job->sender_ts) ts++;
  memcpy(temp, &ts, 4);
  temp[4] = (TXT_TYPE_CLI_DATA << 2);
  memcpy(&temp[5], job->text + job->offset, chunk);

  uint8_t secret[PUB_KEY_SIZE];
  memcpy(secret, client->shared_secret, PUB_KEY_SIZE);

  mesh::Packet* pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + chunk);
  bool sent = false;
  if (pkt) {
    if (job->out_path_len == OUT_PATH_UNKNOWN) {
      sendFlood(pkt, 0, job->path_hash_size);
    } else {
      sendDirect(pkt, job->out_path, job->out_path_len, 0);
    }
    sent = true;
  }

  LOTATO_DBG_LN("cli reply tx: chunk %u/%u bytes=%u %s acl=%d peer=%02x%02x%02x%02x %s",
    (unsigned)cur_chunk, (unsigned)total_chunks, (unsigned)chunk,
    (job->out_path_len == OUT_PATH_UNKNOWN) ? "flood" : "direct",
    job->acl_idx,
    (unsigned)client->id.pub_key[0], (unsigned)client->id.pub_key[1],
    (unsigned)client->id.pub_key[2], (unsigned)client->id.pub_key[3],
    sent ? "ok" : "FAIL");
#ifdef ESP32
  if (lotato_dbg_active()) {
    Serial.print("Lotato: cli reply tx: full msg len=");
    Serial.print((unsigned)job->total_len);
    Serial.print(": ");
    const char* p = job->text;
    size_t n = 0;
    while (*p) { Serial.write(static_cast<uint8_t>(*p++)); if (++n % 48 == 0) yield(); }
    Serial.print("\r\n");
  }
#endif

  job->offset += chunk;
  if (job->offset >= job->total_len) {
    LOTATO_DBG_LN("cli reply q: complete %u/%u acl=%d",
      (unsigned)cur_chunk, (unsigned)total_chunks, job->acl_idx);
    _cli_reply_root = job->next;
    if (!_cli_reply_root) _cli_reply_tip = nullptr;
    delete[] job->text;
    delete job;
  } else {
    job->next_send_at = millis() + CLI_REPLY_DELAY_MILLIS;
  }
}

void MyMesh::clearCliReplyQueue() {
  while (_cli_reply_root) {
    CliReplyJob* j = _cli_reply_root;
    _cli_reply_root = j->next;
    delete[] j->text;
    delete j;
  }
  _cli_reply_tip = nullptr;
}

/* ── end CLI reply FIFO ─────────────────────────────────────────────── */

void MyMesh::loop() {
#ifdef WITH_BRIDGE
  bridge.loop();
#endif

  mesh::Mesh::loop();
  serviceCliReplyQueue();

#ifdef ESP32
  my_mesh_lotato_wifi_scan_poll(this);
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
  if (_cli_reply_root != nullptr) return true;  // CLI reply chunks still queued
  return _mgr->getOutboundTotal() > 0;
}
