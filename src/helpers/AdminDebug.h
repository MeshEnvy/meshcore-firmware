#pragma once

// Opt-in admin/CLI tracing over Serial during bench bring-up. Compiles to nothing otherwise.
#if defined(ADMIN_DEBUG) && defined(ARDUINO)
  #include <Arduino.h>

  inline void admin_dbg_cmd(char* buf, size_t bufsz, const char* cmd) {
    if (!cmd || bufsz == 0) {
      if (bufsz > 0) buf[0] = 0;
      return;
    }
    size_t n = 0;
    while (cmd[n] && n + 1 < bufsz && n < 48) {
      buf[n] = cmd[n];
      n++;
    }
    buf[n] = 0;
    if (n >= 8 && memcmp(buf, "password", 8) == 0) {
      strcpy(buf, "password ***");
    } else if (n >= 14 && memcmp(buf, "set guest.pass", 14) == 0) {
      strcpy(buf, "set guest.password ***");
    }
  }

  #define ADMIN_DBG(...) do { Serial.print("\r"); Serial.printf(__VA_ARGS__); Serial.print("\r\n"); } while (0)
  #define ADMIN_DBG_MS(...) do { Serial.printf("\r[%lu] ", (unsigned long)millis()); Serial.printf(__VA_ARGS__); Serial.print("\r\n"); } while (0)
  #define ADMIN_DBG_CMD(label, cmd) do { \
      char _admin_cmd_buf[52]; \
      admin_dbg_cmd(_admin_cmd_buf, sizeof(_admin_cmd_buf), (cmd)); \
      ADMIN_DBG_MS("%s %s", (label), _admin_cmd_buf); \
    } while (0)
#else
  #define ADMIN_DBG(...) do {} while (0)
  #define ADMIN_DBG_MS(...) do {} while (0)
  #define ADMIN_DBG_CMD(label, cmd) do {} while (0)
#endif
