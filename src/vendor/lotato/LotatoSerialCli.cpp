#include "LotatoSerialCli.h"

#include <Arduino.h>

void lotato_serial_print_mesh_cli_reply(const char* reply) {
  if (!reply) return;
  Serial.print("  -> ");
#if defined(ARDUINO_ARCH_ESP32)
  constexpr size_t kChunk = 48;
  size_t n = 0;
  for (const char* p = reply; *p != '\0'; ++p) {
    Serial.write(static_cast<uint8_t>(*p));
    if (++n % kChunk == 0) {
      yield();
    }
  }
  Serial.print("\r\n");
#else
  Serial.println(reply);
#endif
}
