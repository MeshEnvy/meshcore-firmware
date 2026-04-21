#include <lofs/LoFS.h>

#include "backends/ExternalFlashBackend.h"
#include "backends/InternalFlashBackend.h"
#include "backends/RamBackend.h"
#include "backends/SdBackend.h"

#include <cstring>

#if defined(ESP32_PLATFORM)
#include <freertos/semphr.h>
static SemaphoreHandle_t s_lofs_mtx;
static void lofs_lock() {
  if (!s_lofs_mtx) s_lofs_mtx = xSemaphoreCreateMutex();
  if (s_lofs_mtx) xSemaphoreTake(s_lofs_mtx, portMAX_DELAY);
}
static void lofs_unlock() {
  if (s_lofs_mtx) xSemaphoreGive(s_lofs_mtx);
}
#else
static void lofs_lock() {}
static void lofs_unlock() {}
#endif

namespace {

struct MountEntry {
  char prefix[24]{};
  lofs::FsBackend* backend = nullptr;
};

MountEntry s_mounts[8];
uint8_t s_nmounts = 0;

bool is_mount_prefix(const char* path, const char* pref, size_t plen) {
  if (strncmp(path, pref, plen) != 0) return false;
  return path[plen] == '/' || path[plen] == '\0';
}

/** Normalize user path to a virtual absolute path starting with a mount prefix. */
bool normalize_virtual(const char* in, char* out, size_t cap) {
  if (!in || !out || cap < 8) return false;
  while (*in == ' ') in++;
  if (*in == '\0') return false;

  if (in[0] != '/') {
    int n = snprintf(out, cap, "/__int__/%s", in);
    return n > 0 && (size_t)n < cap;
  }
  if (is_mount_prefix(in, "/__int__", 8) || is_mount_prefix(in, "/__ext__", 8) ||
      is_mount_prefix(in, "/__sd__", 7) || is_mount_prefix(in, "/__ram__", 8)) {
    strncpy(out, in, cap - 1);
    out[cap - 1] = '\0';
    return true;
  }
  int n = snprintf(out, cap, "/__int__%s", in);
  return n > 0 && (size_t)n < cap;
}

bool resolve_locked(const char* virtual_path, lofs::FsBackend** backend, char* stripped, size_t stripped_cap) {
  int best_len = -1;
  uint8_t best_i = 255;
  for (uint8_t i = 0; i < s_nmounts; i++) {
    size_t pl = strlen(s_mounts[i].prefix);
    if (!is_mount_prefix(virtual_path, s_mounts[i].prefix, pl)) continue;
    if ((int)pl > best_len) {
      best_len = (int)pl;
      best_i = i;
    }
  }
  if (best_i == 255 || !backend) return false;
  const char* p = virtual_path + best_len;
  if (*p == '/') p++;
  if (*p == '\0') {
    if (stripped_cap < 2) return false;
    stripped[0] = '/';
    stripped[1] = '\0';
  } else {
    if (*p != '/') {
      if (stripped_cap < 2) return false;
      stripped[0] = '/';
      strncpy(stripped + 1, p, stripped_cap - 2);
      stripped[stripped_cap - 1] = '\0';
    } else {
      strncpy(stripped, p, stripped_cap - 1);
      stripped[stripped_cap - 1] = '\0';
    }
  }
  *backend = s_mounts[best_i].backend;
  return *backend != nullptr;
}

constexpr size_t kCopyBuf = 512;

bool copy_stream(lofs::FsBackend* from_b, const char* from_p, lofs::FsBackend* to_b, const char* to_p) {
  if (!from_b || !to_b || !from_p || !to_p) return false;
  File in = from_b->open(from_p, FILE_O_READ);
  if (!in) return false;
  File out = to_b->open(to_p, FILE_O_WRITE);
  if (!out) {
    in.close();
    return false;
  }
  uint8_t buf[kCopyBuf];
  for (;;) {
    size_t n = in.read(buf, sizeof(buf));
    if (n == 0) break;
    size_t w = out.write(buf, n);
    if (w != n) {
      in.close();
      out.close();
      to_b->remove(to_p);
      return false;
    }
  }
  in.close();
  out.close();
  return true;
}

bool rename_via_tmp(const char* src_v, const char* dst_v) {
  lofs::FsBackend* sb = nullptr;
  lofs::FsBackend* db = nullptr;
  char sp[256], dp[256], tmp_v[280];
  if (!resolve_locked(src_v, &sb, sp, sizeof(sp))) return false;
  if (!resolve_locked(dst_v, &db, dp, sizeof(dp))) return false;

  lofs::FsBackend* tb = nullptr;
  char tmp_strip[256];
  int n = snprintf(tmp_v, sizeof(tmp_v), "%s.lofs-tmp", dst_v);
  if (n <= 0 || (size_t)n >= sizeof(tmp_v)) return false;
  if (!resolve_locked(tmp_v, &tb, tmp_strip, sizeof(tmp_strip))) return false;
  if (tb != db) return false;

  if (!copy_stream(sb, sp, tb, tmp_strip)) {
    if (tb->exists(tmp_strip)) tb->remove(tmp_strip);
    return false;
  }
  if (db->exists(dp)) {
    if (!db->remove(dp)) {
      tb->remove(tmp_strip);
      return false;
    }
  }
  if (!db->rename(tmp_strip, dp)) {
    tb->remove(tmp_strip);
    return false;
  }
  (void)sb->remove(sp);
  return true;
}

}  // namespace

bool LoFS::mount(const char* prefix, lofs::FsBackend* backend) {
  if (!prefix || !prefix[0] || !backend) return false;
  lofs_lock();
  for (uint8_t i = 0; i < s_nmounts; i++) {
    if (strcmp(s_mounts[i].prefix, prefix) == 0) {
      s_mounts[i].backend = backend;
      lofs_unlock();
      return true;
    }
  }
  if (s_nmounts >= 8) {
    lofs_unlock();
    return false;
  }
  strncpy(s_mounts[s_nmounts].prefix, prefix, sizeof(s_mounts[0].prefix) - 1);
  s_mounts[s_nmounts].prefix[sizeof(s_mounts[0].prefix) - 1] = '\0';
  s_mounts[s_nmounts].backend = backend;
  s_nmounts++;
  lofs_unlock();
  return true;
}

bool LoFS::unmount(const char* prefix) {
  if (!prefix || !prefix[0]) return false;
  lofs_lock();
  for (uint8_t i = 0; i < s_nmounts; i++) {
    if (strcmp(s_mounts[i].prefix, prefix) != 0) continue;
    for (uint8_t j = i + 1; j < s_nmounts; j++) s_mounts[j - 1] = s_mounts[j];
    s_nmounts--;
    lofs_unlock();
    return true;
  }
  lofs_unlock();
  return false;
}

lofs::FsBackend* LoFS::resolveBackend(const char* virtual_path, char* stripped_out, size_t stripped_cap) {
  lofs::FsBackend* b = nullptr;
  lofs_lock();
  bool ok = resolve_locked(virtual_path, &b, stripped_out, stripped_cap);
  lofs_unlock();
  return ok ? b : nullptr;
}

void LoFS::mountDefaults() {
  auto& inb = lofs::InternalFlashBackend::instance();
  auto& exb = lofs::ExternalFlashBackend::instance();
  auto& sdb = lofs::SdBackend::instance();
  auto& ram = lofs::RamBackend::instance();

  if (inb.available()) mount("/__int__", &inb);

#if defined(ESP32_PLATFORM) || defined(RP2040_PLATFORM)
  if (inb.available()) mount("/__ext__", &exb);
#else
  if (exb.available()) mount("/__ext__", &exb);
#endif

  if (sdb.available()) mount("/__sd__", &sdb);
  if (ram.available()) mount("/__ram__", &ram);
}

static File open_norm(const char* filepath, uint8_t mode) {
  char virt[256];
  if (!normalize_virtual(filepath, virt, sizeof(virt))) return lofs::invalid_file();
  lofs::FsBackend* b = nullptr;
  char stripped[256];
  lofs_lock();
  if (!resolve_locked(virt, &b, stripped, sizeof(stripped))) {
    lofs_unlock();
    return lofs::invalid_file();
  }
  File f = b->open(stripped, mode);
  lofs_unlock();
  return f;
}

static File open_norm_str(const char* filepath, const char* mode) {
  char virt[256];
  if (!normalize_virtual(filepath, virt, sizeof(virt))) return lofs::invalid_file();
  lofs::FsBackend* b = nullptr;
  char stripped[256];
  lofs_lock();
  if (!resolve_locked(virt, &b, stripped, sizeof(stripped))) {
    lofs_unlock();
    return lofs::invalid_file();
  }
  File f = b->open(stripped, mode);
  lofs_unlock();
  return f;
}

File LoFS::open(const char* filepath, uint8_t mode) { return open_norm(filepath, mode); }

File LoFS::open(const char* filepath, const char* mode) { return open_norm_str(filepath, mode); }

bool LoFS::exists(const char* filepath) {
  char virt[256];
  if (!normalize_virtual(filepath, virt, sizeof(virt))) return false;
  lofs::FsBackend* b = nullptr;
  char stripped[256];
  lofs_lock();
  if (!resolve_locked(virt, &b, stripped, sizeof(stripped))) {
    lofs_unlock();
    return false;
  }
  bool e = b->exists(stripped);
  lofs_unlock();
  return e;
}

bool LoFS::mkdir(const char* filepath) {
  char virt[256];
  if (!normalize_virtual(filepath, virt, sizeof(virt))) return false;
  lofs::FsBackend* b = nullptr;
  char stripped[256];
  lofs_lock();
  if (!resolve_locked(virt, &b, stripped, sizeof(stripped))) {
    lofs_unlock();
    return false;
  }
  bool ok = b->mkdir(stripped);
  lofs_unlock();
  return ok;
}

bool LoFS::remove(const char* filepath) {
  char virt[256];
  if (!normalize_virtual(filepath, virt, sizeof(virt))) return false;
  lofs::FsBackend* b = nullptr;
  char stripped[256];
  lofs_lock();
  if (!resolve_locked(virt, &b, stripped, sizeof(stripped))) {
    lofs_unlock();
    return false;
  }
  bool ok = b->remove(stripped);
  lofs_unlock();
  return ok;
}

bool LoFS::rename(const char* oldfilepath, const char* newfilepath) {
  if (!oldfilepath || !newfilepath) return false;
  char src_v[256], dst_v[256];
  if (!normalize_virtual(oldfilepath, src_v, sizeof(src_v))) return false;
  if (!normalize_virtual(newfilepath, dst_v, sizeof(dst_v))) return false;
  lofs_lock();
  bool ok = rename_via_tmp(src_v, dst_v);
  lofs_unlock();
  return ok;
}

bool LoFS::rmdir(const char* filepath, bool recursive) {
  char virt[256];
  if (!normalize_virtual(filepath, virt, sizeof(virt))) return false;
  lofs::FsBackend* b = nullptr;
  char stripped[256];
  lofs_lock();
  if (!resolve_locked(virt, &b, stripped, sizeof(stripped))) {
    lofs_unlock();
    return false;
  }
  bool ok = b->rmdir(stripped, recursive);
  lofs_unlock();
  return ok;
}

uint64_t LoFS::totalBytes(const char* filepath) {
  char virt[256];
  if (!normalize_virtual(filepath, virt, sizeof(virt))) return 0;
  lofs::FsBackend* b = nullptr;
  char stripped[256];
  (void)stripped;
  lofs_lock();
  if (!resolve_locked(virt, &b, stripped, sizeof(stripped))) {
    lofs_unlock();
    return 0;
  }
  uint64_t t = b->totalBytes();
  lofs_unlock();
  return t;
}

uint64_t LoFS::usedBytes(const char* filepath) {
  char virt[256];
  if (!normalize_virtual(filepath, virt, sizeof(virt))) return 0;
  lofs::FsBackend* b = nullptr;
  char stripped[256];
  lofs_lock();
  if (!resolve_locked(virt, &b, stripped, sizeof(stripped))) {
    lofs_unlock();
    return 0;
  }
  uint64_t u = b->usedBytes();
  lofs_unlock();
  return u;
}

uint64_t LoFS::freeBytes(const char* filepath) {
  uint64_t t = totalBytes(filepath);
  uint64_t u = usedBytes(filepath);
  return t > u ? t - u : 0;
}

bool LoFS::isSDCardAvailable() { return lofs::SdBackend::instance().available(); }
