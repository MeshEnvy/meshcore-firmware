#pragma once

#include "CLICommandUtils.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mesh { namespace companion {

// The radio owns RXEN admission and receiver restoration. This command adapter
// does not stop RX, change preferences, or bypass an unread packet/other owner.
template<bool AllowRfRxSet = true, typename Driver, typename ClearStats>
bool handleRadioDiagnosticsCommand(Driver& driver, bool radio_available,
                                   const char* command, char* reply,
                                   size_t capacity, ClearStats clear_stats) {
  if (!command || !reply || capacity == 0) return false;
  if (strcmp(command, "get rxps.wd") == 0) {
    snprintf(reply, capacity, "> soft=%lu,hard=%lu",
             (unsigned long)driver.getRxPsWatchdogSoftCount(),
             (unsigned long)driver.getRxPsWatchdogHardCount());
    return true;
  }
  if (strcmp(command, "clear stats") == 0) {
    // Match infrastructure: radio packet counts, Dispatcher packet counts and
    // sticky error flags, and table duplicate counters. Watchdog history,
    // receive timestamps, queues, and duplicate-detection entries are retained.
    clear_stats();
    snprintf(reply, capacity, "(OK - stats reset)");
    return true;
  }
  if (strcmp(command, "get radio.rxps.rfrx_disabled") == 0) {
    if (!driver.supportsRxPowerSavingRfRxDisable()) {
      cli::formatError(reply, capacity, "unsupported");
    } else if (!radio_available) {
      cli::formatError(reply, capacity, "radio unavailable");
    } else {
      snprintf(reply, capacity, "> %s",
               driver.isRxPowerSavingRfRxDisabled() ? "on" : "off");
    }
    return true;
  }
  if (!AllowRfRxSet) return false;
  static const char set_key[] = "set radio.rxps.rfrx_disabled";
  if (strncmp(command, set_key, sizeof(set_key) - 1) != 0
      || (command[sizeof(set_key) - 1] != 0
          && command[sizeof(set_key) - 1] != ' '
          && command[sizeof(set_key) - 1] != '\t')) return false;
  const char* value = command + sizeof(set_key) - 1;
  if (*value == ' ') ++value;
  if (strcmp(value, "on") != 0 && strcmp(value, "off") != 0) {
    cli::formatError(reply, capacity, "use set radio.rxps.rfrx_disabled on|off");
  } else if (!driver.supportsRxPowerSavingRfRxDisable()) {
    cli::formatError(reply, capacity, "unsupported");
  } else if (!radio_available) {
    cli::formatError(reply, capacity, "radio unavailable");
  } else if (!driver.setRxPowerSavingRfRxDisabled(strcmp(value, "on") == 0)) {
    cli::formatError(reply, capacity, "radio busy or RXEN change failed");
  } else {
    snprintf(reply, capacity, "OK - radio.rxps.rfrx_disabled %s", value);
  }
  return true;
}

inline void formatCadDiagnostics(char* reply, size_t capacity, bool enabled,
                                 const char* scan, const char* retry,
                                 const char* maximum, uint32_t busy_count) {
  if (!reply || capacity == 0) return;
  snprintf(reply, capacity,
           "radio.cad %s, scan=%s ms, retry=%s ms, max=%s ms, busy=%lu",
           enabled ? "on" : "off", scan, retry, maximum,
           (unsigned long)busy_count);
}

} } // namespace mesh::companion
