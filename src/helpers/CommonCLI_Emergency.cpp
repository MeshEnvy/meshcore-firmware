#include "CommonCLI.h"

#if MESH_ENABLE_EMERGENCY_POWER
#include "CheckedSettingsFile.h"
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
#include "AtomicFileWriter.h"
#else
#include "ContactFileTransaction.h"
#endif

namespace {
const char* const EMERGENCY_POWER_PATH = "/power_emergency";

bool readEmergencyPower(FILESYSTEM* fs, const char* path,
                        mesh::emergencypower::Settings& settings) {
  uint8_t bytes[mesh::emergencypower::RecordSize];
  return mesh::storage::readFixedSettingsFile(fs, path, bytes, sizeof(bytes))
      && mesh::emergencypower::decode(bytes, sizeof(bytes), settings);
}

bool emergencyCommand(const char* command, const char* prefix, const char*& value) {
  const size_t length = strlen(prefix);
  if (strncmp(command, prefix, length) != 0
      || (command[length] && command[length] != ' ' && command[length] != '\t')) return false;
  value = command + length;
  while (*value == ' ' || *value == '\t') ++value;
  return true;
}

bool emergencyValue(const char* value, const char* expected) {
  const size_t length = strlen(expected);
  if (strncmp(value, expected, length) != 0) return false;
  value += length;
  while (*value == ' ' || *value == '\t') ++value;
  return !*value;
}
}

void CommonCLI::loadEmergencyPower(FILESYSTEM* fs) {
  _emergency_power_fs = fs;
  mesh::emergencypower::Settings loaded;
#if defined(ESP32_PLATFORM) || defined(RP2040_PLATFORM)
  if (!mesh::ContactFileTransaction::recover(fs, EMERGENCY_POWER_PATH))
    _emergency_power_fs = nullptr;
  else
#endif
  if (!readEmergencyPower(fs, EMERGENCY_POWER_PATH, loaded))
    readEmergencyPower(fs, "/power_emergency.bak", loaded);
  _emergency_power.begin(loaded, millis());
  _emergency_radio_sleeping = false;
}

bool CommonCLI::saveEmergencyPower(const mesh::emergencypower::Settings& settings) {
  if (!_emergency_power_fs) return false;
  uint8_t bytes[mesh::emergencypower::RecordSize];
  mesh::emergencypower::encode(bytes, settings);
#if defined(NRF52_PLATFORM)
  return mesh::storage::writeCheckedSettingsFile(_emergency_power_fs, EMERGENCY_POWER_PATH,
      bytes, sizeof(bytes)) == mesh::AtomicFileWriter::Outcome::Committed;
#else
#if defined(STM32_PLATFORM)
  mesh::AtomicFileWriter writer(_emergency_power_fs, EMERGENCY_POWER_PATH);
#else
  mesh::ContactFileTransaction writer(_emergency_power_fs, EMERGENCY_POWER_PATH);
#endif
  if (!writer || writer.write(bytes, sizeof(bytes)) != sizeof(bytes) || !writer.commit()) return false;
  uint8_t verified[sizeof(bytes)];
  return mesh::storage::readFixedSettingsFile(_emergency_power_fs, EMERGENCY_POWER_PATH,
      verified, sizeof(verified)) && memcmp(bytes, verified, sizeof(bytes)) == 0;
#endif
}

void CommonCLI::loopEmergencyPower() {
  const uint32_t now = millis();
  if (!_emergency_power.due(now)) return;
  const uint16_t mv = _board->getBattMilliVolts();
  _emergency_power.sample(now, mv, batteryPercent(mv),
      mesh::batteryforward::validEndpoints(batteryEmptyMv(), batteryFullMv()));
}

uint32_t CommonCLI::emergencySleepMilliseconds() {
  loopEmergencyPower();
  return _emergency_power.sleepMilliseconds(millis(), _rtc->getCurrentTime());
}

bool CommonCLI::handleEmergencyPowerCommand(const char* command, char* reply) {
  if (!command || !reply) return false;
  const char* value;
  if (emergencyCommand(command, "get power.emergency", value)) {
    if (*value) snprintf(reply, 157, "Error: use get power.emergency");
    else {
      const uint32_t sleep_ms = emergencySleepMilliseconds();
      char battery[12] = "unknown";
      if (_emergency_power.valid)
        snprintf(battery, sizeof(battery), "%u%%", unsigned(_emergency_power.percentage));
      snprintf(reply, 157, "> mode=%s active=%s battery=%s clock=%s window=%s radio=%s wake=%lus hold=%lus",
          _emergency_power.settings.manual ? "manual" : "auto", emergencyActive() ? "on" : "off",
          battery, mesh::emergencypower::State::usesRtc(_rtc->getCurrentTime()) ? "rtc" : "uptime",
          sleep_ms ? "sleep" : "awake", _emergency_radio_sleeping ? "sleep" : "awake",
          static_cast<unsigned long>((sleep_ms + 999UL) / 1000UL),
          static_cast<unsigned long>((_emergency_power.holdRemaining(millis()) + 999UL) / 1000UL));
    }
    return true;
  }
  if (emergencyCommand(command, "set power.emergency", value)) {
    const bool manual = emergencyValue(value, "on");
    if (!manual && !emergencyValue(value, "auto")) {
      snprintf(reply, 157, "Error: use set power.emergency on|auto");
      return true;
    }
    if (manual && !_board->supportsEmergencySleep()) {
      snprintf(reply, 157, "Error: emergency sleep unsupported on this board");
      return true;
    }
    mesh::emergencypower::Settings candidate;
    candidate.manual = manual;
    if (candidate.manual != _emergency_power.settings.manual && !saveEmergencyPower(candidate))
      snprintf(reply, 157, "Error: emergency settings save unconfirmed; active policy unchanged");
    else {
      _emergency_power.setManual(manual, millis());
      snprintf(reply, 157, "OK - emergency %s", manual ? "on" : "auto");
    }
    return true;
  }
  if (!emergencyCommand(command, "set power.awake", value)) return false;
  uint16_t minutes = 0;
  const char* p = value;
  bool digits = false;
  while (*p >= '0' && *p <= '9') {
    digits = true;
    const unsigned number = unsigned(minutes) * 10U + unsigned(*p++ - '0');
    if (number > mesh::emergencypower::MaximumAwakeMinutes) {
      snprintf(reply, 157, "Error: use set power.awake 0-10080 (minutes)");
      return true;
    }
    minutes = uint16_t(number);
  }
  while (*p == ' ' || *p == '\t') ++p;
  if (!digits || *p) snprintf(reply, 157, "Error: use set power.awake 0-10080 (minutes)");
  else {
    _emergency_power.stayAwake(minutes, millis());
    snprintf(reply, 157, "OK - awake for %u minutes", unsigned(minutes));
  }
  return true;
}
#endif // MESH_ENABLE_EMERGENCY_POWER
