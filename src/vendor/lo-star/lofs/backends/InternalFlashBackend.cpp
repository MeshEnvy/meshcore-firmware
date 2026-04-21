#include "InternalFlashBackend.h"

#if defined(ESP32_PLATFORM)
#include <LittleFS.h>
#elif defined(RP2040_PLATFORM)
#include <LittleFS.h>
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
#include <InternalFileSystem.h>
#endif

namespace lofs {

InternalFlashBackend::InternalFlashBackend() { bindPlatformFs(); }

void InternalFlashBackend::bindPlatformFs() {
#if defined(ESP32_PLATFORM)
  _fs = &LittleFS;
#elif defined(RP2040_PLATFORM)
  _fs = &LittleFS;
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _fs = &InternalFS;
#else
  _fs = nullptr;
#endif
}

InternalFlashBackend& InternalFlashBackend::instance() {
  static InternalFlashBackend inst;
  return inst;
}

bool InternalFlashBackend::available() const { return _fs != nullptr; }

File InternalFlashBackend::open(const char* path, uint8_t mode) {
  if (!_fs || !path) return lofs::invalid_file();
#if defined(ESP32_PLATFORM) || defined(RP2040_PLATFORM)
  if (mode == FILE_O_READ) return _fs->open(path, "r");
  return _fs->open(path, "w", true);
#else
  return _fs->open(path, mode == FILE_O_READ ? FILE_O_READ : FILE_O_WRITE);
#endif
}

File InternalFlashBackend::open(const char* path, const char* mode) {
  if (!_fs || !path || !mode) return lofs::invalid_file();
#if defined(ESP32_PLATFORM) || defined(RP2040_PLATFORM)
  return _fs->open(path, mode, mode[0] == 'w');
#else
  return _fs->open(path, (mode[0] == 'r' && mode[1] == '\0') ? FILE_O_READ : FILE_O_WRITE);
#endif
}

bool InternalFlashBackend::exists(const char* path) {
  if (!_fs || !path) return false;
  return _fs->exists(path);
}

bool InternalFlashBackend::mkdir(const char* path) {
  if (!_fs || !path) return false;
  return _fs->mkdir(path);
}

bool InternalFlashBackend::remove(const char* path) {
  if (!_fs || !path) return false;
  return _fs->remove(path);
}

bool InternalFlashBackend::rename(const char* from, const char* to) {
  if (!_fs || !from || !to) return false;
  return _fs->rename(from, to);
}

static bool rmdir_one(lofs::InternalFlashBackend& self, lofs::FSys* fs, const char* path, bool recursive) {
  if (!fs || !path) return false;
  if (!recursive) return fs->rmdir(path);
  File dir = self.open(path, (uint8_t)FILE_O_READ);
  if (!dir) return false;
  if (!dir.isDirectory()) {
    dir.close();
    return false;
  }
  for (;;) {
    File f = dir.openNextFile();
    if (!f) break;
    char child[256];
    snprintf(child, sizeof(child), "%s/%s", path, f.name());
    if (f.isDirectory()) {
      f.close();
      rmdir_one(self, fs, child, true);
    } else {
      f.close();
      fs->remove(child);
    }
  }
  dir.close();
  return fs->rmdir(path);
}

bool InternalFlashBackend::rmdir(const char* path, bool recursive) {
  if (!_fs || !path) return false;
  return rmdir_one(*this, _fs, path, recursive);
}

uint64_t InternalFlashBackend::totalBytes() {
  if (!_fs) return 0;
#if defined(ESP32_PLATFORM) || defined(RP2040_PLATFORM)
  return LittleFS.totalBytes();
#else
  return 0;
#endif
}

uint64_t InternalFlashBackend::usedBytes() {
  if (!_fs) return 0;
#if defined(ESP32_PLATFORM) || defined(RP2040_PLATFORM)
  return LittleFS.usedBytes();
#else
  return 0;
#endif
}

}  // namespace lofs
