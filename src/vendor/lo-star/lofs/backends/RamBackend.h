#pragma once

#include <lofs/FsBackend.h>

#if defined(ESP32_PLATFORM)
#include <FS.h>
#include <memory>
#endif

namespace lofs {

#if defined(ESP32_PLATFORM)
class RamFSImpl;
#endif

/**
 * In-RAM filesystem backend for LoFS.
 *
 * Stores file data in heap-backed `std::map` keyed by normalized absolute path.
 * Reboots wipe everything. Enforces a total-bytes cap (`LOFS_RAM_CAP_BYTES`,
 * default 65536) so stuck callers cannot exhaust heap.
 *
 * Currently ESP32-only; other platforms return a non-available backend.
 */
class RamBackend : public FsBackend {
public:
  static RamBackend& instance();

  bool available() const override;
  File open(const char* path, uint8_t mode) override;
  File open(const char* path, const char* mode) override;
  bool exists(const char* path) override;
  bool mkdir(const char* path) override;
  bool remove(const char* path) override;
  bool rename(const char* from, const char* to) override;
  bool rmdir(const char* path, bool recursive) override;
  uint64_t totalBytes() override;
  uint64_t usedBytes() override;

private:
  RamBackend();

#if defined(ESP32_PLATFORM)
  std::shared_ptr<RamFSImpl> _impl;
  FSys _fs;
#endif
};

}  // namespace lofs
