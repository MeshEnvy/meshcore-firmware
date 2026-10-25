#include "DisplayNightSettings.h"

#ifdef MESHCORE_HAS_COLOR_DISPLAY
#include "DisplayNightPolicy.h"
#include <helpers/CheckedSettingsFile.h>
#include <stdio.h>
#include <string.h>

namespace mesh { namespace ui {
namespace {
FILESYSTEM* settings_fs = nullptr;
constexpr char path[] = "/display_night";
constexpr char backup[] = "/display_night.bak";
uint8_t mode = NightAuto;
int16_t delay_minutes = 60;
bool active = false, has_position = false;
uint32_t observed_utc = 0, evaluated_minute = UINT32_MAX;
int32_t latitude = 0, longitude = 0;

void evaluate() {
  active = mode == NightOn || (mode == NightAuto && has_position
      && night_policy::active(observed_utc, latitude, longitude, delay_minutes));
}

void encode(uint8_t selected, int16_t delay, uint8_t (&data)[8]) {
  data[0] = 'D'; data[1] = 'N'; data[2] = 1; data[3] = selected;
  const uint16_t encoded = uint16_t(delay + 180);
  data[4] = encoded & 255; data[5] = encoded >> 8;
  data[6] = data[7] = 0;
  for (unsigned i = 0; i < 7; ++i) data[7] ^= data[i];
}

bool read(const char* name, uint8_t (&data)[8]) {
  if (!mesh::storage::readFixedSettingsFile(settings_fs, name, data, sizeof(data))) return false;
  const uint16_t encoded = uint16_t(data[4]) | (uint16_t(data[5]) << 8);
  uint8_t expected[8];
  encode(data[3], int16_t(encoded) - 180, expected);
  return data[3] <= NightAuto && encoded <= 360
      && memcmp(data, expected, sizeof(data)) == 0;
}

bool recoverBackup() {
  uint8_t current[8], saved[8];
  if (read(path, current) || !read(backup, saved)) return true;
  // Never rotate a corrupt primary over the only valid record on a later save.
  if (settings_fs->exists(path) && !settings_fs->remove(path)) return false;
  if (!settings_fs->rename(backup, path)) return false;
  return read(path, current) && !memcmp(current, saved, sizeof(current));
}

bool save(uint8_t selected, int16_t delay) {
  if (!settings_fs || !recoverBackup()) return false;
  uint8_t data[8];
  encode(selected, delay, data);
#if defined(NRF52_PLATFORM)
  const bool committed = mesh::storage::writeCheckedSettingsFile(settings_fs, path, data, sizeof(data))
      == mesh::AtomicFileWriter::Outcome::Committed;
  if (committed) settings_fs->remove(backup);
  return committed;
#else
  constexpr char temporary[] = "/display_night.tmp";
  settings_fs->remove(temporary);
  if (settings_fs->exists(temporary)) return false;
#if defined(STM32_PLATFORM)
  File file(*settings_fs);
  if (!file.open(temporary, FILE_O_WRITE)) return false;
#elif defined(RP2040_PLATFORM)
  File file = settings_fs->open(temporary, "w");
#else
  File file = settings_fs->open(temporary, "w", true);
#endif
  if (!file) return false;
  const bool written = file.write(data, sizeof(data)) == sizeof(data);
  file.close();
  uint8_t checked[8];
  if (!written || !read(temporary, checked) || memcmp(data, checked, sizeof(data))) return false;
  if (settings_fs->exists(backup) && !settings_fs->remove(backup)) return false;
  if (settings_fs->exists(path) && !settings_fs->rename(path, backup)) return false;
  if (!settings_fs->rename(temporary, path)) {
    if (settings_fs->exists(backup)) settings_fs->rename(backup, path);
    return false;
  }
  if (!read(path, checked) || memcmp(data, checked, sizeof(data))) return false;
  settings_fs->remove(backup);
  return true;
#endif
}
}  // namespace

uint8_t displayNightMode() { return mode; }
int16_t displayNightDelayMinutes() { return delay_minutes; }
bool displayNightActive() { return active; }

void loadDisplayNightSettings(FILESYSTEM* fs) {
  settings_fs = fs;
  mode = NightAuto;
  delay_minutes = 60;
  if (fs) recoverBackup();
  uint8_t data[8];
  if (read(path, data) || read(backup, data)) {
    mode = data[3];
    delay_minutes = int16_t(uint16_t(data[4]) | (uint16_t(data[5]) << 8)) - 180;
  }
  evaluated_minute = UINT32_MAX;
  evaluate();
}

void updateDisplayNightMode(uint32_t utc, bool has_location, int32_t latitude_e6,
                            int32_t longitude_e6) {
  observed_utc = utc;
  const bool usable = has_location && night_policy::validLocation(latitude_e6, longitude_e6);
  const bool changed = usable != has_position;
  has_position = usable;
  latitude = latitude_e6;
  longitude = longitude_e6;
  if (!changed && evaluated_minute == utc / 60) return;
  evaluated_minute = utc / 60;
  evaluate();
}

bool handleDisplayNightCommand(const char* command, char* reply, size_t capacity) {
  const bool get = strncmp(command, "get ", 4) == 0;
  if (!get && strncmp(command, "set ", 4) != 0) return false;
  const char* key = command + 4;
  bool delay = false;
  size_t length = 13;
  if (!strncmp(key, "display.night.delay", 19)) { delay = true; length = 19; }
  else if (strncmp(key, "display.night", 13)) return false;
  if (key[length] && key[length] != ' ') return false;
  const char* value = key + length;
  static const char* const names[] = {"off", "on", "auto"};
  if (get) {
    if (*value) snprintf(reply, capacity, "Error: unexpected argument");
    else if (delay) snprintf(reply, capacity, "> %d", int(delay_minutes));
    else snprintf(reply, capacity, "> %s", names[mode]);
    return true;
  }
  while (*value == ' ') ++value;
  uint8_t selected = mode;
  int16_t minutes = delay_minutes;
  if (delay) {
    const bool negative = *value == '-';
    if (negative || *value == '+') ++value;
    unsigned magnitude = 0;
    bool valid = *value != 0;
    for (; valid && *value; ++value) {
      valid = *value >= '0' && *value <= '9';
      if (valid) { magnitude = magnitude * 10 + (*value - '0'); valid = magnitude <= 180; }
    }
    if (!valid) { snprintf(reply, capacity, "Error: delay must be -180..180 minutes"); return true; }
    minutes = negative ? -int(magnitude) : int(magnitude);
  } else {
    for (selected = NightOff; selected <= NightAuto; ++selected)
      if (!strcmp(value, names[selected])) break;
    if (selected > NightAuto) { snprintf(reply, capacity, "Error: use auto|on|off"); return true; }
  }
  if (!save(selected, minutes)) snprintf(reply, capacity, "Error: night settings save failed");
  else {
    mode = selected;
    delay_minutes = minutes;
    evaluate();
    snprintf(reply, capacity, "OK");
  }
  return true;
}

} }
#endif
