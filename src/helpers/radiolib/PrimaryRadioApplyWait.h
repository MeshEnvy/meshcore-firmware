#pragma once

#include <RadioProfiles.h>

namespace mesh {

// Only an accepted primary tuple may abandon an unfinished RX after this
// bounded handoff. Completed RX, chip BUSY, TX and exclusive owners are guarded
// separately by RadioLibWrapper. Ordinary scan/CAD/TX admission never uses it.
class PrimaryRadioApplyWait {
  RadioProfileParams target_;
  uint32_t started_ms_ = 0;
  uint32_t wait_ms_ = 0;
  bool temporary_ = false;
  bool active_ = false;

  static uint32_t packetMillis(const RadioProfileParams& params,
                               uint16_t preamble, unsigned length) {
    const double symbol_us = RadioProfiles::symbolUs(params);
    if (symbol_us <= 0) return 30000;
    const int de = symbol_us >= 16000 ? 1 : 0;
    const int denominator = params.sf - 2 * de;
    if (denominator <= 0) return 30000;
    // Use the maximum coding redundancy, not just the selected TX CR. An RX
    // header may select any supported coding rate. Match the wrapper's LoRa
    // airtime formula without tuning the chip to the requested tuple first.
    const int coded = (2 * length + (params.sf <= 6 ? 8 : 10) - 2 * de) / denominator;
    const double symbols = preamble + (params.sf <= 6 ? 6.25 : 4.25) + 8 + coded * 8;
    const double milliseconds = ceil(symbols * symbol_us / 1000.0);
    return milliseconds >= 30000 ? 30000 : static_cast<uint32_t>(milliseconds);
  }

 public:
  void clear() { active_ = false; }

  bool mayAbandonReceive(uint32_t now, const RadioProfileParams& target, bool temporary,
                         const RadioProfileParams& current, uint16_t current_preamble,
                         uint16_t target_preamble, unsigned maximum_length) {
    if (!active_ || target_ != target || temporary_ != temporary) {
      target_ = target;
      temporary_ = temporary;
      started_ms_ = now;
      const uint32_t current_ms = packetMillis(current, current_preamble, maximum_length);
      const uint32_t target_ms = packetMillis(target, target_preamble, maximum_length);
      wait_ms_ = current_ms > target_ms ? current_ms : target_ms;
      wait_ms_ = wait_ms_ >= 29000 ? 30000 : wait_ms_ + 1000;
      if (wait_ms_ < 2000) wait_ms_ = 2000;
      active_ = true;
    }
    return static_cast<uint32_t>(now - started_ms_) >= wait_ms_;
  }
};

} // namespace mesh
