#include "PotatoNodeStore.h"

#ifdef ESP32

#include <SPIFFS.h>
#include <helpers/esp32/PotatoMeshDebug.h>
#include <cstring>

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

void PotatoNodeStore::loadIndex() {
  if (!_fs) return;

  File f = _fs->open(PATH, "r");
  if (!f) {
    // First boot: create an empty pre-sized file so r+ seeks work later.
    // Third argument 'true' is required on ESP32 SPIFFS to create new files.
    File wf = _fs->open(PATH, "w", true);
    if (!wf) {
      POTATO_MESH_DBG_LN("node store: failed to create %s", PATH);
      return;
    }
    uint8_t zero[RECORD_SIZE] = {};
    for (int i = 0; i < MAX; i++) wf.write(zero, RECORD_SIZE);
    wf.close();
    POTATO_MESH_DBG_LN("node store: created %s (%u slots x %u bytes = %u KB)",
                        PATH, (unsigned)MAX, (unsigned)RECORD_SIZE,
                        (unsigned)(MAX * RECORD_SIZE / 1024));
    return;
  }

  int slot = 0;
  PotatoNodeRecord rec;
  while (slot < MAX && f.read((uint8_t*)&rec, RECORD_SIZE) == (int)RECORD_SIZE) {
    if (rec.magic == POTATO_NODE_MAGIC) {
      memcpy(_index[slot].key, rec.pub_key, 4);
      _index[slot].last_advert    = rec.last_advert;
      _index[slot].last_posted_ms = 0; // never posted this session
      _count++;
    }
    slot++;
  }
  f.close();
  POTATO_MESH_DBG_LN("node store: loaded %d nodes from %s", _count, PATH);
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
  // SPIFFS on ESP32 Arduino can be unreliable with "r+" and random-access writes.
  // Use a rewrite-to-temp approach (like other MeshCore persistence code) to avoid seek/write quirks.
  static constexpr const char* TMP = "/potato-nodes.tmp";

  File in = _fs->open(PATH, "r");
  File out = _fs->open(TMP, "w", true);
  if (!out) {
    POTATO_MESH_DBG_LN("node store: open tmp failed heap=%u in_open=%d",
                        (unsigned)ESP.getFreeHeap(), in ? 1 : 0);
    if (in) in.close();
    return false;
  }

  size_t in_sz = (in && in.size() > 0) ? (size_t)in.size() : 0;
  size_t expect_sz = (size_t)MAX * RECORD_SIZE;
  POTATO_MESH_DBG_LN(
      "node store write: start slot=%d max=%u rec_sz=%u in_open=%d in_sz=%u expect_sz=%u heap=%u",
      slot, (unsigned)MAX, (unsigned)RECORD_SIZE, in ? 1 : 0, (unsigned)in_sz, (unsigned)expect_sz,
      (unsigned)ESP.getFreeHeap());
  if (in && in_sz > 0 && in_sz != expect_sz) {
    POTATO_MESH_DBG_LN("node store write: warn bin size mismatch (may pad with zeros)");
  }

  PotatoNodeRecord buf;
  uint8_t zero[RECORD_SIZE] = {};
  bool logged_eof = false;
  for (int i = 0; i < MAX; i++) {
    if ((i & 0xff) == 0 && i > 0) {
      POTATO_MESH_DBG_LN("node store write: progress i=%d/%d pos=%u heap=%u", i, MAX,
                         (unsigned)out.position(), (unsigned)ESP.getFreeHeap());
    }
    if (i == slot) {
      // Must consume the old slot from `in` so sequential reads stay aligned with index `i`
      // (skipping this left the stream one record behind and corrupted the rest of the file).
      if (in) {
        int n = in.read((uint8_t*)&buf, RECORD_SIZE);
        if (n != (int)RECORD_SIZE) {
          if (n > 0) {
            POTATO_MESH_DBG_LN("node store write: replace slot=%d discard short read n=%d", slot, n);
          } else if (!logged_eof) {
            logged_eof = true;
            POTATO_MESH_DBG_LN("node store write: replace slot=%d discard hit eof", slot);
          }
        }
      }
      size_t w = out.write((const uint8_t*)&rec, RECORD_SIZE);
      if (w != RECORD_SIZE) {
        POTATO_MESH_DBG_LN("node store: tmp write primary slot=%d wrote=%u need=%u pos=%u heap=%u",
                           slot, (unsigned)w, (unsigned)RECORD_SIZE, (unsigned)out.position(),
                           (unsigned)ESP.getFreeHeap());
        if (in) in.close();
        out.close();
        _fs->remove(TMP);
        return false;
      }
      continue;
    }

    bool wrote = false;
    if (in) {
      int n = in.read((uint8_t*)&buf, RECORD_SIZE);
      if (n == (int)RECORD_SIZE) {
        size_t w = out.write((const uint8_t*)&buf, RECORD_SIZE);
        if (w != RECORD_SIZE) {
          POTATO_MESH_DBG_LN(
              "node store: tmp copy write failed i=%d wrote=%u need=%u in_pos=%u out_pos=%u heap=%u",
              i, (unsigned)w, (unsigned)RECORD_SIZE, (unsigned)in.position(),
              (unsigned)out.position(), (unsigned)ESP.getFreeHeap());
          in.close();
          out.close();
          _fs->remove(TMP);
          return false;
        }
        wrote = true;
      } else {
        if (n > 0) {
          POTATO_MESH_DBG_LN("node store write: short read i=%d n=%d need=%u (zero-pad slot)", i, n,
                             (unsigned)RECORD_SIZE);
        } else if (!logged_eof) {
          logged_eof = true;
          POTATO_MESH_DBG_LN("node store write: eof at i=%d (zero-fill rest)", i);
        }
      }
    } else if (i == 0) {
      POTATO_MESH_DBG_LN("node store write: no existing bin read handle; zero-fill all non-slot");
    }
    if (!wrote) {
      size_t w = out.write(zero, RECORD_SIZE);
      if (w != RECORD_SIZE) {
        POTATO_MESH_DBG_LN("node store: tmp zero write failed i=%d wrote=%u need=%u out_pos=%u heap=%u",
                           i, (unsigned)w, (unsigned)RECORD_SIZE, (unsigned)out.position(),
                           (unsigned)ESP.getFreeHeap());
        if (in) in.close();
        out.close();
        _fs->remove(TMP);
        return false;
      }
    }
  }

  size_t out_sz = out.position();
  if (in) in.close();
  out.close();

  POTATO_MESH_DBG_LN("node store write: flushed tmp bytes=%u heap=%u", (unsigned)out_sz,
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
  if (!f.seek((uint32_t)slot * RECORD_SIZE)) { f.close(); return false; }
  bool ok = (f.read((uint8_t*)&out, RECORD_SIZE) == (int)RECORD_SIZE);
  f.close();
  return ok && (out.magic == POTATO_NODE_MAGIC);
}

#endif // ESP32
