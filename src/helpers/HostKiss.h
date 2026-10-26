#pragma once

#include <helpers/HostKissConfig.h>
#include <stddef.h>
#include <stdint.h>

#if MESH_ENABLE_HOST_KISS
#include <helpers/IdentityStore.h>
namespace mesh {
class Radio;
class RadioProfileCLI;
class Dispatcher;
namespace hostkiss {
void begin(FILESYSTEM* fs, Radio& radio, RadioProfileCLI& profiles, Dispatcher& dispatcher);
bool handleCommand(const char* command, char* reply, size_t capacity, bool local);
void loop();
bool serviceUsb();
bool usbActive();
bool radioActive();
void resetUsbSession();
void receive(const uint8_t* bytes, size_t length, uint8_t profile,
             uint32_t generation, float snr, float rssi);
void sendComplete(bool success);
}}
#else
namespace mesh { namespace hostkiss {
template<class F, class R, class P, class D> inline void begin(F*, R&, P&, D&) {}
inline bool handleCommand(const char*, char*, size_t, bool) { return false; }
inline void loop() {}
inline bool serviceUsb() { return false; }
inline bool usbActive() { return false; }
inline bool radioActive() { return false; }
inline void resetUsbSession() {}
inline void receive(const uint8_t*, size_t, uint8_t, uint32_t, float, float) {}
inline void sendComplete(bool) {}
}}
#endif
