#pragma once

#include <helpers/HostKissConfig.h>
#if MESH_ENABLE_HOST_KISS
#include <helpers/IdentityStore.h>

namespace mesh { namespace hostkiss {
enum class Transport : uint8_t { Usb = 0, Tcp = 1 };
struct Selection {
  bool enabled = false;
  Transport transport = Transport::Usb;
  bool operator==(const Selection& value) const {
    return enabled == value.enabled && transport == value.transport;
  }
};
class Settings {
  FILESYSTEM* fs_ = nullptr;
  bool healthy_ = false;
  Selection selection_;
 public:
  bool begin(FILESYSTEM* fs);
  bool save(const Selection& selection);
  const Selection& selection() const { return selection_; }
  bool healthy() const { return healthy_; }
};
}}
#endif
