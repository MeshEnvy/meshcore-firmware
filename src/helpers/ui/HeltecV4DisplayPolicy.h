#pragma once

#include <stdint.h>
#include <string.h>

namespace mesh { namespace ui {

enum class HeltecV4DisplayMode : uint8_t { Auto, Oled, Tft, Off };
enum class HeltecV4DisplayType : uint8_t { None, Oled, Tft };

inline const char* heltecV4DisplayModeName(HeltecV4DisplayMode mode) {
  switch (mode) {
    case HeltecV4DisplayMode::Oled: return "oled";
    case HeltecV4DisplayMode::Tft: return "tft";
    case HeltecV4DisplayMode::Off: return "off";
    default: return "auto";
  }
}

inline const char* heltecV4DisplayTypeName(HeltecV4DisplayType type) {
  switch (type) {
    case HeltecV4DisplayType::Oled: return "oled";
    case HeltecV4DisplayType::Tft: return "tft";
    default: return "none";
  }
}

inline bool parseHeltecV4DisplayMode(const char* text, HeltecV4DisplayMode& mode) {
  if (!text) return false;
  if (strcmp(text, "auto") == 0) mode = HeltecV4DisplayMode::Auto;
  else if (strcmp(text, "oled") == 0) mode = HeltecV4DisplayMode::Oled;
  else if (strcmp(text, "tft") == 0) mode = HeltecV4DisplayMode::Tft;
  else if (strcmp(text, "off") == 0) mode = HeltecV4DisplayMode::Off;
  else return false;
  return true;
}

inline HeltecV4DisplayType selectHeltecV4Display(HeltecV4DisplayMode mode,
                                               bool oled_present,
                                               bool tft_present) {
  if (mode == HeltecV4DisplayMode::Off) return HeltecV4DisplayType::None;
  if (mode == HeltecV4DisplayMode::Oled) return HeltecV4DisplayType::Oled;
  if (mode == HeltecV4DisplayMode::Tft) return HeltecV4DisplayType::Tft;
  if (oled_present) return HeltecV4DisplayType::Oled;
  return tft_present ? HeltecV4DisplayType::Tft : HeltecV4DisplayType::None;
}

}}  // namespace mesh::ui
