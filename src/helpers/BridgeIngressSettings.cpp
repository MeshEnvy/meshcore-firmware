#include "BridgeIngressSettings.h"

#if MESH_ENABLE_BRIDGE_INGRESS
#include "CheckedSettingsFile.h"
#include "FilePresence.h"
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
#include "AtomicFileWriter.h"
#else
#include "ContactFileTransaction.h"
#endif
#include <stdio.h>
#include <string.h>

namespace mesh { namespace bridge {
namespace {
constexpr char Path[] = "/bridge_ingress";
constexpr size_t ImageSize = 10;
FILESYSTEM* settings_fs = nullptr;
IngressProfile selected = IngressProfile::Blocked;
bool writable = false;

uint32_t checksum(const uint8_t* data, size_t size) {
  uint32_t crc = 0xffffffffU;
  while (size--) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
  }
  return ~crc;
}

void encode(uint8_t image[ImageSize], IngressProfile profile) {
  memcpy(image, "BIN\1", 4);
  image[4] = static_cast<uint8_t>(profile);
  image[5] = image[4] ^ 0xff;
  const uint32_t crc = checksum(image, 6);
  for (uint8_t i = 0; i < 4; ++i) image[6 + i] = crc >> (i * 8);
}

bool decode(const uint8_t image[ImageSize], IngressProfile& profile) {
  if (memcmp(image, "BIN\1", 4) != 0 || image[4] > 1 ||
      image[5] != (image[4] ^ 0xff)) return false;
  const uint32_t saved_crc = uint32_t(image[6]) | (uint32_t(image[7]) << 8) |
      (uint32_t(image[8]) << 16) | (uint32_t(image[9]) << 24);
  if (saved_crc != checksum(image, 6)) return false;
  profile = static_cast<IngressProfile>(image[4]);
  return true;
}

void quarantine() {
  selected = IngressProfile::Blocked;
  writable = false;
}

bool saveSetting(IngressProfile profile) {
  if (!settings_fs || !writable) return false;
  uint8_t image[ImageSize];
  encode(image, profile);
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  AtomicFileWriter file(settings_fs, Path);
#else
  ContactFileTransaction file(settings_fs, Path, filePresence<FILESYSTEM>);
#endif
  using Outcome = decltype(file.outcome());
  if (!file || file.write(image, sizeof(image)) != sizeof(image)) {
    if (file.outcome() == Outcome::Indeterminate) quarantine();
    return false;
  }
  file.commit();
  if (file.outcome() == Outcome::NotCommitted) return false;
  if (file.outcome() != Outcome::Committed) {
    quarantine();
    return false;
  }
  uint8_t verified[ImageSize];
  if (!storage::readFixedSettingsFile(settings_fs, Path, verified, sizeof(verified)) ||
      memcmp(verified, image, sizeof(image)) != 0) {
    quarantine();
    return false;
  }
  selected = profile;
  return true;
}
} // namespace

IngressProfile ingressProfile() { return selected; }

void loadIngressSettings(FILESYSTEM* fs) {
  settings_fs = fs;
  quarantine();
  if (!fs) return;
#if defined(ESP32_PLATFORM) || defined(RP2040_PLATFORM)
  if (!ContactFileTransaction::recover(fs, Path, filePresence<FILESYSTEM>)) return;
#endif
  bool present = false;
  if (!filePresence(fs, Path, present)) return;
  if (!present) {
    selected = IngressProfile::Radio;
    writable = true;
    return;
  }
  uint8_t image[ImageSize];
  const auto read = storage::readFixedSettingsFileResult(fs, Path, image, sizeof(image));
  if (read == storage::FixedSettingsRead::Unreadable) return;
  // A known malformed record can be explicitly repaired. Until then it must
  // not silently turn a saved radio2-only ingress into primary-radio traffic.
  writable = true;
  if (read == storage::FixedSettingsRead::Complete) decode(image, selected);
}

bool handleIngressCommand(const char* command, char* reply, size_t capacity) {
  if (!command || !reply || !capacity) return false;
  const bool get = strncmp(command, "get bridge.ingress", 18) == 0 &&
      (command[18] == 0 || command[18] == ' ' || command[18] == '\t');
  const bool set = strncmp(command, "set bridge.ingress", 18) == 0 &&
      (command[18] == 0 || command[18] == ' ' || command[18] == '\t');
  if (!get && !set) return false;
  if (get) {
    if (command[18] != 0)
      snprintf(reply, capacity, "Error: use get bridge.ingress");
    else if (selected == IngressProfile::Blocked)
      snprintf(reply, capacity, "Error: bridge ingress blocked; settings invalid or unreadable");
    else
      snprintf(reply, capacity, "> %s", selected == IngressProfile::Radio2 ? "radio2" : "radio");
    return true;
  }
  const char* value = command + 18;
  while (*value == ' ' || *value == '\t') ++value;
  IngressProfile profile;
  if (strcmp(value, "radio") == 0) profile = IngressProfile::Radio;
  else if (strcmp(value, "radio2") == 0) profile = IngressProfile::Radio2;
  else {
    snprintf(reply, capacity, "Error: use set bridge.ingress radio|radio2");
    return true;
  }
  if (!writable) {
    snprintf(reply, capacity, "Error: bridge ingress storage unavailable; reboot first");
  } else if (!saveSetting(profile)) {
    snprintf(reply, capacity, "Error: bridge ingress not saved%s",
             writable ? "" : "; storage uncertain, ingress blocked until reload");
  } else {
    snprintf(reply, capacity, "OK - bridge.ingress %s", value);
  }
  return true;
}

}} // namespace mesh::bridge
#endif
