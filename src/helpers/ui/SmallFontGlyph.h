#pragma once

#include <stdint.h>

namespace mesh {
namespace ui {

// Packed monochrome bitmap metrics, with GFX-style baseline offsets.
#if defined(NRF52_PLATFORM)
// Both fixed ASCII fonts use offsets <= 209, dimensions <= 7, advances <= 6
// and baseline offsets -5..1. Keep every metric/pixel while storing the two
// 95-glyph tables in 570 bytes instead of 1520; no decoded cache is needed.
// Native and Cortex-M4 tests check every source metric and rendered glyph.
struct __attribute__((packed)) SmallFontGlyph {
  uint8_t bitmap_offset;
  uint16_t width : 3, height : 3, advance : 3;
  int16_t x_offset : 3, y_offset : 4;
};
static_assert(sizeof(SmallFontGlyph) == 3, "Compact font metric layout changed");
#else
struct SmallFontGlyph {
  uint16_t bitmap_offset;
  uint8_t width, height, advance;
  int8_t x_offset, y_offset;
};
#endif

}  // namespace ui
}  // namespace mesh
