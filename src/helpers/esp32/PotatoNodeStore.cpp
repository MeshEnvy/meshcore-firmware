#include "PotatoNodeStore.h"

#ifdef ESP32

#include <SPIFFS.h>
#include <helpers/esp32/PotatoMeshDebug.h>
#include <cstring>

namespace {

constexpr uint32_t kDenseMagic    = 0x444E5450u; // 'PTND' (little-endian on ESP32)
constexpr uint16_t kDenseVersion = 1;

static inline bool bm_get(const uint8_t* bm, int s) {
  return (bm[s >> 3] & (1u << (s & 7))) != 0;
}
static inline void bm_set(uint8_t* bm, int s) { bm[s >> 3] |= (1u << (s & 7)); }

} // namespace

void PotatoNodeStore::begin(fs::FS* fs) {
  _fs = fs;
  if (!_idx_mtx) _idx_mtx = xSemaphoreCreateMutex();
  _count = 0;
  memset(_index, 0, sizeof(_index));
  loadIndex();
}

void PotatoNodeStore::resetPostTimers() {
  if (!_idx_mtx) return;
  xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  for (int i = 0; i < MAX; i++) _index[i].last_posted_ms = 0;
  xSemaphoreGive(_idx_mtx);
}

static bool write_empty_dense_file(fs::FS* fs) {
  File wf = fs->open(PotatoNodeStore::PATH, "w", true);
  if (!wf) return false;
  uint32_t mag = kDenseMagic;
  uint16_t ver = kDenseVersion;
  uint16_t pad = 0;
  wf.write((const uint8_t*)&mag, 4);
  wf.write((const uint8_t*)&ver, 2);
  wf.write((const uint8_t*)&pad, 2);
  uint8_t z[PotatoNodeStore::BITMAP_BYTES]{};
  wf.write(z, sizeof(z));
  wf.close();
  return true;
}

void PotatoNodeStore::loadIndex() {
  if (!_fs) return;

  File f = _fs->open(PATH, "r");
  if (!f) {
    if (!write_empty_dense_file(_fs)) {
      POTATO_MESH_DBG_LN("node store: failed to create %s", PATH);
      return;
    }
    POTATO_MESH_DBG_LN("node store: created empty dense %s (hdr=%u + bitmap=%u B)", PATH,
                        8u, (unsigned)BITMAP_BYTES);
    return;
  }

  const size_t fsz = f.size();
  f.seek(0);
  uint32_t mag = 0;
  if (f.read((uint8_t*)&mag, 4) == 4 && mag == kDenseMagic) {
    uint16_t ver = 0, pad = 0;
    if (f.read((uint8_t*)&ver, 2) != 2 || f.read((uint8_t*)&pad, 2) != 2 || ver != kDenseVersion) {
      POTATO_MESH_DBG_LN("node store: dense header bad ver=%u — reset", (unsigned)ver);
      f.close();
      _fs->remove(PATH);
      write_empty_dense_file(_fs);
      return;
    }
    uint8_t bm[BITMAP_BYTES];
    if (f.read(bm, sizeof(bm)) != (int)sizeof(bm)) {
      POTATO_MESH_DBG_LN("node store: dense bitmap read failed — reset");
      f.close();
      _fs->remove(PATH);
      write_empty_dense_file(_fs);
      return;
    }
    PotatoNodeRecord rec;
    for (int s = 0; s < MAX; s++) {
      if (!bm_get(bm, s)) continue;
      if (f.read((uint8_t*)&rec, RECORD_SIZE) != (int)RECORD_SIZE) {
        POTATO_MESH_DBG_LN("node store: dense payload truncated at slot=%d — partial load", s);
        break;
      }
      if (rec.magic == POTATO_NODE_MAGIC) {
        memcpy(_index[s].key, rec.pub_key, 4);
        _index[s].last_advert    = rec.last_advert;
        _index[s].last_posted_ms = 0;
        _count++;
      }
    }
    f.close();
    POTATO_MESH_DBG_LN("node store: loaded %d nodes from dense %s (%u bytes)", _count, PATH,
                       (unsigned)fsz);
    return;
  }
  f.close();
  POTATO_MESH_DBG_LN("node store: non-dense or unreadable %s — removing, starting empty", PATH);
  _fs->remove(PATH);
  if (!write_empty_dense_file(_fs)) {
    POTATO_MESH_DBG_LN("node store: failed to recreate empty dense after remove");
  }
}

int PotatoNodeStore::findSlot(const uint8_t* pub_key) const {
  for (int i = 0; i < MAX; i++) {
    if (_index[i].last_advert != 0 &&
        memcmp(_index[i].key, pub_key, 4) == 0) {
      // Confirm full key match via disk read would be ideal, but false positives
      // (same 4-byte prefix, different full key) are extremely rare and the worst
      // outcome is a harmless overwrite. Accept the 4-byte match for speed.
      return i;
    }
  }
  return -1;
}

int PotatoNodeStore::findEmptySlot() const {
  for (int i = 0; i < MAX; i++) {
    if (_index[i].last_advert == 0) return i;
  }
  return -1;
}

int PotatoNodeStore::evictLRU() {
  int oldest = 0;
  uint32_t oldest_advert = _index[0].last_advert;
  for (int i = 1; i < MAX; i++) {
    if (_index[i].last_advert < oldest_advert) {
      oldest_advert = _index[i].last_advert;
      oldest = i;
    }
  }
  _index[oldest].last_advert    = 0;
  _index[oldest].last_posted_ms = 0;
  memset(_index[oldest].key, 0, 4);
  _count--;
  return oldest;
}

bool PotatoNodeStore::writeRecord(int slot, const PotatoNodeRecord& rec) {
  if (!_fs || slot < 0 || slot >= MAX) return false;

  static constexpr const char* TMP = "/potato-nodes.tmp";

  uint8_t new_bm[BITMAP_BYTES];
  memset(new_bm, 0, sizeof(new_bm));
  int out_recs = 0;
  for (int s = 0; s < MAX; s++) {
    if (s == slot || _index[s].last_advert != 0) {
      bm_set(new_bm, s);
      out_recs++;
    }
  }

  File in = _fs->open(PATH, "r");
  if (!in) {
    POTATO_MESH_DBG_LN("node store: missing %s on write", PATH);
    return false;
  }
  uint8_t old_bm[BITMAP_BYTES];
  memset(old_bm, 0, sizeof(old_bm));
  in.seek(0);
  uint32_t hm = 0;
  if (in.read((uint8_t*)&hm, 4) != 4 || hm != kDenseMagic) {
    POTATO_MESH_DBG_LN("node store: write aborted — %s not dense (reboot to clear store)", PATH);
    in.close();
    return false;
  }
  uint16_t hv = 0, hpad = 0;
  if (in.read((uint8_t*)&hv, 2) != 2 || in.read((uint8_t*)&hpad, 2) != 2 || hv != kDenseVersion ||
      in.read(old_bm, sizeof(old_bm)) != (int)sizeof(old_bm)) {
    POTATO_MESH_DBG_LN("node store: write aborted — corrupt dense header");
    in.close();
    return false;
  }

  File out = _fs->open(TMP, "w", true);
  if (!out) {
    POTATO_MESH_DBG_LN("node store: dense open tmp failed heap=%u", (unsigned)ESP.getFreeHeap());
    in.close();
    return false;
  }

  uint32_t mag = kDenseMagic;
  uint16_t ver = kDenseVersion;
  uint16_t pad = 0;
  if (out.write((const uint8_t*)&mag, 4) != 4 || out.write((const uint8_t*)&ver, 2) != 2 ||
      out.write((const uint8_t*)&pad, 2) != 2 || out.write(new_bm, sizeof(new_bm)) != (int)sizeof(new_bm)) {
    POTATO_MESH_DBG_LN("node store: dense write header/bitmap failed heap=%u", (unsigned)ESP.getFreeHeap());
    in.close();
    out.close();
    _fs->remove(TMP);
    return false;
  }

  PotatoNodeRecord old_r{};
  for (int s = 0; s < MAX; s++) {
    bool have_old = false;
    if (bm_get(old_bm, s)) {
      int n = in.read((uint8_t*)&old_r, RECORD_SIZE);
      have_old = (n == (int)RECORD_SIZE && old_r.magic == POTATO_NODE_MAGIC);
    }

    const bool want_new = (s == slot) || (_index[s].last_advert != 0);
    if (!want_new) continue;

    size_t w;
    if (s == slot) {
      w = out.write((const uint8_t*)&rec, RECORD_SIZE);
    } else {
      if (!have_old) {
        POTATO_MESH_DBG_LN("node store: dense rewrite missing payload s=%d (slot=%d) heap=%u "
                            "spiffs_used=%u/%u",
                            s, slot, (unsigned)ESP.getFreeHeap(), (unsigned)SPIFFS.usedBytes(),
                            (unsigned)SPIFFS.totalBytes());
        in.close();
        out.close();
        _fs->remove(TMP);
        return false;
      }
      w = out.write((const uint8_t*)&old_r, RECORD_SIZE);
    }
    if (w != RECORD_SIZE) {
      POTATO_MESH_DBG_LN("node store: dense payload write failed s=%d wrote=%u need=%u out_pos=%u "
                         "spiffs_used=%u/%u heap=%u",
                         s, (unsigned)w, (unsigned)RECORD_SIZE, (unsigned)out.position(),
                         (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes(),
                         (unsigned)ESP.getFreeHeap());
      in.close();
      out.close();
      _fs->remove(TMP);
      return false;
    }
  }

  const size_t out_sz = out.position();
  in.close();
  out.close();

  POTATO_MESH_DBG_LN("node store write: dense tmp bytes=%u recs=%d heap=%u", (unsigned)out_sz, out_recs,
                      (unsigned)ESP.getFreeHeap());

  _fs->remove(PATH);
  if (!_fs->rename(TMP, PATH)) {
    POTATO_MESH_DBG_LN("node store: rename tmp->bin failed heap=%u tmp_gone=%d", (unsigned)ESP.getFreeHeap(),
                        _fs->exists(TMP) ? 0 : 1);
    _fs->remove(TMP);
    return false;
  }

  POTATO_MESH_DBG_LN("node store write: ok slot=%d heap=%u", slot, (unsigned)ESP.getFreeHeap());
  return true;
}

int PotatoNodeStore::put(const uint8_t* pub_key, const char* name, uint8_t type,
                         uint32_t last_advert, int32_t lat, int32_t lon) {
  if (!_fs || !pub_key) return -1;

  int slot = -1;
  int slot_mode = 0; // 0=update, 1=new empty, 2=evict
  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  slot = findSlot(pub_key);
  if (slot < 0) {
    if (_count < MAX) {
      slot = findEmptySlot();
      slot_mode = 1;
    }
    if (slot < 0) {
      slot = evictLRU();
      slot_mode = 2;
      POTATO_MESH_DBG_LN("node store: store full, evicted LRU slot %d", slot);
    }
  }
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);

  PotatoNodeRecord rec;
  memcpy(rec.pub_key, pub_key, 32);
  strncpy(rec.name, name ? name : "", sizeof(rec.name) - 1);
  rec.name[sizeof(rec.name) - 1] = '\0';
  rec.type        = type;
  memset(rec._pad, 0, sizeof(rec._pad));
  rec.last_advert = last_advert;
  rec.gps_lat     = lat;
  rec.gps_lon     = lon;
  rec.magic       = POTATO_NODE_MAGIC;

  POTATO_MESH_DBG_LN("node store put: slot=%d mode=%s idx_count=%u last_advert=%lu type=%u heap=%u",
                      slot, slot_mode == 0 ? "update" : (slot_mode == 1 ? "new" : "evict"),
                      (unsigned)_count, (unsigned long)last_advert, (unsigned)type,
                      (unsigned)ESP.getFreeHeap());

  PotatoNodeRecord existing{};
  if (readRecord(slot, existing) && memcmp(&existing, &rec, RECORD_SIZE) == 0) {
    return slot;
  }

  if (!writeRecord(slot, rec)) {
    POTATO_MESH_DBG_LN("node store: write failed slot=%d", slot);
    return -1;
  }

  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  bool is_new = (_index[slot].last_advert == 0);
  memcpy(_index[slot].key, pub_key, 4);
  _index[slot].last_advert = last_advert;
  if (is_new) _count++;
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);

  return slot;
}

void PotatoNodeStore::logFlushTargetsDebug() const {
  if (!potato_mesh_dbg_active()) return;
  char line[384];
  size_t pos = 0;
  int listed = 0;
  int overflow = 0;
  constexpr int kMaxList = 32;
  PotatoNodeRecord rec;
  line[0] = '\0';

  for (int i = 0; i < MAX; i++) {
    if (_index[i].last_advert == 0) continue;
    if (!readRecord(i, rec)) continue;
    char nid[10];
    nid[0] = '!';
    static const char* hx = "0123456789abcdef";
    for (int b = 0; b < 4; b++) {
      nid[1 + b * 2] = hx[rec.pub_key[b] >> 4];
      nid[1 + b * 2 + 1] = hx[rec.pub_key[b] & 0x0f];
    }
    nid[9] = '\0';
    if (listed >= kMaxList) {
      overflow++;
      continue;
    }
    int w = snprintf(line + pos, sizeof(line) - pos, "%s%s", pos ? " " : "", nid);
    if (w < 0 || (size_t)w >= sizeof(line) - pos) break;
    pos += (size_t)w;
    listed++;
  }

  if (line[0]) {
    POTATO_MESH_DBG_LN("potato CLI: flush — node ids: %s", line);
    if (overflow > 0) {
      POTATO_MESH_DBG_LN("potato CLI: flush — (%d more ids not listed)", overflow);
    }
  }
}

bool PotatoNodeStore::dueForIngest(int slot, uint32_t now_ms) const {
  if (slot < 0 || slot >= MAX || !_idx_mtx) return false;
  xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  bool empty = (_index[slot].last_advert == 0);
  uint32_t lp = _index[slot].last_posted_ms;
  xSemaphoreGive(_idx_mtx);
  if (empty) return false;
  if (lp == 0) return true;
  return (int32_t)(now_ms - lp) >= (int32_t)POTATO_NODE_REPOST_MS;
}

void PotatoNodeStore::markPosted(int slot, uint32_t now_ms) {
  if (slot < 0 || slot >= MAX) return;
  if (!_idx_mtx) return;
  xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  _index[slot].last_posted_ms = now_ms;
  xSemaphoreGive(_idx_mtx);
}

bool PotatoNodeStore::readRecord(int slot, PotatoNodeRecord& out) const {
  if (!_fs || slot < 0 || slot >= MAX) return false;
  File f = _fs->open(PATH, "r");
  if (!f) return false;

  f.seek(0);
  uint32_t mag = 0;
  if (f.read((uint8_t*)&mag, 4) == 4 && mag == kDenseMagic) {
    uint16_t ver = 0, pad = 0;
    if (f.read((uint8_t*)&ver, 2) != 2 || f.read((uint8_t*)&pad, 2) != 2 || ver != kDenseVersion) {
      f.close();
      return false;
    }
    uint8_t bm[BITMAP_BYTES];
    if (f.read(bm, sizeof(bm)) != (int)sizeof(bm)) {
      f.close();
      return false;
    }
    if (!bm_get(bm, slot)) {
      f.close();
      return false;
    }
    uint32_t pos = 8 + (uint32_t)sizeof(bm);
    for (int s = 0; s < slot; s++) {
      if (bm_get(bm, s)) pos += (uint32_t)RECORD_SIZE;
    }
    if (!f.seek(pos)) {
      f.close();
      return false;
    }
    bool ok = (f.read((uint8_t*)&out, RECORD_SIZE) == (int)RECORD_SIZE);
    f.close();
    return ok && (out.magic == POTATO_NODE_MAGIC);
  }

  f.close();
  return false;
}

#endif // ESP32
