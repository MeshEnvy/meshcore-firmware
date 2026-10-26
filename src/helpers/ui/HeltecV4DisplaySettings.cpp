#if defined(HELTEC_V4_DISPLAY_UNIFIED) && HELTEC_V4_DISPLAY_UNIFIED

#include "HeltecV4DisplaySettings.h"
#include <helpers/CheckedSettingsFile.h>
#include <stdio.h>
#include <string.h>

namespace mesh { namespace ui {
namespace {
FILESYSTEM* settings_fs = nullptr;
bool saved_mode = false;
constexpr char path[] = "/display_type";
constexpr char backup[] = "/display_type.bak";
constexpr char temporary[] = "/display_type.tmp";

void encode(HeltecV4DisplayMode mode, uint8_t (&data)[6]) {
  data[0] = 'D'; data[1] = 'T'; data[2] = 1;
  data[3] = uint8_t(mode); data[4] = 0; data[5] = 0;
  for (unsigned i = 0; i < 5; ++i) data[5] ^= data[i];
}

bool read(const char* name, HeltecV4DisplayMode& mode) {
  uint8_t data[6], expected[6];
  if (!storage::readFixedSettingsFile(settings_fs, name, data, sizeof(data))
      || data[3] > uint8_t(HeltecV4DisplayMode::Off)) return false;
  const auto candidate = HeltecV4DisplayMode(data[3]);
  encode(candidate, expected);
  if (memcmp(data, expected, sizeof(data)) != 0) return false;
  mode = candidate;
  return true;
}

bool recover() {
  HeltecV4DisplayMode mode;
  if (read(path, mode) || !read(backup, mode)) return true;
  if (settings_fs->exists(path) && !settings_fs->remove(path)) return false;
  return settings_fs->rename(backup, path) && read(path, mode);
}

bool save(HeltecV4DisplayMode mode) {
  if (!settings_fs || !recover()) return false;
  if (settings_fs->exists(temporary) && !settings_fs->remove(temporary)) return false;
  if (settings_fs->exists(temporary)) return false;
  File file = settings_fs->open(temporary, "w", true);
  if (!file) return false;
  uint8_t data[6];
  encode(mode, data);
  const bool written = file.write(data, sizeof(data)) == sizeof(data);
  file.close();
  HeltecV4DisplayMode checked;
  if (!written || !read(temporary, checked) || checked != mode) return false;
  if (settings_fs->exists(backup) && !settings_fs->remove(backup)) return false;
  if (settings_fs->exists(path) && !settings_fs->rename(path, backup)) return false;
  if (!settings_fs->rename(temporary, path)) {
    // A failed publish must not discard the last complete preference record.
    if (settings_fs->exists(backup) && !settings_fs->exists(path))
      settings_fs->rename(backup, path);
    return false;
  }
  if (!read(path, checked) || checked != mode) return false;
  if (settings_fs->exists(backup)) settings_fs->remove(backup);
  return true;
}

const char* modeName(HeltecV4DisplayMode mode) {
  switch (mode) {
    case HeltecV4DisplayMode::Oled: return "oled";
    case HeltecV4DisplayMode::Tft: return "tft";
    case HeltecV4DisplayMode::Off: return "off";
    default: return "auto";
  }
}
}  // namespace

bool loadHeltecV4DisplaySettings(FILESYSTEM* fs, HeltecV4Display& display) {
  settings_fs = fs;
  saved_mode = false;
  HeltecV4DisplayMode mode = HeltecV4DisplayMode::Auto;
  if (fs) {
    recover();
    saved_mode = read(path, mode) || read(backup, mode);
  }
  display.setMode(mode);
  return saved_mode;
}

bool handleHeltecV4DisplayCommand(const char* command, char* reply,
                                 size_t reply_size, HeltecV4Display& display) {
  const bool get = strncmp(command, "get ", 4) == 0;
  if (!get && strncmp(command, "set ", 4) != 0) return false;
  constexpr char key[] = "display.type";
  constexpr size_t length = sizeof(key) - 1;
  if (strncmp(command + 4, key, length) != 0
      || (command[4 + length] != 0 && command[4 + length] != ' ')) return false;
  const char* value = command + 4 + length;
  if (get) {
    if (*value) snprintf(reply, reply_size, "Error: unexpected argument");
    else snprintf(reply, reply_size, "display.type %s (active %s)",
                  modeName(display.mode()), display.status());
    return true;
  }
  while (*value == ' ') ++value;
  HeltecV4DisplayMode mode;
  if (!strcmp(value, "auto")) mode = HeltecV4DisplayMode::Auto;
  else if (!strcmp(value, "oled")) mode = HeltecV4DisplayMode::Oled;
  else if (!strcmp(value, "tft")) mode = HeltecV4DisplayMode::Tft;
  else if (!strcmp(value, "off")) mode = HeltecV4DisplayMode::Off;
  else {
    snprintf(reply, reply_size, "Error: use set display.type <auto|oled|tft|off>");
    return true;
  }
  if ((!saved_mode || display.mode() != mode) && !save(mode)) {
    snprintf(reply, reply_size, "Error: display type save failed");
    return true;
  }
  saved_mode = true;
  display.setMode(mode);
  snprintf(reply, reply_size, "OK - display.type %s (active %s)",
           modeName(mode), display.status());
  return true;
}

}}  // namespace mesh::ui

#endif  // HELTEC_V4_DISPLAY_UNIFIED
