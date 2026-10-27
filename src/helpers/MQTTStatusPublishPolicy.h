#pragma once

#include <stdint.h>

namespace MQTTStatusPublishPolicy {

// Bridge-task owned. A failed destination keeps its own retry deadline;
// another broker accepting the same snapshot cannot postpone that retry.
struct State {
  uint32_t attempted_at = 0;
  uint32_t accepted_at = 0;
  bool has_attempt = false;
  bool has_accepted = false;
  bool pending = false;

  void request() { pending = true; has_attempt = false; }

  bool due(uint32_t now, uint32_t interval, uint32_t retry_interval,
           bool periodic_enabled) const {
    if (pending) return !has_attempt || uint32_t(now - attempted_at) >= retry_interval;
    return periodic_enabled && (!has_accepted || uint32_t(now - accepted_at) >= interval);
  }

  void attempted(uint32_t now, bool accepted) {
    attempted_at = now;
    has_attempt = true;
    pending = !accepted;
    if (accepted) { accepted_at = now; has_accepted = true; }
  }
};

}  // namespace MQTTStatusPublishPolicy
