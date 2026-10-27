#include "HostKiss.h"
#if MESH_ENABLE_HOST_KISS

#include "HostKissCodec.h"
#include "HostKissSettings.h"
#include "RadioProfileCLI.h"
#include "UsbLogging.h"
#include <Dispatcher.h>
#include <new>
#include <stdio.h>
#include <string.h>
#if defined(ESP32)
#include <WiFi.h>
#include <errno.h>
#include <lwip/sockets.h>
#endif

namespace mesh { namespace hostkiss {
namespace {
constexpr size_t ByteBudget = 128;
constexpr uint16_t TcpPort = 5004;
Settings settings;
Radio* radio = nullptr;
RadioProfileCLI* profiles = nullptr;
Dispatcher* dispatcher = nullptr;
Codec* codec = nullptr;  // All frame buffers are absent while the setting is off.
bool usb_session = false, suspended = false, leased = false, transmitting = false;
bool initial_waiting = false, initial_fresh = false, established = false, ready_started = false;
bool ready_pending = false, completion_pending = false, completion_success = false;
uint8_t quarantine_return = 0;
uint32_t profile_generation = 0, intent_generation = 0;
uint32_t received = 0, transmitted = 0, failed = 0, discarded = 0;
#if defined(ESP32)
struct Tcp {
  WiFiServer server;
  WiFiClient client;
  uint32_t address = 0, last_attempt = 0;
  bool listening = false, attempted = false, connected = false;
  Tcp() : server(TcpPort, 2) {}
};
Tcp* tcp = nullptr;
#endif

bool tcpConnected() {
#if defined(ESP32)
  return tcp && tcp->connected;
#else
  return false;
#endif
}
bool active() { return usb_session || tcpConnected(); }

void releaseRadio() {
  if (dispatcher) dispatcher->cancelHostKissFrame();
  transmitting = false;
  const auto* p = radio ? radio->profiles() : nullptr;
  // This service is the sole owner of the Kiss overlay. Its generation may
  // already have changed; let RadioProfileCLI reconcile that change itself.
  // An unrelated foreign protocol must never be cleared on our way out.
  if (leased && profiles && p && p->secondary.params.protocol == RadioProfileProtocol::Kiss)
    profiles->clearSecondaryProtocol();
  leased = false;
}

void resetFrames() {
  if (codec) codec->reset();
  ready_pending = completion_pending = false;
  quarantine_return = 0;
  initial_waiting = initial_fresh = established = ready_started = false;
}

void closeSession() {
  releaseRadio();
  resetFrames();
  usb_session = suspended = false;
  setUsbBinaryStreamOwned(false);
#if defined(ESP32)
  if (tcp) { tcp->client.stop(); tcp->connected = false; }
#endif
}

const char* admission() {
  if (!radio || !profiles || !dispatcher || !radio->supportsHostKiss()) return "unsupported radio";
  if (!profiles->manualSecondaryConfigured()
      || profiles->savedSecondaryConfig().mode != RadioProfileMode::RxTx) return "configure saved radio2 rxtx";
  if (profiles->savedSecondaryConfig().params.protocol != RadioProfileProtocol::MeshCore)
    return "radio2 protocol reserved";
  const auto* p = radio->profiles();
  if (!p || !profiles->secondaryRuntimeAvailable()) return "temporary or pending radio settings";
  if (p->secondary.mode != RadioProfileMode::RxTx || p->primary_temporary || p->secondary_temporary)
    return "radio2 unavailable";
  if (p->secondary.params.protocol != (leased ? RadioProfileProtocol::Kiss : RadioProfileProtocol::MeshCore))
    return "radio2 owned by another service";
  return nullptr;
}

bool leaseRadio() {
  if (admission() || !profiles->useSecondaryProtocol(RadioProfileProtocol::Kiss)) return false;
  const auto* p = radio->profiles();
  if (!p || p->secondary.params.protocol != RadioProfileProtocol::Kiss) return false;
  leased = true;
  profile_generation = p->generation[1];
  intent_generation = profiles->secondaryIntentGeneration();
  return true;
}

bool validLease() {
  if (!leased || !active() || suspended || admission()) return false;
  const auto* p = radio->profiles();
  return p && p->generation[1] == profile_generation
      && profiles->secondaryIntentGeneration() == intent_generation;
}
bool current() { return established && validLease(); }

// Before the first READY, a pending settings handoff or another RF owner is a
// wait, not a failed connection. All pre-READY DATA is discarded; admission
// requires no buffered input and a complete KISS frame boundary.
bool startInitialLease() {
  if (!leaseRadio()) return false;
  resetFrames();
  codec->feed(Codec::Fend);
  ready_pending = true;
  return true;
}
void admitInitial(bool input_empty) {
  if (initial_waiting && codec && input_empty && codec->atFrameBoundary()
      && (!usb_session || isUsbBinaryStreamReady())) startInitialLease();
}

bool inputByte(uint8_t byte) {
  codec->feed(byte);
  if (codec->returned()) { closeSession(); return false; }
  if (initial_waiting || suspended || !leased) {
    const uint8_t* bytes;
    size_t length;
    while (codec->peekTransmit(bytes, length)) {
      codec->consumeTransmit();
      ++discarded;
    }
  }
  return true;
}

void outputProgress() {
  // No RX or TX output is admitted before this initial READY completes.
  if (leased && !initial_waiting && !established && !ready_pending
      && !codec->queuedOutput()) established = true;
}

const char* state(const Selection& selected, const char* reason) {
  if (!selected.enabled) return "off";
  if (suspended) return "suspended";
  if (!codec) return "unavailable";
  if (active() && initial_waiting) return "waiting-radio";
  if (active() && !established) return "starting";
  if (active()) return "connected";
  if (reason) return "blocked";
#if defined(ESP32)
  if (selected.transport == Transport::Tcp) {
    if (!(WiFi.getMode() & WIFI_STA) || WiFi.status() != WL_CONNECTED
        || !uint32_t(WiFi.localIP())) return "waiting-wifi";
    return tcp && tcp->listening ? "listening" : "starting";
  }
#endif
  return "ready";
}

void invalidateSession() {
  ++discarded;
  releaseRadio();
  resetFrames();
  if (usb_session) {
    // Keep binary input quarantined until RETURN or a USB session boundary.
    // Releasing it to the ASCII parser could interpret a stale binary tail.
    suspended = true;
  } else {
    closeSession();
#if defined(ESP32)
    // Drop accepts queued in the old profile session, not only its owner.
    if (tcp) { tcp->server.end(); tcp->listening = tcp->attempted = false; }
#endif
  }
}

void checkLease() {
  if (!leased || validLease()) return;
  // A partial READY cannot be withdrawn: the peer may already have its bytes.
  if (established || ready_started) invalidateSession();
  else {
    releaseRadio();
    resetFrames();
    initial_waiting = true;
  }
}

void queueControls() {
  if (!codec) return;
  // Completion must survive a full RX queue, rather than being silently lost.
  if (completion_pending && codec->queuedOutput() < Codec::QueueDepth) {
    codec->queueTxDone(completion_success);
    completion_pending = false;
    ready_pending = true;
  }
  if (ready_pending && codec->queuedOutput() < Codec::QueueDepth) {
    codec->queueReady();
    ready_pending = false;
  }
}

void pumpTransmit() {
  if (!current() || transmitting || completion_pending || dispatcher->hostKissFramePending()) return;
  const uint8_t* bytes;
  size_t length;
  if (codec->peekTransmit(bytes, length)
      && dispatcher->submitHostKissFrame(bytes, length, profile_generation)) {
    codec->consumeTransmit();
    transmitting = true;
  }
}

void stopTransport() {
  closeSession();
#if defined(ESP32)
  if (tcp) tcp->server.end();
  delete tcp;
  tcp = nullptr;
#endif
}

bool allocate(const Selection& selection, Codec*& next_codec
#if defined(ESP32)
              , Tcp*& next_tcp
#endif
              ) {
  next_codec = codec;
#if defined(ESP32)
  next_tcp = tcp;
#endif
  if (!selection.enabled) return true;
  if (!next_codec) next_codec = new (std::nothrow) Codec;
  if (!next_codec) return false;
#if defined(ESP32)
  if (selection.transport == Transport::Tcp && !next_tcp) {
    next_tcp = new (std::nothrow) Tcp;
    if (!next_tcp) {
      if (next_codec != codec) delete next_codec;
      next_codec = codec;
      return false;
    }
  }
#endif
  return true;
}

#if defined(ESP32)
void loopTcp() {
  if (!tcp) return;
  const bool station = (WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED;
  const uint32_t address = station ? uint32_t(WiFi.localIP()) : 0;
  if (!address && !tcp->address && !tcp->listening && !tcp->connected) return;
  if (!address || address != tcp->address) {
    closeSession();
    tcp->server.end();
    tcp->listening = tcp->attempted = false;
    tcp->address = address;
    if (!address) return;
    // Never bind this unauthenticated modem to the setup AP.
    tcp->server = WiFiServer(IPAddress(address), TcpPort, 2);
  }
  const uint32_t now = millis();
  if (tcp->listening && !bool(tcp->server)) {
    tcp->server.end();
    tcp->listening = tcp->attempted = false;
  }
  if (!tcp->listening && (!tcp->attempted || uint32_t(now - tcp->last_attempt) >= 1000)) {
    tcp->last_attempt = now;
    tcp->attempted = true;
    tcp->server.begin();
    tcp->listening = bool(tcp->server);
  }
  if (tcp->connected && !tcp->client.connected()) closeSession();
  if (tcp->listening) {
    WiFiClient incoming = tcp->server.available();
    if (incoming) {
      if (tcp->connected) incoming.stop();
      else {
        resetFrames();
        tcp->client = incoming;
        tcp->client.setNoDelay(true);
        tcp->connected = true;
        suspended = false;
        initial_waiting = true;
        // A stable tuple accepts the first DATA even from a generic KISS
        // client with flow control disabled. Only a genuinely blocked initial
        // handoff needs to discard pre-READY input.
        startInitialLease();
      }
    }
  }
  if (!tcp->connected) return;
  const bool was_ready = established;
  // Drain invalid/pre-READY host bytes before admitting the final RF tuple.
  // recv's EAGAIN proves the socket input was empty on this bounded poll.
  bool input_empty = false;
  if (!established) {
    for (size_t i = 0; i < ByteBudget && codec->canAcceptByte(); ++i) {
      uint8_t byte;
      const int got = ::recv(tcp->client.fd(), &byte, 1, MSG_DONTWAIT);
      if (got == 1) { if (!inputByte(byte)) return; }
      else {
        if (!got || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
          closeSession(); return;
        }
        input_empty = errno == EAGAIN || errno == EWOULDBLOCK;
        break;
      }
    }
    admitInitial(input_empty);
  }
  queueControls();
  const uint8_t* bytes;
  size_t length;
  if (codec->peekOutput(bytes, length)) {
    if (length > ByteBudget) length = ByteBudget;
    const int written = ::send(tcp->client.fd(), bytes, length, MSG_DONTWAIT);
    if (written > 0) {
      ready_started = true;
      codec->consumeOutput(size_t(written)); outputProgress();
    }
    else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      closeSession(); return;
    }
  }
  if (!was_ready) return;
  for (size_t i = 0; i < ByteBudget && codec->canAcceptByte(); ++i) {
    uint8_t byte;
    const int got = ::recv(tcp->client.fd(), &byte, 1, MSG_DONTWAIT);
    if (got == 1) {
      if (!inputByte(byte)) break;
    } else {
      if (!got || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) closeSession();
      break;
    }
  }
}
#endif
}  // namespace

void begin(FILESYSTEM* fs, Radio& r, RadioProfileCLI& p, Dispatcher& d) {
  stopTransport();
  delete codec;
  codec = nullptr;
  radio = &r;
  profiles = &p;
  dispatcher = &d;
  received = transmitted = failed = discarded = 0;
  settings.begin(fs);
  const auto& selected = settings.selection();
  if (!selected.enabled || !r.supportsHostKiss()) return;
#if !defined(ESP32)
  if (selected.transport == Transport::Tcp) return;
#endif
  Codec* next_codec = nullptr;
#if defined(ESP32)
  Tcp* next_tcp = nullptr;
  if (allocate(selected, next_codec, next_tcp)) { codec = next_codec; tcp = next_tcp; }
#else
  if (allocate(selected, next_codec)) codec = next_codec;
#endif
}

bool handleCommand(const char* command, char* reply, size_t capacity, bool local) {
  const bool get = !strncmp(command, "get ", 4);
  const bool set = !strncmp(command, "set ", 4);
  if (!get && !set) return false;
  const char* key = command + 4;
  bool transport = !strncmp(key, "kiss.transport", 14)
      && (!key[14] || key[14] == ' ');
  const bool status = !strncmp(key, "kiss", 4) && (!key[4] || key[4] == ' ');
  if (!transport && !status) return false;
  const char* value = key + (transport ? 14 : 4);
  if (get) {
    if (*value) { snprintf(reply, capacity, "Error: use get kiss%s", transport ? ".transport" : ""); return true; }
    const auto& selected = settings.selection();
    if (transport) snprintf(reply, capacity, "> %s", selected.transport == Transport::Usb ? "usb" : "tcp");
    else {
      const char* reason = admission();
      snprintf(reply, capacity,
          "> %s transport=%s state=%s rx=%lu tx=%lu fail=%lu drop=%lu bad=%lu%s%s",
          selected.enabled ? "on" : "off", selected.transport == Transport::Usb ? "usb" : "tcp:5004",
          state(selected, reason),
          (unsigned long)received, (unsigned long)transmitted, (unsigned long)failed,
          (unsigned long)(discarded + (codec ? codec->droppedFrames() : 0)),
          (unsigned long)(codec ? codec->malformedFrames() : 0),
          selected.enabled && (!active() || initial_waiting) && reason ? " reason=" : "",
          selected.enabled && (!active() || initial_waiting) && reason ? reason : "");
    }
    return true;
  }
  if (!local) { snprintf(reply, capacity, "Error: KISS settings require a local connection"); return true; }
  while (*value == ' ') ++value;
  Selection candidate = settings.selection();
  if (transport) {
    if (!strcmp(value, "usb")) candidate.transport = Transport::Usb;
    else if (!strcmp(value, "tcp")) candidate.transport = Transport::Tcp;
    else { snprintf(reply, capacity, "Error: use set kiss.transport usb|tcp"); return true; }
#if !defined(ESP32)
    if (candidate.transport == Transport::Tcp) { snprintf(reply, capacity, "Error: TCP KISS requires ESP32 station WiFi"); return true; }
#endif
  } else if (!strcmp(value, "on")) candidate.enabled = true;
  else if (!strcmp(value, "off")) candidate.enabled = false;
  else { snprintf(reply, capacity, "Error: use set kiss on|off"); return true; }
  if (candidate.enabled && (!radio || !radio->supportsHostKiss())) {
    snprintf(reply, capacity, "Error: raw radio2 KISS is unsupported by this radio"); return true;
  }
  if (candidate == settings.selection() && settings.healthy()) { snprintf(reply, capacity, "OK - KISS unchanged"); return true; }
  Codec* next_codec;
#if defined(ESP32)
  Tcp* next_tcp;
  if (!allocate(candidate, next_codec, next_tcp)) {
#else
  if (!allocate(candidate, next_codec)) {
#endif
    snprintf(reply, capacity, "Error: insufficient memory; KISS unchanged"); return true;
  }
  // Persist before disrupting the old stream. A failed save leaves its exact
  // radio lease and pending frames intact.
  if (!settings.save(candidate)) {
    if (next_codec != codec) delete next_codec;
#if defined(ESP32)
    if (next_tcp != tcp) delete next_tcp;
#endif
    snprintf(reply, capacity, "Error: KISS save failed; service unchanged"); return true;
  }
  closeSession();
  codec = next_codec;
#if defined(ESP32)
  tcp = next_tcp;
  if (tcp && (!candidate.enabled || candidate.transport != Transport::Tcp)) {
    tcp->server.end(); delete tcp; tcp = nullptr;
  }
#endif
  if (!candidate.enabled) { delete codec; codec = nullptr; }
  snprintf(reply, capacity, candidate.enabled
      ? "OK - KISS on (%s); get kiss for status" : "OK - KISS off",
      candidate.transport == Transport::Usb ? "USB: send KISS FEND to start" : "station TCP 5004");
  return true;
}

void loop() {
  if (!settings.selection().enabled || !codec) return;
  if (active() && !suspended) checkLease();
#if defined(ESP32)
  if (settings.selection().transport == Transport::Tcp) loopTcp();
#endif
  pumpTransmit();
}

bool serviceUsb() {
  if (!settings.selection().enabled || settings.selection().transport != Transport::Usb) return false;
  Stream& port = usbCompanionPort();
  if (!usb_session) {
    if (!port.available() || port.peek() != Codec::Fend) return false;
    resetFrames();
    usb_session = true;
    suspended = false;
    initial_waiting = true;
    initial_fresh = true;
    setUsbBinaryStreamOwned(true);
  }
  // USB text writers may have been admitted just before the binary claim.
  // Their nonblocking retirement must finish before READY or host bytes move.
  if (!isUsbBinaryStreamReady()) return true;
  if (!codec) {
    // Allocation/capability failures must also keep binary bytes out of CLI.
    // This three-state RETURN recognizer needs no packet buffer.
    for (size_t i = 0; i < ByteBudget && port.available(); ++i) {
      const int value = port.read();
      if (value < 0) break;
      if (value == Codec::Fend) {
        if (quarantine_return == 2) { closeSession(); return true; }
        quarantine_return = 1;
      } else if (value == 0xff && quarantine_return == 1) quarantine_return = 2;
      else quarantine_return = 0;
    }
    return true;
  }
  if (initial_fresh) {
    initial_fresh = false;
    startInitialLease();
  }
  if (!suspended) checkLease();
  const bool was_ready = established;
  if (!established) {
    for (size_t i = 0; i < ByteBudget && port.available() && codec->canAcceptByte(); ++i) {
      const int value = port.read();
      if (value < 0) break;
      if (!inputByte(uint8_t(value))) return true;
    }
    if (!suspended) admitInitial(!port.available());
  }
  if (!suspended) {
    queueControls();
    const uint8_t* bytes;
    size_t length;
    if (codec->peekOutput(bytes, length)) {
      if (length > ByteBudget) length = ByteBudget;
      const size_t written = port.write(bytes, length);
      if (written) ready_started = true;
      codec->consumeOutput(written);
      outputProgress();
    }
  }
  if (!was_ready) { pumpTransmit(); return true; }
  for (size_t i = 0; i < ByteBudget && port.available() && codec->canAcceptByte(); ++i) {
    const int value = port.read();
    if (value < 0) break;
    if (!inputByte(uint8_t(value))) return true;
  }
  pumpTransmit();
  return true;
}

bool usbActive() { return usb_session; }
bool radioActive() { return current(); }
void resetUsbSession() { if (usb_session) closeSession(); }

void receive(const uint8_t* bytes, size_t length, uint8_t profile,
             uint32_t generation, float, float) {
  if (!codec || profile != 1 || generation != profile_generation || !current()) return;
  if (codec->queueReceive(bytes, length)) ++received;
}

void sendComplete(bool success) {
  // A completion from an invalidated session must not reach its replacement.
  if (!transmitting) return;
  transmitting = false;
  if (!current()) return;
  if (success) ++transmitted;
  else ++failed;
  completion_success = success;
  completion_pending = true;
}

}}  // namespace mesh::hostkiss
#endif
