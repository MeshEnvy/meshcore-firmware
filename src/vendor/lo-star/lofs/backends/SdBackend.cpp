#include "SdBackend.h"

#if defined(LOFS_HAS_SDCARD) && LOFS_HAS_SDCARD
#include <SD.h>
#include <SPI.h>
#endif

namespace lofs {

#if defined(LOFS_HAS_SDCARD) && LOFS_HAS_SDCARD
SdBackend::SdBackend() : _inited(false) {}
#else
SdBackend::SdBackend() = default;
#endif

SdBackend& SdBackend::instance() {
  static SdBackend inst;
  return inst;
}

#if defined(LOFS_HAS_SDCARD) && LOFS_HAS_SDCARD

bool SdBackend::ensure_sd() const {
  if (_inited) return SD.cardType() != CARD_NONE;
  _inited = true;
  SD.begin();
  return SD.cardType() != CARD_NONE;
}

bool SdBackend::available() const { return ensure_sd(); }

File SdBackend::open(const char* path, uint8_t mode) {
  if (!ensure_sd() || !path) return lofs::invalid_file();
  return SD.open(path, mode == FILE_O_READ ? FILE_READ : FILE_WRITE);
}

File SdBackend::open(const char* path, const char* mode) {
  if (!ensure_sd() || !path || !mode) return lofs::invalid_file();
  return SD.open(path, mode);
}

bool SdBackend::exists(const char* path) { return ensure_sd() && path && SD.exists(path); }

bool SdBackend::mkdir(const char* path) { return ensure_sd() && path && SD.mkdir(path); }

bool SdBackend::remove(const char* path) { return ensure_sd() && path && SD.remove(path); }

bool SdBackend::rename(const char* from, const char* to) {
  return ensure_sd() && from && to && SD.rename(from, to);
}

static bool rmdir_sd(const char* path, bool recursive) {
  if (!path) return false;
  if (!recursive) return SD.rmdir(path);
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  for (;;) {
    File f = dir.openNextFile();
    if (!f) break;
    char child[256];
    snprintf(child, sizeof(child), "%s/%s", path, f.name());
    if (f.isDirectory()) {
      f.close();
      rmdir_sd(child, true);
    } else {
      f.close();
      SD.remove(child);
    }
  }
  dir.close();
  return SD.rmdir(path);
}

bool SdBackend::rmdir(const char* path, bool recursive) {
  return ensure_sd() && path && rmdir_sd(path, recursive);
}

uint64_t SdBackend::totalBytes() {
  if (!ensure_sd()) return 0;
  return (uint64_t)SD.cardSize();
}

uint64_t SdBackend::usedBytes() { return 0; }

#else

bool SdBackend::available() const { return false; }
File SdBackend::open(const char*, uint8_t) { return lofs::invalid_file(); }
File SdBackend::open(const char*, const char*) { return lofs::invalid_file(); }
bool SdBackend::exists(const char*) { return false; }
bool SdBackend::mkdir(const char*) { return false; }
bool SdBackend::remove(const char*) { return false; }
bool SdBackend::rename(const char*, const char*) { return false; }
bool SdBackend::rmdir(const char*, bool) { return false; }
uint64_t SdBackend::totalBytes() { return 0; }
uint64_t SdBackend::usedBytes() { return 0; }

#endif

}  // namespace lofs
