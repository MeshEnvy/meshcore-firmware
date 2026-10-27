#pragma once

#include <LovyanGFX.hpp>
#include <helpers/ui/DisplayDriver.h>


class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel_instance_;
  lgfx::Bus_SPI bus_instance_;
  lgfx::Light_PWM light_instance_;

public:
  LGFX();
};


class CardputerDisplay : public DisplayDriver {
public:
  // UI text size setting; each level loads its own DejaVu Sans font.
  enum TextLevel : uint8_t { TEXT_NORMAL = 0, TEXT_LARGE = 1, TEXT_XLARGE = 2, TEXT_LEVEL_COUNT };

private:
  bool _isOn = false;
  uint16_t _lastColor = 0;
  uint8_t _textLevel = TEXT_LARGE;
  uint8_t _fontPx = 12;  // em size of the loaded font
  float _scale = 1.0f;   // current setTextSize() scale
  LGFX LCD;

public:
  enum PaletteColor : uint16_t {
    P_BLACK = TFT_BLACK,
    P_WHITE = TFT_WHITE,
    P_GREEN = TFT_GREEN,
    P_BLUE = TFT_BLUE,
    P_RED = TFT_RED,
    P_ORANGE = TFT_ORANGE,
    P_YELLOW = TFT_YELLOW,
    P_GRAY = TFT_GRAY,
  };

  CardputerDisplay() : DisplayDriver(240, 135) {}
  bool begin();
  void setBrightness(uint8_t level) { LCD.setBrightness(level); }

  bool isOn() override { return _isOn; };
  void turnOn() override;
  void turnOff() override;
  void clear() override;
  void startFrame(ColorVal bkg = UIColor::window_bkg) override;
  void setTextSize(int sz) override;
  void setColor(ColorVal c) override;
  void setCursor(int x, int y) override;
  void print(const char *str) override;
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t *bits, int w, int h) override;
  void endFrame() override;
  uint16_t getTextWidth(const char *str) override;
  int32_t getFontHeight() const;
  // Line advance used by LovyanGFX when wrapping text (chat history).
  int16_t getFontLineHeight() const;
  // Row pitch for lists and stacked labels at the current text size.
  int16_t getRowHeight() const { return (int16_t)(_fontPx * _scale + 0.5f); }

  void setTextLevel(uint8_t level);
  uint8_t getTextLevel() const { return _textLevel; }
  static const char *textLevelName(uint8_t level);
  // Status bar text stays at the Normal size whatever the setting.
  void setSmallText();
  void drawTextLeftAlignWithScroll(int x, int y, int available_width, const char *text);
};
