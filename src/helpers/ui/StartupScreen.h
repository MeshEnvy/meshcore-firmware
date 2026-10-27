#pragma once

#include "DisplayDriver.h"
#include <Arduino.h>
#include <stdio.h>

#if defined(COMPANION_PAIRING_UI_HIL)
void companionStartupUiHilFrame(const char* status);
#endif

namespace mesh { namespace ui {

// Used before UITask exists. Do not initialize the normal power policy here:
// its boot timeout must start when the real UI is ready, not during keygen.
class StartupScreen {
  DisplayDriver* _display = nullptr;
  bool _quiet = false;
  bool _keygen_visible = false;
  bool _generating = false;
  const char* _status = "Starting...";
  uint8_t _phase = 0;
  uint32_t _last_frame = 0;
  uint32_t _generation_started = 0;
  uint32_t _eink_interval = 5000;
  uint16_t _dot_columns = 0, _dot_rows = 0;
  uint32_t _dots_drawn = 0;
  int _dot_top = 0;

  void appendEinkDot() {
    if (_dots_drawn >= uint32_t(_dot_columns) * _dot_rows) return;
    const uint32_t started = millis();
    _display->setColor(UIColor::primary_txt);
    _display->fillRect(4 + int(_dots_drawn % _dot_columns) * 8,
                      _dot_top + int(_dots_drawn / _dot_columns) * 8, 3, 3);
    ++_dots_drawn;
    // Preserve the initial label and every previous dot in the framebuffer.
    _display->endFrame();
    _last_frame = millis();
    // A partial refresh can block for a second. Give entropy collection ten
    // times its measured cost, capped at a minute. Exclude the initial full
    // refresh and saturate before multiplying an unbounded duration.
    const uint32_t duration = uint32_t(_last_frame - started);
    _eink_interval = duration >= 6000 ? 60000 : duration * 10;
    if (_eink_interval < 5000) _eink_interval = 5000;
  }

  void draw() {
    if (_display == nullptr || (_quiet && !_keygen_visible)) return;
    DisplayDriver& display = *_display;
    display.startFrame();
    display.setTextSize(1);
    display.setCompactText(false);
    display.setColor(UIColor::primary_txt);
    const int line = display.textLineHeight();
    const char* first = _status;
    const char* second = nullptr;
    if (display.getTextWidth(first) > display.width()) {
      first = _generating ? "Generating" :
          strcmp(_status, "Loading identity") == 0 ? "Loading" :
          strcmp(_status, "Saving identity") == 0 ? "Saving" : "Starting";
      if (_generating) second = "key";
      else if (strcmp(_status, "Starting...") != 0) second = "identity";
      if (display.getTextWidth(first) > display.width()) {
        first = _generating ? "Key" :
            strcmp(_status, "Loading identity") == 0 ? "Load" :
            strcmp(_status, "Saving identity") == 0 ? "Save" : "Boot";
        if (_generating) second = "gen";
      }
    }
    if (second != nullptr && display.getTextWidth(second) > display.width())
      second = "ID";
    const int spinner_size = 12;
    const int text_height = line * (second == nullptr ? 1 : 2);
    const bool eink_progress = _generating && display.isEink();
    const bool elapsed = _generating && !eink_progress
        && text_height + spinner_size + 4 + line <= display.height();
    const int total_height = text_height
        + (_generating && !eink_progress ? spinner_size + 2 : 0)
        + (elapsed ? line + 2 : 0);
    int y = (display.height() - total_height) / 2;
    if (eink_progress) y = 4;
    if (y < 0) y = 0;
    display.drawTextCentered(display.width() / 2, y, first);
    if (second != nullptr)
      display.drawTextCentered(display.width() / 2, y + line, second);
    if (eink_progress) {
      _dot_top = y + text_height + 8;
      _dot_columns = display.width() >= 11 ? (display.width() - 11) / 8 + 1 : 0;
      _dot_rows = display.height() >= _dot_top + 7
          ? (display.height() - _dot_top - 7) / 8 + 1 : 0;
      _dots_drawn = 0;
      if (_dot_columns && _dot_rows) {
        display.fillRect(4, _dot_top, 3, 3);
        _dots_drawn = 1;
      }
    } else if (_generating) {
      // A rotating bright pair on an eight-dot ring works on monochrome and
      // color panels, without depending on a font's spinner characters.
      static const uint8_t dots[8][2] = {
          {5, 0}, {9, 1}, {10, 5}, {9, 9},
          {5, 10}, {1, 9}, {0, 5}, {1, 1}};
      const int x = (display.width() - spinner_size) / 2;
      const int top = y + text_height + 2;
      if (x >= 0 && top + spinner_size <= display.height()) {
        for (uint8_t i = 0; i < 2; ++i) {
          const uint8_t index = (_phase + i) & 7;
          display.fillRect(x + dots[index][0], top + dots[index][1], 2, 2);
        }
      }
      if (elapsed) {
        char seconds[16];
        snprintf(seconds, sizeof(seconds), "%lus",
                 (unsigned long)(uint32_t(millis() - _generation_started) / 1000));
        if (display.getTextWidth(seconds) <= display.width())
          display.drawTextCentered(display.width() / 2,
                                   top + spinner_size + 2, seconds);
      }
    }
    display.endFrame();
#if defined(COMPANION_PAIRING_UI_HIL)
    companionStartupUiHilFrame(_status);
#endif
    _last_frame = millis();
  }

public:
  void begin(DisplayDriver* display, bool usb_power) {
    const auto& prefs = displayPowerPrefs();
    const auto mode = (usb_power ? prefs.usb : prefs.battery).mode;
    _display = display;
    _quiet = mode == DisplayMode::Off || mode == DisplayMode::Pairing;
    _keygen_visible = false;
    _generating = false;
    if (_quiet) {
      if (_display != nullptr && _display->isOn()) _display->turnOff();
      return;
    }
    if (_display == nullptr) return;
    if (!_display->isOn()) _display->turnOn();
    starting();
  }

  void starting() {
    _generating = false;
    _keygen_visible = false;
    _status = "Starting...";
    if (_quiet && _display != nullptr && _display->isOn()) _display->turnOff();
    draw();
  }

  void loadingIdentity() {
    _generating = false;
    _status = "Loading identity";
    draw();
  }

  void savingIdentity() {
    _generating = false;
    _status = "Saving identity";
    draw();
  }

  void generatingKey() {
    _keygen_visible = true;
    _generating = true;
    _status = "Generating key";
    _phase = 0;
    _generation_started = millis();
    _eink_interval = 5000;
    if (_display != nullptr && !_display->isOn()) _display->turnOn();
    draw();
  }

  void service() {
    if (_display == nullptr || !_generating) return;
    // E-paper's partial refresh is slower and needs a gentler refresh cadence.
    const uint32_t interval = _display->isEink() ? _eink_interval : 500UL;
    if (uint32_t(millis() - _last_frame) < interval) return;
    if (_display->isEink()) {
      appendEinkDot();
      return;
    }
    _phase = (_phase + 1) & 7;
    draw();
  }

  static void progress(void* context) {
    if (context != nullptr) static_cast<StartupScreen*>(context)->service();
  }
};

} }
