#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mesh { namespace ui {

struct HeltecV4TouchIdentity {
  uint8_t address = 0;
  uint8_t chip = 0;
  uint32_t config = 0;
  uint16_t width = 0;
  uint16_t height = 0;
};

inline bool validHeltecV4TouchIdentity(const HeltecV4TouchIdentity& identity) {
  // The application note gives the chip code only as a five-bit field; it
  // does not reserve code zero. Require independently read configuration,
  // address and plausible touch coordinates instead of guessing chip IDs.
  // Touch resolution is independent of LCD resolution: the manufacturer's
  // own example is 360x360, even for a rectangular display assembly.
  return (identity.address == 0x2E || identity.address == 0x5C)
      && identity.chip != 0xFF
      && identity.config != 0 && identity.config != 0xFFFFFFFFu
      && identity.width >= 160 && identity.width <= 4095
      && identity.height >= 160 && identity.height <= 4095;
}

inline bool equalHeltecV4TouchIdentity(const HeltecV4TouchIdentity& a,
                                      const HeltecV4TouchIdentity& b) {
  return a.address == b.address && a.chip == b.chip && a.config == b.config
      && a.width == b.width && a.height == b.height;
}

template <typename Io>
bool readHeltecV4TouchIdentity(Io& io, HeltecV4TouchIdentity& identity) {
  uint8_t config[4], resolution[4];
  if (!io.readRegister(0x0001, &identity.address, 1)
      || !io.readRegister(0x9E6B, &identity.chip, 1)
      || !io.readRegister(0x9E00, config, sizeof(config))
      || !io.readRegister(0x9E4C, resolution, sizeof(resolution))) return false;
  identity.config = uint32_t(config[0]) | uint32_t(config[1]) << 8
      | uint32_t(config[2]) << 16 | uint32_t(config[3]) << 24;
  identity.width = uint16_t(resolution[0]) | uint16_t(resolution[1]) << 8;
  identity.height = uint16_t(resolution[2]) | uint16_t(resolution[3]) << 8;
  return validHeltecV4TouchIdentity(identity);
}

// CHSC6xxx application note, sections 3.1 and 3.1.1-3.1.5:
// https://resource.heltec.cn/download/WiFi_LoRa_32_Expansion_Kit/Touchscreen-Data/
// Application%C2%A0Note%C2%A0for%C2%A0CTPM_CHSC6xxx(1).pdf
// Only the documented temporary DMA-mode command and identity reads are used.
// There are no controller firmware/flash writes. Reset always exits DMA mode,
// including after a NACK, short read or rejected identity. Io provides reset(),
// delayMs(), write(bytes,length), and readRegister(address,bytes,length).
template <typename Io>
bool probeHeltecV4Touch(Io& io, HeltecV4TouchIdentity* identity = nullptr) {
  static const uint8_t enter_dma[] = {0x42, 0xBD, 0x28, 0x35,
                                      0xC1, 0x00, 0x35, 0xAE};
  bool present = false;
  HeltecV4TouchIdentity first, second;
  for (uint8_t attempt = 0; attempt < 2 && !present; ++attempt) {
    io.reset();
    io.delayMs(30);
    if (!io.write(enter_dma, sizeof(enter_dma))) continue;
    io.delayMs(20);
    present = readHeltecV4TouchIdentity(io, first)
        && readHeltecV4TouchIdentity(io, second)
        && equalHeltecV4TouchIdentity(first, second);
  }
  io.reset();
  io.delayMs(30);
  if (present && identity) *identity = first;
  return present;
}

}}  // namespace mesh::ui
