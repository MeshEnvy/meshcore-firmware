#include "HostKissSettings.h"
#if MESH_ENABLE_HOST_KISS
#include "CheckedSettingsFile.h"
#include "FilePresence.h"
#include "PersistentStoreFormat.h"
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
#include "AtomicFileWriter.h"
#else
#include "ContactFileTransaction.h"
#endif

namespace mesh { namespace hostkiss {
namespace {
constexpr char Path[] = "/host_kiss";
constexpr size_t ImageSize = 12;
void encode(uint8_t (&image)[ImageSize], const Selection& value) {
  memset(image, 0, sizeof(image));
  memcpy(image, "HKS1", 4);
  image[4] = value.enabled ? 1 : 0;
  image[5] = uint8_t(value.transport);
  storage::writeLE32(image + 8, storage::updateCRC32(0xffffffff, image, 8));
}
bool valid(const uint8_t (&image)[ImageSize]) {
  return memcmp(image, "HKS1", 4) == 0 && image[4] <= 1 && image[5] <= 1
      && image[6] == 0 && image[7] == 0
      && storage::readLE32(image + 8) == storage::updateCRC32(0xffffffff, image, 8);
}
}
bool Settings::begin(FILESYSTEM* fs) {
  fs_ = fs;
  healthy_ = false;
  selection_ = {};
  if (!fs_) return false;
#if defined(ESP32_PLATFORM) || defined(RP2040_PLATFORM) || defined(ESP32)
  if (!ContactFileTransaction::recover(fs_, Path, filePresence<FILESYSTEM>)) return false;
#endif
  bool present = false;
  if (!filePresence(fs_, Path, present)) return false;
  if (!present) { healthy_ = true; return true; }
  uint8_t image[ImageSize];
  const auto result = storage::readFixedSettingsFileResult(fs_, Path, image, sizeof(image));
  if (result == storage::FixedSettingsRead::Unreadable) return false;
  healthy_ = true;  // A complete malformed record may be explicitly replaced.
  if (result != storage::FixedSettingsRead::Complete || !valid(image)) return false;
  selection_.enabled = image[4] != 0;
  selection_.transport = Transport(image[5]);
  return true;
}
bool Settings::save(const Selection& selection) {
  if (!healthy_ || !fs_ || uint8_t(selection.transport) > 1) return false;
  uint8_t image[ImageSize];
  encode(image, selection);
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  AtomicFileWriter writer(fs_, Path);
  const bool wrote = writer && writer.write(image, sizeof(image)) == sizeof(image);
  const bool saved = writer.commit(wrote);
  if (writer.outcome() == AtomicFileWriter::Outcome::Indeterminate) healthy_ = false;
  if (!saved) return false;
#else
  ContactFileTransaction writer(fs_, Path, filePresence<FILESYSTEM>);
  if (!writer || writer.write(image, sizeof(image)) != sizeof(image)) {
    if (writer.outcome() == ContactFileTransaction::Outcome::Indeterminate) healthy_ = false;
    return false;
  }
  writer.commit();
  if (writer.outcome() == ContactFileTransaction::Outcome::NotCommitted) return false;
#endif
  uint8_t verified[ImageSize];
  healthy_ = storage::readFixedSettingsFile(fs_, Path, verified, sizeof(verified))
      && memcmp(image, verified, sizeof(image)) == 0;
  if (!healthy_) return false;
  selection_ = selection;
  return true;
}
}}  // namespace mesh::hostkiss
#endif
