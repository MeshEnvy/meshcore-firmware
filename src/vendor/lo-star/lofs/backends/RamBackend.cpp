#include "RamBackend.h"

#if defined(ESP32_PLATFORM)

#include <FSImpl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef LOFS_RAM_CAP_BYTES
#define LOFS_RAM_CAP_BYTES (64u * 1024u)
#endif

namespace lofs {

namespace {

struct RamNode {
  bool is_dir = false;
  std::shared_ptr<std::vector<uint8_t>> data;  // file bytes (null for dirs)
};

std::string normalize_path(const char* p) {
  if (!p || !*p) return "/";
  std::string s;
  if (p[0] != '/') {
    s.reserve(strlen(p) + 1);
    s.push_back('/');
    s.append(p);
  } else {
    s = p;
  }
  // collapse consecutive slashes
  std::string out;
  out.reserve(s.size());
  bool prev_slash = false;
  for (char c : s) {
    if (c == '/') {
      if (prev_slash) continue;
      prev_slash = true;
    } else {
      prev_slash = false;
    }
    out.push_back(c);
  }
  // drop trailing slash (except root)
  if (out.size() > 1 && out.back() == '/') out.pop_back();
  return out;
}

const char* basename_of(const std::string& p) {
  size_t s = p.rfind('/');
  if (s == std::string::npos) return p.c_str();
  return p.c_str() + s + 1;
}

bool is_direct_child(const std::string& dir_path, const std::string& candidate) {
  // dir_path is either "/" or "/some/dir" (no trailing slash)
  std::string prefix = (dir_path == "/") ? "/" : (dir_path + "/");
  if (candidate.size() <= prefix.size()) return false;
  if (candidate.compare(0, prefix.size(), prefix) != 0) return false;
  // no further slashes in the remainder
  return candidate.find('/', prefix.size()) == std::string::npos;
}

}  // namespace

class RamFSImpl : public fs::FSImpl {
 public:
  RamFSImpl() {
    _mtx = xSemaphoreCreateMutex();
    RamNode root;
    root.is_dir = true;
    _nodes["/"] = root;
  }

  fs::FileImplPtr open(const char* path, const char* mode, bool create) override;
  bool exists(const char* path) override;
  bool rename(const char* pathFrom, const char* pathTo) override;
  bool remove(const char* path) override;
  bool mkdir(const char* path) override;
  bool rmdir(const char* path) override;

  uint64_t usedBytes();
  uint64_t totalBytes() const { return _cap; }

  // Internal helpers used by RamFileImpl while holding the mutex.
  void lock() {
    if (_mtx) xSemaphoreTake(_mtx, portMAX_DELAY);
  }
  void unlock() {
    if (_mtx) xSemaphoreGive(_mtx);
  }

  // Called by RamFileImpl::write to update the shared used-bytes counter.
  // Returns true if the delta fits in cap; false otherwise (caller must not grow).
  bool reserveBytes(size_t delta) {
    if (_used + delta > _cap) return false;
    _used += delta;
    return true;
  }
  void releaseBytes(size_t delta) {
    if (delta > _used) _used = 0;
    else _used -= delta;
  }

  std::map<std::string, RamNode>& nodes() { return _nodes; }

 private:
  std::map<std::string, RamNode> _nodes;
  uint64_t _used = 0;
  uint64_t _cap = LOFS_RAM_CAP_BYTES;
  SemaphoreHandle_t _mtx = nullptr;
};

class RamFileImpl : public fs::FileImpl {
 public:
  // File handle ctor
  RamFileImpl(RamFSImpl* fs, std::string abs_path, std::shared_ptr<std::vector<uint8_t>> data, bool write_mode)
      : _fs(fs),
        _path(std::move(abs_path)),
        _data(std::move(data)),
        _write(write_mode),
        _is_dir(false),
        _valid(true) {
    _name_off = _path.rfind('/');
  }

  // Directory handle ctor
  RamFileImpl(RamFSImpl* fs, std::string abs_path, std::vector<std::string> children)
      : _fs(fs),
        _path(std::move(abs_path)),
        _data(nullptr),
        _write(false),
        _is_dir(true),
        _valid(true),
        _dir_children(std::move(children)) {
    _name_off = _path.rfind('/');
  }

  ~RamFileImpl() override { close(); }

  size_t write(const uint8_t* buf, size_t size) override {
    if (!_valid || _is_dir || !_write || !_data || !buf || size == 0) return 0;
    _fs->lock();
    if (!_fs->reserveBytes(size)) {
      _fs->unlock();
      return 0;
    }
    size_t required = _pos + size;
    if (required > _data->size()) _data->resize(required);
    memcpy(_data->data() + _pos, buf, size);
    _pos += size;
    _fs->unlock();
    return size;
  }

  size_t read(uint8_t* buf, size_t size) override {
    if (!_valid || _is_dir || !_data || !buf) return 0;
    _fs->lock();
    size_t avail = _data->size() > _pos ? _data->size() - _pos : 0;
    size_t n = size < avail ? size : avail;
    if (n > 0) memcpy(buf, _data->data() + _pos, n);
    _pos += n;
    _fs->unlock();
    return n;
  }

  void flush() override {}

  bool seek(uint32_t pos, fs::SeekMode mode) override {
    if (!_valid || _is_dir) return false;
    _fs->lock();
    size_t sz = _data ? _data->size() : 0;
    size_t target = _pos;
    switch (mode) {
      case fs::SeekSet: target = pos; break;
      case fs::SeekCur: target = _pos + pos; break;
      case fs::SeekEnd: target = sz + pos; break;
    }
    if (target > sz) target = sz;
    _pos = target;
    _fs->unlock();
    return true;
  }

  size_t position() const override { return _pos; }
  size_t size() const override { return _data ? _data->size() : 0; }
  bool setBufferSize(size_t) override { return false; }
  void close() override {
    _valid = false;
    _data.reset();
  }
  time_t getLastWrite() override { return 0; }
  const char* path() const override { return _path.c_str(); }
  const char* name() const override {
    if (_name_off == std::string::npos) return _path.c_str();
    return _path.c_str() + _name_off + 1;
  }
  boolean isDirectory() override { return _is_dir; }

  fs::FileImplPtr openNextFile(const char* mode) override {
    if (!_valid || !_is_dir) return fs::FileImplPtr();
    while (_dir_idx < _dir_children.size()) {
      std::string child = _dir_children[_dir_idx++];
      std::string full = (_path == "/") ? ("/" + child) : (_path + "/" + child);
      fs::FileImplPtr p = _fs->open(full.c_str(), mode ? mode : "r", false);
      if (p) return p;
    }
    return fs::FileImplPtr();
  }

  boolean seekDir(long position) override {
    if (!_is_dir) return false;
    if (position < 0) return false;
    _dir_idx = (size_t)position;
    return true;
  }

  String getNextFileName() override {
    if (!_is_dir) return String();
    if (_dir_idx >= _dir_children.size()) return String();
    std::string child = _dir_children[_dir_idx++];
    std::string full = (_path == "/") ? ("/" + child) : (_path + "/" + child);
    return String(full.c_str());
  }

  String getNextFileName(bool* isDir) override {
    if (!_is_dir) {
      if (isDir) *isDir = false;
      return String();
    }
    if (_dir_idx >= _dir_children.size()) {
      if (isDir) *isDir = false;
      return String();
    }
    std::string child = _dir_children[_dir_idx++];
    std::string full = (_path == "/") ? ("/" + child) : (_path + "/" + child);
    if (isDir) {
      _fs->lock();
      auto it = _fs->nodes().find(full);
      *isDir = (it != _fs->nodes().end() && it->second.is_dir);
      _fs->unlock();
    }
    return String(full.c_str());
  }

  void rewindDirectory() override { _dir_idx = 0; }

  operator bool() override { return _valid; }

 private:
  RamFSImpl* _fs;
  std::string _path;
  size_t _name_off = std::string::npos;
  std::shared_ptr<std::vector<uint8_t>> _data;
  bool _write = false;
  bool _is_dir = false;
  bool _valid = false;
  size_t _pos = 0;
  std::vector<std::string> _dir_children;
  size_t _dir_idx = 0;
};

fs::FileImplPtr RamFSImpl::open(const char* path, const char* mode, bool create) {
  std::string p = normalize_path(path);
  const bool want_write = mode && (mode[0] == 'w' || mode[0] == 'a');
  const bool truncate = mode && mode[0] == 'w';

  lock();
  auto it = _nodes.find(p);
  if (it == _nodes.end()) {
    if (!want_write || !create) {
      unlock();
      return fs::FileImplPtr();
    }
    RamNode n;
    n.is_dir = false;
    n.data = std::make_shared<std::vector<uint8_t>>();
    _nodes[p] = n;
    it = _nodes.find(p);
  }

  if (it->second.is_dir) {
    if (want_write) {
      unlock();
      return fs::FileImplPtr();
    }
    std::vector<std::string> children;
    for (auto& kv : _nodes) {
      if (kv.first == p) continue;
      if (is_direct_child(p, kv.first)) children.push_back(kv.first.substr(p == "/" ? 1 : p.size() + 1));
    }
    unlock();
    return std::make_shared<RamFileImpl>(this, p, std::move(children));
  }

  // Regular file
  if (want_write && truncate) {
    releaseBytes(it->second.data ? it->second.data->size() : 0);
    if (it->second.data) it->second.data->clear();
    else it->second.data = std::make_shared<std::vector<uint8_t>>();
  }
  auto data = it->second.data;
  unlock();
  return std::make_shared<RamFileImpl>(this, p, data, want_write);
}

bool RamFSImpl::exists(const char* path) {
  std::string p = normalize_path(path);
  lock();
  bool e = _nodes.find(p) != _nodes.end();
  unlock();
  return e;
}

bool RamFSImpl::mkdir(const char* path) {
  std::string p = normalize_path(path);
  lock();
  auto it = _nodes.find(p);
  if (it != _nodes.end()) {
    unlock();
    return false;
  }
  RamNode n;
  n.is_dir = true;
  _nodes[p] = n;
  unlock();
  return true;
}

bool RamFSImpl::remove(const char* path) {
  std::string p = normalize_path(path);
  lock();
  auto it = _nodes.find(p);
  if (it == _nodes.end() || it->second.is_dir) {
    unlock();
    return false;
  }
  releaseBytes(it->second.data ? it->second.data->size() : 0);
  _nodes.erase(it);
  unlock();
  return true;
}

bool RamFSImpl::rename(const char* pathFrom, const char* pathTo) {
  std::string from = normalize_path(pathFrom);
  std::string to = normalize_path(pathTo);
  if (from == to) return true;
  lock();
  auto it = _nodes.find(from);
  if (it == _nodes.end()) {
    unlock();
    return false;
  }
  auto dst = _nodes.find(to);
  if (dst != _nodes.end()) {
    if (dst->second.is_dir) {
      unlock();
      return false;
    }
    releaseBytes(dst->second.data ? dst->second.data->size() : 0);
    _nodes.erase(dst);
  }
  _nodes[to] = it->second;
  _nodes.erase(it);
  unlock();
  return true;
}

bool RamFSImpl::rmdir(const char* path) {
  std::string p = normalize_path(path);
  if (p == "/") return false;
  lock();
  auto it = _nodes.find(p);
  if (it == _nodes.end() || !it->second.is_dir) {
    unlock();
    return false;
  }
  for (auto& kv : _nodes) {
    if (kv.first == p) continue;
    if (is_direct_child(p, kv.first)) {
      unlock();
      return false;
    }
  }
  _nodes.erase(it);
  unlock();
  return true;
}

uint64_t RamFSImpl::usedBytes() {
  lock();
  uint64_t u = _used;
  unlock();
  return u;
}

// --- RamBackend glue ---

RamBackend::RamBackend() : _impl(std::make_shared<RamFSImpl>()), _fs(_impl) {}

RamBackend& RamBackend::instance() {
  static RamBackend inst;
  return inst;
}

bool RamBackend::available() const { return _impl != nullptr; }

File RamBackend::open(const char* path, uint8_t mode) {
  if (!path) return lofs::invalid_file();
  return _fs.open(path, mode == FILE_O_READ ? "r" : "w", mode != FILE_O_READ);
}

File RamBackend::open(const char* path, const char* mode) {
  if (!path || !mode) return lofs::invalid_file();
  const bool create = (mode[0] == 'w' || mode[0] == 'a');
  return _fs.open(path, mode, create);
}

bool RamBackend::exists(const char* path) { return path && _fs.exists(path); }
bool RamBackend::mkdir(const char* path) { return path && _fs.mkdir(path); }
bool RamBackend::remove(const char* path) { return path && _fs.remove(path); }
bool RamBackend::rename(const char* from, const char* to) {
  return from && to && _fs.rename(from, to);
}

bool RamBackend::rmdir(const char* path, bool recursive) {
  if (!path) return false;
  if (!recursive) return _fs.rmdir(path);
  // Recursive: collect children first, then remove.
  std::string p = normalize_path(path);
  if (p == "/") return false;
  _impl->lock();
  std::vector<std::string> victims;
  std::vector<std::string> subdirs;
  for (auto& kv : _impl->nodes()) {
    if (kv.first == p) continue;
    if (kv.first.size() > p.size() + 1 && kv.first.compare(0, p.size() + 1, p + "/") == 0) {
      if (kv.second.is_dir) subdirs.push_back(kv.first);
      else victims.push_back(kv.first);
    }
  }
  _impl->unlock();
  for (auto& f : victims) _fs.remove(f.c_str());
  // deepest first
  for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) _fs.rmdir(it->c_str());
  return _fs.rmdir(p.c_str());
}

uint64_t RamBackend::totalBytes() { return _impl ? _impl->totalBytes() : 0; }
uint64_t RamBackend::usedBytes() { return _impl ? _impl->usedBytes() : 0; }

}  // namespace lofs

#else   // !ESP32_PLATFORM

namespace lofs {

RamBackend::RamBackend() {}
RamBackend& RamBackend::instance() {
  static RamBackend inst;
  return inst;
}
bool RamBackend::available() const { return false; }
File RamBackend::open(const char*, uint8_t) { return lofs::invalid_file(); }
File RamBackend::open(const char*, const char*) { return lofs::invalid_file(); }
bool RamBackend::exists(const char*) { return false; }
bool RamBackend::mkdir(const char*) { return false; }
bool RamBackend::remove(const char*) { return false; }
bool RamBackend::rename(const char*, const char*) { return false; }
bool RamBackend::rmdir(const char*, bool) { return false; }
uint64_t RamBackend::totalBytes() { return 0; }
uint64_t RamBackend::usedBytes() { return 0; }

}  // namespace lofs

#endif  // ESP32_PLATFORM
