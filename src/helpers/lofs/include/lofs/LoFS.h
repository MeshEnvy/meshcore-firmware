#pragma once

#include <lofs/FsBackend.h>

#define LOFS_VERSION "0.3.0-meshcore"

class LoFS {
public:
  static bool mount(const char* prefix, lofs::FsBackend* backend);
  static bool unmount(const char* prefix);
  static lofs::FsBackend* resolveBackend(const char* virtual_path, char* stripped_out, size_t stripped_cap);
  static void mountDefaults();

  static File open(const char* filepath, uint8_t mode);
  static File open(const char* filepath, const char* mode);
  static bool exists(const char* filepath);
  static bool mkdir(const char* filepath);
  static bool remove(const char* filepath);
  static bool rename(const char* oldfilepath, const char* newfilepath);
  static bool rmdir(const char* filepath, bool recursive = false);
  static uint64_t totalBytes(const char* filepath);
  static uint64_t usedBytes(const char* filepath);
  static uint64_t freeBytes(const char* filepath);

  static bool isSDCardAvailable();
};
