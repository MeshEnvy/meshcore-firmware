#pragma once

#include <Mesh.h>
#include <string.h>

namespace mesh {

// RAM-only receive admission for a USB client's selected OTA admin peer.
// Matching happens after ECDH/MAC verification, before Binary login events or
// queued CLI messages lose their original RF route metadata.
class CompanionDirectAdminGuard {
  Identity peer_;
  uint8_t secret_[PUB_KEY_SIZE] = {};
  uint32_t until_ = 0;
  bool enabled_ = false, login_ = false;

public:
  static constexpr uint32_t LEASE_MS = 300000;
  ~CompanionDirectAdminGuard() { clear(); }
  void clear() {
    volatile uint8_t* bytes = secret_;
    for (size_t i = 0; i < sizeof(secret_); ++i) bytes[i] = 0;
    enabled_ = login_ = false;
  }
  bool active(uint32_t now) {
    if (enabled_ && static_cast<int32_t>(now - until_) >= 0) clear();
    return enabled_;
  }
  bool matches(const Identity& peer, uint32_t now) {
    return active(now) && peer_.matches(peer);
  }
  bool selected(const Identity& peer, uint32_t now) const {
    return enabled_ && static_cast<int32_t>(now - until_) < 0 && peer_.matches(peer);
  }
  void arm(const Identity& peer, const uint8_t* secret, uint32_t now) {
    if (!matches(peer, now)) login_ = false;
    peer_ = peer;
    memcpy(secret_, secret, sizeof(secret_));
    enabled_ = true;
    until_ = now + LEASE_MS;
  }
  void beginLogin(const Identity& peer, uint32_t now) {
    if (matches(peer, now)) login_ = true;
  }
  bool accepts(const Packet* packet, uint8_t type, const uint8_t* secret, uint32_t now) {
    if (!active(now) || !secret || memcmp(secret_, secret, sizeof(secret_))) return true;
    if (!packet || packet->getPathHashCount() != 0) return false;
    if (packet->isRouteDirect()) return true;
    // Unknown ACL return paths cause old servers to flood the first login
    // response. No relayed copy, other response type, or CLI uses this exception.
    return login_ && type == PAYLOAD_TYPE_RESPONSE && packet->isRouteFlood();
  }
  bool acceptsPath(const Packet* packet, const uint8_t* secret, uint8_t path_len, uint32_t now) {
    if (!active(now) || !secret || memcmp(secret_, secret, sizeof(secret_))) return true;
    return !(path_len & 63) && accepts(packet, PAYLOAD_TYPE_PATH, secret, now);
  }
  bool sendReturnPath(Mesh& mesh, const Identity& peer, uint32_t now) {
    if (!matches(peer, now)) return false;
    const uint8_t empty[1] = {};
    Packet* packet = mesh.createPathReturn(peer_, secret_, empty, 0, 0, nullptr, 0);
    if (packet) packet->tx_radio = RADIO_TX_PRIMARY;
    if (!packet || !mesh.sendDirect(packet, nullptr, 0)) return false;
    login_ = false;
    until_ = now + LEASE_MS;
    return true;
  }
};

}  // namespace mesh
