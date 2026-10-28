#include "CardputerMesh.h"

void CardputerMesh::onTraceRecv(mesh::Packet *packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                                const uint8_t *path_snrs, const uint8_t *path_hashes, uint8_t path_len) {
  MyMesh::onTraceRecv(packet, tag, auth_code, flags, path_snrs, path_hashes, path_len);

  if (path_len == getNodePrefs()->path_hash_mode + 1 && tag == last_ping_tag) {
    int8_t snr_signed = (int8_t)path_snrs[0];
    _ui->pingRecv(snr_signed / 4.0f, packet->getSNR());
  }
}

void CardputerMesh::onControlDataRecv(mesh::Packet *packet) {
  MyMesh::onControlDataRecv(packet);
  uint8_t type = packet->payload[0] & 0xF0;
  uint8_t node_type = packet->payload[0] & 0x0F;

  if (!(type == CTL_TYPE_NODE_DISCOVER_RESP && packet->payload_len >= 6)) {
    return;
  }

  if (node_type != ADV_TYPE_REPEATER) {
    return;
  }

  if (packet->payload_len < 6 + PUB_KEY_SIZE) {
    return;
  }

  uint32_t tag;
  memcpy(&tag, &packet->payload[2], 4);

  if (tag != last_discover_tag) {
    return;
  }

  mesh::Identity id(&packet->payload[6]);

  if (id.matches(self_id)) {
    return;
  }

  _ui->discoverRecv(id, packet->getSNR());
}

mesh::DispatcherAction CardputerMesh::onRecvPacket(mesh::Packet *pkt) {
  mesh::DispatcherAction action = MyMesh::onRecvPacket(pkt);

  if (pkt->getPayloadType() == PAYLOAD_TYPE_GRP_TXT) {
    uint8_t hash[MAX_HASH_SIZE];
    pkt->calculatePacketHash(hash);

    if (memcmp(hash, last_message_hash, MAX_HASH_SIZE) == 0) {
      last_message_heard_repeats++;
      _ui->messageRepeatsRecv(last_message_heard_repeats);
    }
  }

  return action;
}

bool CardputerMesh::channelHistoryId(const ChannelDetails &channel, uint8_t id[PUB_KEY_SIZE]) {
  static const uint8_t domain[] = "Cardputer channel history v2";
  const size_t name_length = strnlen(channel.name, sizeof(channel.name));
  if (!name_length || name_length == sizeof(channel.name)) return false;
  uint8_t identity[sizeof(domain) + 1 + sizeof(channel.name) + sizeof(channel.channel.secret)];
  size_t used = 0;
  memcpy(identity, domain, sizeof(domain)); used += sizeof(domain);
  identity[used++] = static_cast<uint8_t>(name_length);
  memcpy(identity + used, channel.name, name_length); used += name_length;
  memcpy(identity + used, channel.channel.secret, sizeof(channel.channel.secret));
  used += sizeof(channel.channel.secret);
  mesh::Utils::sha256(id, PUB_KEY_SIZE, identity, used);
  return true;
}

bool CardputerMesh::getChannelHistoryId(int channel_idx, uint8_t id[PUB_KEY_SIZE]) {
  ChannelDetails channel;
  return getChannel(channel_idx, channel) && channelHistoryId(channel, id);
}

void CardputerMesh::onUiChannelMessage(uint8_t channel_idx, const char *text) {
  uint8_t id[PUB_KEY_SIZE];
  if (!getChannelHistoryId(channel_idx, id)) return;
  _store->storeMessage(id, text, false, true);
  _ui->onChannelMessageRecv(channel_idx, id, text);
}

void CardputerMesh::onUiContactMessage(const ContactInfo &from, const char *text) {
  _store->storeMessage(from.id.pub_key, text, false, false);
  _ui->onContactMessageRecv(from, text);
}

void CardputerMesh::onUiContactMessageSent(const ContactInfo &recipient, const char *text) {
  _store->storeMessage(recipient.id.pub_key, text, true, false);
  _ui->onContactMessageSent(recipient, text);
}

void CardputerMesh::onUiChannelMessageSent(uint8_t channel_idx, const char *text, size_t length) {
  uint8_t id[PUB_KEY_SIZE];
  if (!getChannelHistoryId(channel_idx, id)) return;
  char stored_text[MAX_MESSAGE_LENGTH + 1];
  const size_t stored_length = length < MAX_MESSAGE_LENGTH ? length : MAX_MESSAGE_LENGTH;
  memcpy(stored_text, text, stored_length);
  stored_text[stored_length] = 0;
  _store->storeMessage(id, stored_text, true, true);
  _ui->onChannelMessageSent(channel_idx, id, stored_text);
}

bool CardputerMesh::sendFloodScoped(const mesh::GroupChannel &channel, mesh::Packet *pkt,
                                    uint32_t delay_millis) {
  const bool is_group_text = pkt && pkt->getPayloadType() == PAYLOAD_TYPE_GRP_TXT;
  uint8_t hash[MAX_HASH_SIZE];
  if (is_group_text) {
    pkt->calculatePacketHash(hash);
  }

  // The base send owns pkt, including releasing it when admission fails.
  bool sent = MyMesh::sendFloodScoped(channel, pkt, delay_millis);

  if (sent && is_group_text) {
    memcpy(last_message_hash, hash, sizeof(last_message_hash));
    last_message_heard_repeats = 0;
  }
  return sent;
}

bool CardputerMesh::sendChannelMessage(int channel_idx, uint32_t timestamp, const char *text, size_t length) {
  return sendUiChannelMessage(channel_idx, timestamp, text, length);
}

int CardputerMesh::sendDirectMessage(const ContactInfo &recipient, uint32_t timestamp, const char *text) {
  if (isMessageSending() || !text || !text[0]
      || strnlen(text, MAX_MESSAGE_LENGTH + 1) > MAX_MESSAGE_LENGTH) return MSG_SEND_FAILED;
  uint32_t est_timeout = 0, expected_ack = 0;
  const int rc = MyMesh::sendMessage(recipient, timestamp, 0, text, expected_ack, est_timeout);
  if (rc == MSG_SEND_FAILED) return rc;
  bool flood = (recipient.out_path_len == OUT_PATH_UNKNOWN);
  last_msg.timestamp = timestamp;
  last_msg.expected_ack = expected_ack;
  last_msg.recipient = recipient;
  strncpy(last_msg.text, text, sizeof(last_msg.text));
  last_msg.text[sizeof(last_msg.text) - 1] = 0;
  last_msg.state = MESSAGE_SENDING;
  last_msg.need_direct = !flood;
  last_msg.attempt = 1;
  last_msg.total_attempts = DIRECT_SEND_FLOOD_ATTEMPTS + (!flood ? DIRECT_SEND_ROUTE_ATTEMPTS : 0);

  const uint32_t minimum_timeout = flood ? DIRECT_SEND_FLOOD_RESEND_RELAY : DIRECT_SEND_ROUTE_RESEND_RELAY;
  resend_timeout = futureMillis(est_timeout > minimum_timeout ? est_timeout : minimum_timeout);
  _ui->onMessageSendAttempt(last_msg.attempt, last_msg.total_attempts, last_msg.state);
  return rc;
}

void CardputerMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
  MyMesh::logRxRaw(snr, rssi, raw, len);
  rx_packet_count++;
}

bool CardputerMesh::processAck(const uint8_t *data, ContactInfo *&peer) {
  const bool base_owned = MyMesh::processAck(data, peer);
  uint32_t hash;
  memcpy(&hash, data, 4);

  if (last_msg.state == MESSAGE_SENDING && last_msg.expected_ack != 0 && last_msg.expected_ack == hash) {
    if (!base_owned) {
      peer = lookupContactByPubKey(last_msg.recipient.id.pub_key, PUB_KEY_SIZE);
    }
    last_msg.state = MESSAGE_DELIVERED;
    last_msg.expected_ack = 0; // ignore duplicate ACKs
    resend_timeout = 0;
    _ui->onMessageSendAttempt(last_msg.attempt + 1, last_msg.total_attempts, last_msg.state);
    return true;
  }

  return base_owned;
}

void CardputerMesh::beginCustom() {
  _store->loadCustomPrefs(_custom_prefs);
  _saved_custom_prefs = _custom_prefs;
}

bool CardputerMesh::savePrefs() {
  return MyMesh::savePrefs();
}

bool CardputerMesh::saveCustomPrefs() {
  if (!_store->saveCustomPrefs(_custom_prefs)) {
    _custom_prefs = _saved_custom_prefs;
    return false;
  }
  _saved_custom_prefs = _custom_prefs;
  return true;
}

bool CardputerMesh::sendPing(ContactInfo &contact) {
  CompanionNodePrefs *prefs = getNodePrefs();
  uint8_t flags = prefs->path_hash_mode & 0x03;

  getRNG()->random((uint8_t *)&last_ping_tag, sizeof(last_ping_tag));

  mesh::Packet *pkt = the_mesh_cp.createTrace(last_ping_tag, 0, flags);

  if (!pkt) {
    return false;
  }

  sendDirect(pkt, contact.id.pub_key, prefs->path_hash_mode + 1);
  return true;
}

bool CardputerMesh::sendAdvert(bool flood) {
  mesh::Packet *pkt;
  CompanionNodePrefs *prefs = getNodePrefs();

  if (prefs->advert_loc_policy == ADVERT_LOC_NONE) {
    pkt = createSelfAdvert(prefs->node_name);
  } else {
    pkt = createSelfAdvert(prefs->node_name, sensors.node_lat, sensors.node_lon);
  }

  if (!pkt) {
    return false;
  }

  if (flood) {
    TransportKey default_scope;
    memcpy(&default_scope.key, prefs->default_scope_key, sizeof(default_scope.key));
    MyMesh::sendFloodScoped(default_scope, pkt, 0);
  } else {
    sendZeroHop(pkt);
  }

  return true;
}

struct __attribute__((packed)) discover_data_t {
  uint8_t ctl_type;
  uint8_t discover_type;
  uint32_t tag;
  uint32_t since;
};

bool CardputerMesh::sendRepeatersDiscover() {
  discover_data_t data;
  data.ctl_type = CTL_TYPE_NODE_DISCOVER_REQ;
  data.discover_type = (1 << ADV_TYPE_REPEATER);
  data.since = 0;
  getRNG()->random((uint8_t *)&data.tag, sizeof(data.tag));

  auto pkt = createControlData((uint8_t *)&data, sizeof(data));
  if (!pkt) {
    return false;
  }

  last_discover_tag = data.tag;
  sendZeroHop(pkt);

  return true;
}

void CardputerMesh::loadMessageHistory(const uint8_t pkey[PUB_KEY_SIZE], bool is_channel,
                                       ChatHistory &history) {
  _store->loadMessages(pkey, is_channel, history);
}

void CardputerMesh::checkResend() {
  if (last_msg.state != MESSAGE_SENDING) {
    return;
  }

  ContactInfo *ref = lookupContactByPubKey(last_msg.recipient.id.pub_key, PUB_KEY_SIZE);
  if (!ref || last_msg.attempt >= last_msg.total_attempts) {
    cancelResending();
    return;
  }

  if (last_msg.need_direct && last_msg.attempt >= DIRECT_SEND_ROUTE_ATTEMPTS) {
    last_msg.need_direct = false;

    MESH_DEBUG_PRINTLN("Resetting path");
    const uint8_t previous_path_len = ref->out_path_len;
    ref->out_path_len = OUT_PATH_UNKNOWN;
    if (!queueUiContactWrite(*ref)) {
      ref->out_path_len = previous_path_len;
      cancelResending();
      return;
    }
  }
  last_msg.recipient = *ref;
  if (!last_msg.need_direct) last_msg.recipient.out_path_len = OUT_PATH_UNKNOWN;

  if (last_msg.attempt < last_msg.total_attempts) {
    uint32_t est_timeout = 0, expected_ack = 0;

    // expected_ack is derived from the attempt number, so it must be refreshed on every resend
    const int rc = MyMesh::sendMessage(last_msg.recipient, last_msg.timestamp, last_msg.attempt, last_msg.text,
                                      expected_ack, est_timeout);
    if (rc == MSG_SEND_FAILED) {
      cancelResending();
      return;
    }
    last_msg.expected_ack = expected_ack;
    const uint32_t minimum_timeout = last_msg.recipient.out_path_len == OUT_PATH_UNKNOWN
        ? DIRECT_SEND_FLOOD_RESEND_RELAY : DIRECT_SEND_ROUTE_RESEND_RELAY;
    resend_timeout = futureMillis(est_timeout > minimum_timeout ? est_timeout : minimum_timeout);

    last_msg.attempt++;
    _ui->onMessageSendAttempt(last_msg.attempt, last_msg.total_attempts, last_msg.state);
  } else {
    last_msg.state = MESSAGE_FAILED;
    _ui->onMessageSendAttempt(last_msg.attempt + 1, last_msg.total_attempts, last_msg.state);
    MESH_DEBUG_PRINTLN("Message sending failed");
  }
}

void CardputerMesh::loop() {
  MyMesh::loop();

  if (resend_timeout && millisHasNowPassed(resend_timeout)) {
    resend_timeout = 0;
    checkResend();
  }
}

void CardputerMesh::cancelResending() {
  resend_timeout = 0;
  last_msg.expected_ack = 0;
  if (last_msg.state == MESSAGE_SENDING) {
    last_msg.state = MESSAGE_FAILED;
    _ui->onMessageSendAttempt(last_msg.total_attempts, last_msg.total_attempts, last_msg.state);
  }
}
