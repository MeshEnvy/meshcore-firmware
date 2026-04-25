#pragma once

#ifdef ESP32

#include <cstddef>
#include <cstdint>
// Forward-declare meshcore-native types so this header stays clean; the .cpp is the only TU in
// the fork that sees both meshcore and lostar vocabularies in the same source file.
namespace mesh {
class Mesh;
class Packet;
class Identity;
}  // namespace mesh

namespace fs {
class FS;
}

/**
 * Fork-local lostar adapter for the meshcore simple_repeater application. This is the ONLY TU
 * in the meshcore fork that sees both `mesh::Packet` / `mesh::Identity` and `lostar_*` POD types
 * in the same source file.
 *
 * Boot ordering:
 *
 *   1. `lostar_mc_install(mesh, internal_fs, self_pub_key)` from `MyMesh::begin(...)`:
 *        binds `internal_fs` to an internal `ArduinoFsVolume`, installs `lostar_host_ops`,
 *        runs `lotato::init()` / `louser::init()` / `lofi::init()`. Meshcore admins are ACL-pre-
 *        authenticated on the upstream side, so no extra guards are applied on top.
 *   2. Per-event hooks (called from the fork's normal places in `MyMesh.cpp`):
 *        - `lostar_mc_on_advert(packet, id, timestamp, app_data, app_data_len)` from
 *          `MyMesh::onAdvertRecv(...)`.
 *        - `lostar_mc_on_admin_txt(...)` from `MyMesh::onPeerDataRecv(...)` for
 *          PAYLOAD_TYPE_TXT_MSG admin commands. Returns true if consumed (skip upstream path).
 *        - `lostar_mc_tick()` from `MyMesh::loop()`.
 *        - `lostar_mc_is_busy()` wraps `lostar_is_busy()` for `MyMesh::hasPendingWork()`;
 *          provided for symmetry so the call site can stay within the adapter vocabulary.
 */
void lostar_mc_install(mesh::Mesh *mesh, fs::FS *internal_fs, const uint8_t self_pub_key[32]);

/** Fan an incoming meshcore advert into the lostar advert observers. */
void lostar_mc_on_advert(const mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                         const uint8_t *app_data, std::size_t app_data_len);

/**
 * Route an admin TXT_MSG CLI command through lostar. Matches any registered root (`lotato …` /
 * `wifi …` / `config …` / `user …` / `/?`) and, on match, dispatches through the lostar router
 * with a fork-owned deferred reply. Returns true if consumed (upstream path skipped).
 */
bool lostar_mc_on_admin_txt(uint32_t sender_ts, const uint8_t pub_key[32], const uint8_t secret[32],
                            const uint8_t *out_path, uint8_t out_path_len, uint8_t path_hash_size,
                            char *command, bool is_retry);

/** Service tick: drains the chunked reply FIFO, runs registered lostar tick hooks. */
void lostar_mc_tick();

/**
 * True if the adapter or any lostar module has pending work that should keep the host awake
 * (queued reply chunks, ingest batch, async wifi op, …).
 */
bool lostar_mc_is_busy();

#endif  // ESP32
