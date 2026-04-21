#pragma once

#include <lofs/FsBackend.h>

namespace lofs {

class ExternalFlashBackend : public FsBackend {
public:
  static ExternalFlashBackend& instance();

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
  ExternalFlashBackend();
  void bindPlatformFs();
  bool _delegate_internal = false;
  FSys* _fs = nullptr;
};

}  // namespace lofs

extern "C" lofs::FSys* lofs_variant_external_fs(void) __attribute__((weak));
