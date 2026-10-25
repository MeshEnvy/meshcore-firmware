#pragma once

#include <Arduino.h>
#include <helpers/RefCountedDigitalPin.h>
#include <helpers/ESP32Board.h>
#include <helpers/radiolib/LR2021Band.h>
#include <driver/rtc_io.h>

#ifndef ADC_MULTIPLIER
  #define ADC_MULTIPLIER 5.42
#endif

class MeshnologyW12Board : public ESP32Board {

protected:
  float adc_mult = ADC_MULTIPLIER;
  bool radio_band_initialized = false;
  bool radio_high_band = false;
  bool (*radio_rf_switch_handler)(bool lf_awake, bool sleep_after) = nullptr;

public:
  RefCountedDigitalPin periph_power;
  MeshnologyW12Board() : periph_power(PIN_VEXT_EN, PIN_VEXT_EN_ACTIVE) { }

  void begin();
  void applyLedPolicy() override;
  void onBeforeTransmit(void) override;
  void onAfterTransmit(void) override;
  void enterDeepSleep(uint32_t secs, int pin_wake_btn = -1);
  void powerOff() override;
  void shutdownPeripherals() override;
  bool prepareRadioFrequency(float frequency) override;
  bool supportsRadioHardReset() const override { return true; }
  bool resetRadio() override;
  void setRadioRfSwitchHandler(bool (*handler)(bool lf_awake, bool sleep_after)) {
    radio_rf_switch_handler = handler;
  }
  bool supportsBatteryCalibration() const override { return true; }

  uint16_t getBattMilliVolts() override;
  bool setAdcMultiplier(float multiplier) override {
    if (!isfinite(multiplier) || multiplier < 0.0f || multiplier > 10.0f) return false;
    if (multiplier == 0.0f) {
      adc_mult = ADC_MULTIPLIER;
    } else {
      adc_mult = multiplier;
    }
    return true;
  }
  float getAdcMultiplier() const override { return adc_mult; }
  const char* getManufacturerName() const override;
};
