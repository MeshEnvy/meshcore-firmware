#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <helpers/ESP32Board.h>
#include "globals.h"

#if CARDPUTER_ADV_UI
  #include <FS.h>
  #include "hw/CardputerSpeaker.h"
  #include "hw/CardputerKeyboard.h"
  #include "hw/CardputerLayout.h"
#endif

// M5Stack Cardputer-Adv with the Cap LoRa-1262 (SX1262 + ATGM336H GNSS).
// The internal I2C bus (G8/G9, Wire1) carries the keyboard, the audio codec
// and the Cap's PI4IOE5V6408 expander; Wire stays on the Grove port (G2/G1).
// The radio cannot reach the antenna until expander P0 is driven high.
// Sleep, deep sleep and power-off are ESP32Board's (no LoRa wake on power-off).
// Keyboard/display/speaker UI adapted from MultiMote/meshcore-cardputer-adv
// (MIT, see LICENSE.MultiMote).
#define CARDPUTER_PI4IO_REG_IO_DIR  0x03
#define CARDPUTER_PI4IO_REG_OUT_SET 0x05
#define CARDPUTER_PI4IO_REG_OUT_HIZ 0x07

class CardputerAdvBoard : public ESP32Board {
  bool _lora_cap_ok = false;
#if CARDPUTER_ADV_UI
  CardputerSpeaker speaker;
  CardputerKeyboard keyboard;
  CardputerLayout layout;
#endif

  static bool pi4ioWrite(uint8_t reg, uint8_t val) {
    Wire1.beginTransmission(LORA_IOE_I2C_ADDRESS);
    Wire1.write(reg);
    Wire1.write(val);
    return Wire1.endTransmission() == 0;
  }

public:
  void begin() {
    ESP32Board::begin();  // starts Wire on PIN_BOARD_SDA/SCL (Grove)

    Wire1.begin(PIN_I2C_INTERNAL_SDA, PIN_I2C_INTERNAL_SCL, 100000);
    _lora_cap_ok = pi4ioWrite(CARDPUTER_PI4IO_REG_IO_DIR, 0x01)
        && pi4ioWrite(CARDPUTER_PI4IO_REG_OUT_HIZ, 0x00)
        && pi4ioWrite(CARDPUTER_PI4IO_REG_OUT_SET, 0x01);

#if CARDPUTER_ADV_UI
    keyboard.begin();
#endif
  }

  bool isLoraCapReady() const { return _lora_cap_ok; }

  bool isUserGpioAvailable(uint8_t pin) const override {
    // These built-in buses stay connected in headless roles too. Their pin
    // names are local to this board, so the shared ESP32 policy cannot see
    // them. Never let a saved user output take over the antenna, LCD or audio.
    const uint8_t internal_pins[] = {
      PIN_I2C_INTERNAL_SDA, PIN_I2C_INTERNAL_SCL, PIN_KEYBOARD_INT,
      PIN_TFT_SCK, PIN_TFT_MOSI, PIN_TFT_RST, PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_BL,
      PIN_AUDIO_SCLK, PIN_AUDIO_ASDOUT, PIN_AUDIO_LRCK, PIN_AUDIO_DSDIN,
      PIN_TOP_BUTTON, PIN_SD_CS,
    };
    for (uint8_t reserved : internal_pins) {
      if (pin == reserved) return false;
    }
    return ESP32Board::isUserGpioAvailable(pin);
  }

#if CARDPUTER_ADV_UI
  CardputerSpeaker *getSpeaker() { return &speaker; }
  CardputerKeyboard *getKeyboard() { return &keyboard; }
  CardputerLayout *getLayout() { return &layout; }
  void readKeyboardLayout(FS &fs) { layout.begin(fs); }
#endif

  const char* getManufacturerName() const override {
    return "M5Stack Cardputer-Adv";
  }
};
