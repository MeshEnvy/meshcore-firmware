#include "ExternalFlashBackend.h"
#include "InternalFlashBackend.h"

extern "C" __attribute__((weak)) lofs::FSys* lofs_variant_external_fs(void) { return nullptr; }

namespace lofs {

ExternalFlashBackend::ExternalFlashBackend() { bindPlatformFs(); }

void ExternalFlashBackend::bindPlatformFs() {
#if defined(ESP32_PLATFORM) || defined(RP2040_PLATFORM)
  _delegate_internal = true;
  _fs = nullptr;
#else
  _delegate_internal = false;
  _fs = lofs_variant_external_fs();
#endif
}

ExternalFlashBackend& ExternalFlashBackend::instance() {
  static ExternalFlashBackend inst;
  return inst;
}

bool ExternalFlashBackend::available() const {
  if (_delegate_internal) return InternalFlashBackend::instance().available();
  return _fs != nullptr;
}

File ExternalFlashBackend::open(const char* path, uint8_t mode) {
  if (_delegate_internal) return InternalFlashBackend::instance().open(path, mode);
  if (!_fs) return lofs::invalid_file();
#if defined(ESP32_PLATFORM) || defined(RP2040_PLATFORM)
  return _fs->open(path, mode == FILE_O_READ ? "r" : "w");
#else
  return _fs->open(path, mode == FILE_O_READ ? FILE_O_READ : FILE_O_WRITE);
#endif
}

File ExternalFlashBackend::open(const char* path, const char* mode) {
  if (_delegate_internal) return InternalFlashBackend::instance().open(path, mode);
  if (!_fs) return lofs::invalid_file();
#if defined(ESP32_PLATFORM) || defined(RP2040_PLATFORM)
  return _fs->open(path, mode);
#else
  return _fs->open(path, (mode && mode[0] == 'r' && mode[1] == '\0') ? FILE_O_READ : FILE_O_WRITE);
#endif
}

bool ExternalFlashBackend::exists(const char* path) {
  if (_delegate_internal) return InternalFlashBackend::instance().exists(path);
  return _fs && path && _fs->exists(path);
}

bool ExternalFlashBackend::mkdir(const char* path) {
  if (_delegate_internal) return InternalFlashBackend::instance().mkdir(path);
  return _fs && path && _fs->mkdir(path);
}

bool ExternalFlashBackend::remove(const char* path) {
  if (_delegate_internal) return InternalFlashBackend::instance().remove(path);
  return _fs && path && _fs->remove(path);
}

bool ExternalFlashBackend::rename(const char* from, const char* to) {
  if (_delegate_internal) return InternalFlashBackend::instance().rename(from, to);
  return _fs && from && to && _fs->rename(from, to);
}

static bool rmdir_ext(FSys* fs, ExternalFlashBackend& self, const char* path, bool recursive) {
  if (!fs || !path) return false;
  if (!recursive) return fs->rmdir(path);
  File dir = self.open(path, (uint8_t)FILE_O_READ);
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
      rmdir_ext(fs, self, child, true);
    } else {
      f.close();
      fs->remove(child);
    }
  }
  dir.close();
  return fs->rmdir(path);
}

bool ExternalFlashBackend::rmdir(const char* path, bool recursive) {
  if (_delegate_internal) return InternalFlashBackend::instance().rmdir(path, recursive);
  if (!_fs) return false;
  return rmdir_ext(_fs, *this, path, recursive);
}

uint64_t ExternalFlashBackend::totalBytes() {
  if (_delegate_internal) return InternalFlashBackend::instance().totalBytes();
  return 0;
}

uint64_t ExternalFlashBackend::usedBytes() {
  if (_delegate_internal) return InternalFlashBackend::instance().usedBytes();
  return 0;
}

}  // namespace lofs
