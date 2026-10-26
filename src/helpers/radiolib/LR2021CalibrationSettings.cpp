#if defined(USE_LR2021)

#include "LR2021CalibrationSettings.h"
#include "LR2021CalibrationPolicy.h"
#include "CustomLR2021Wrapper.h"
#include <helpers/CheckedSettingsFile.h>
#include <stdio.h>
#include <string.h>

namespace mesh { namespace lr2021 {
namespace {
FILESYSTEM* settings_fs = nullptr;
XoscTrimSelection saved_selection;
XoscTrimSelection previous_selection;
bool pending = false;
bool failed = false;
bool rollback_failed = false;
uint32_t pending_started = 0;
constexpr uint32_t ApplyTimeoutMillis = 30000;
constexpr uint32_t ApplyHandoffMillis = 2000;
constexpr char path[] = "/radio_xtrim";
constexpr char backup[] = "/radio_xtrim.bak";
constexpr char temporary[] = "/radio_xtrim.tmp";

bool read(const char* name, XoscTrimSelection& selection) {
  uint8_t bytes[XoscTrimRecordSize];
  return storage::readFixedSettingsFile(settings_fs, name, bytes, sizeof(bytes))
      && decodeXoscTrim(bytes, sizeof(bytes), selection);
}

bool recover() {
  XoscTrimSelection checked;
  if (read(path, checked) || !read(backup, checked)) return true;
  if (settings_fs->exists(path) && !settings_fs->remove(path)) return false;
  return settings_fs->rename(backup, path) && read(path, checked);
}

bool save(const XoscTrimSelection& selection) {
  if (!settings_fs || !recover()) return false;
  if (settings_fs->exists(temporary) && !settings_fs->remove(temporary)) return false;
  if (settings_fs->exists(temporary)) return false;
  File file = settings_fs->open(temporary, "w", true);
  if (!file) return false;
  uint8_t bytes[XoscTrimRecordSize];
  encodeXoscTrim(selection, bytes);
  const bool written = file.write(bytes, sizeof(bytes)) == sizeof(bytes);
  file.close();
  XoscTrimSelection checked;
  if (!written || !read(temporary, checked) || !sameXoscTrim(selection, checked)) return false;
  if (settings_fs->exists(backup) && !settings_fs->remove(backup)) return false;
  if (settings_fs->exists(path) && !settings_fs->rename(path, backup)) return false;
  const bool published = settings_fs->rename(temporary, path);
  if (!published || !read(path, checked) || !sameXoscTrim(selection, checked)) {
    // A failed rename/readback may have changed the public record anyway.
    // Remove it before restoring the last complete setting, without touching
    // radio hardware until the new public record has been verified.
    if (settings_fs->exists(path)) settings_fs->remove(path);
    if (settings_fs->exists(backup)) settings_fs->rename(backup, path);
    return false;
  }
  if (settings_fs->exists(backup)) settings_fs->remove(backup);
  return true;
}

void failPending(CustomLR2021Wrapper& radio) {
  radio.cancelXoscTrimApply();
  pending = false;
  failed = true;
  rollback_failed = !save(previous_selection);
  if (!rollback_failed) saved_selection = previous_selection;
}

void formatSelection(char* reply, size_t capacity) {
  const char* suffix = pending ? " (pending)" : failed
      ? rollback_failed ? " (failed; saved rollback failed)" : " (failed; previous saved setting restored)" : "";
  if (saved_selection.enabled) {
    snprintf(reply, capacity, "> %u,%u%s", saved_selection.a, saved_selection.b, suffix);
  } else {
    snprintf(reply, capacity, "> default%s", suffix);
  }
}
}  // namespace

bool loadLR2021CalibrationSettings(FILESYSTEM* fs, CustomLR2021Wrapper& radio) {
  settings_fs = fs;
  saved_selection = XoscTrimSelection{};
  previous_selection = XoscTrimSelection{};
  pending = failed = rollback_failed = false;
  radio.cancelXoscTrimApply();
  if (!radio.supportsXoscTrim()) return false;
  if (fs) recover();
  const bool loaded = fs && (read(path, saved_selection) || read(backup, saved_selection));
  if (!radio.configureBootXoscTrim(saved_selection.enabled, saved_selection.a, saved_selection.b)) {
    failed = true;
    return false;
  }
  return loaded;
}

void loopLR2021CalibrationSettings(CustomLR2021Wrapper& radio) {
  if (!pending) return;
  const uint32_t elapsed = uint32_t(millis() - pending_started);
  // Let the acknowledgement leave with the previous calibration first.
  if (elapsed < ApplyHandoffMillis) return;
  const auto result = radio.trySetXoscTrim(saved_selection.enabled,
      saved_selection.a, saved_selection.b);
  if (result == RadioParamApplyResult::APPLIED) {
    pending = false;
    failed = rollback_failed = false;
  } else if (result == RadioParamApplyResult::FAILED
      || (result == RadioParamApplyResult::BUSY
          && elapsed - ApplyHandoffMillis >= ApplyTimeoutMillis)) {
    // Give the wrapper its bounded RX-only admission attempt at the deadline.
    // Protected work that remains busy must still terminate this request.
    failPending(radio);
  }
}

bool handleLR2021CalibrationCommand(const char* command, char* reply,
                                   size_t capacity, CustomLR2021Wrapper& radio) {
  if (!strcmp(command, "get radio.freqerror")) {
    radio.formatFrequencyError(reply, capacity);
    return true;
  }
  const bool get = !strncmp(command, "get ", 4);
  if (!get && strncmp(command, "set ", 4)) return false;
  constexpr char key[] = "radio.xtrim";
  constexpr size_t length = sizeof(key) - 1;
  if (strncmp(command + 4, key, length)
      || (command[4 + length] && command[4 + length] != ' ')) return false;
  if (!radio.supportsXoscTrim()) {
    snprintf(reply, capacity, "Error: radio.xtrim requires an LR2021 with a crystal, not a TCXO");
    return true;
  }
  const char* value = command + 4 + length;
  if (get) {
    if (*value) snprintf(reply, capacity, "Error: use get radio.xtrim");
    else formatSelection(reply, capacity);
    return true;
  }
  while (*value == ' ') ++value;
  XoscTrimSelection candidate;
  if (!parseXoscTrim(value, candidate)) {
    snprintf(reply, capacity, "Error: use set radio.xtrim <0-47>[,<0-47>] or default");
    return true;
  }
  if (sameXoscTrim(candidate, saved_selection) && !failed) {
    snprintf(reply, capacity, pending ? "OK - oscillator calibration pending" : "OK - oscillator calibration unchanged");
    return true;
  }
  if (!save(candidate)) {
    snprintf(reply, capacity, "Error: oscillator calibration save failed; radio unchanged");
    return true;
  }
  if (!pending) {
    previous_selection = XoscTrimSelection{};
    previous_selection.enabled = radio.getXoscTrim(previous_selection.a, previous_selection.b);
  }
  saved_selection = candidate;
  pending = true;
  failed = rollback_failed = false;
  pending_started = millis();
  snprintf(reply, capacity, "OK - oscillator calibration applying in 2s");
  return true;
}

}}  // namespace mesh::lr2021

#endif  // USE_LR2021
