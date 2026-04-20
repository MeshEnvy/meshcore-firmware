#pragma once

#include <Arduino.h>

#if defined(ESP32) || defined(ESP32_PLATFORM) || defined(RP2040_PLATFORM)
#include <FS.h>
namespace lofs_fs {
using FSys = fs::FS;
using FileT = fs::File;
}  // namespace lofs_fs
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
namespace lofs_fs {
using FSys = Adafruit_LittleFS;
using FileT = Adafruit_LittleFS_Namespace::File;
}  // namespace lofs_fs
using namespace Adafruit_LittleFS_Namespace;
#else
#error "lofs: unsupported platform"
#endif

namespace lofs {

using File = lofs_fs::FileT;
using FSys = lofs_fs::FSys;

/** Invalid/unopened File handle for backends that need to signal failure. */
inline File invalid_file() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return File(InternalFS);
#else
  return File();
#endif
}

/** Per-mount filesystem adapter (internal flash, external flash, SD, …). */
class FsBackend {
public:
  virtual ~FsBackend() = default;
  virtual bool available() const = 0;
  virtual File open(const char* path, uint8_t mode) = 0;
  virtual File open(const char* path, const char* mode) = 0;
  virtual bool exists(const char* path) = 0;
  virtual bool mkdir(const char* path) = 0;
  virtual bool remove(const char* path) = 0;
  /** Native rename on this backend only (same volume). */
  virtual bool rename(const char* from, const char* to) = 0;
  virtual bool rmdir(const char* path, bool recursive) = 0;
  virtual uint64_t totalBytes() = 0;
  virtual uint64_t usedBytes() = 0;
};

}  // namespace lofs

#ifndef FILE_O_READ
#define FILE_O_READ ((uint8_t)0)
#endif
#ifndef FILE_O_WRITE
#define FILE_O_WRITE ((uint8_t)1)
#endif
