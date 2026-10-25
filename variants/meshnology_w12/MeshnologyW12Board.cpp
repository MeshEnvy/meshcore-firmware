#include "MeshnologyW12Board.h"
#include <helpers/ESP32GpioHold.h>

void MeshnologyW12Board::begin() {
    ESP32Board::begin();

    // An MCU-only reset can leave the radio's warm LF controls HIGH. Assert
    // radio reset before releasing or dropping either retained FEM supply.
    mesh::esp32::releaseOutputAfterDeepSleep((gpio_num_t)P_LORA_RESET, LOW);
    delay(1);
    // Keep both FEM rails off while releasing a retained terminal shutdown.
    mesh::esp32::releaseOutputAfterDeepSleep((gpio_num_t)P_LORA_LF_PA_POWER, LOW);
    mesh::esp32::releaseOutputAfterDeepSleep((gpio_num_t)P_LORA_HF_PA_POWER, LOW);
    pinMode(PIN_ADC_CTRL, OUTPUT);
    digitalWrite(PIN_ADC_CTRL, LOW); // Disable battery sense until required

    pinMode(P_LORA_LF_PA_POWER, OUTPUT);
    pinMode(P_LORA_HF_PA_POWER, OUTPUT);
    digitalWrite(P_LORA_LF_PA_POWER, LOW);
    digitalWrite(P_LORA_HF_PA_POWER, LOW);
    prepareRadioFrequency(LORA_FREQ);
    digitalWrite(P_LORA_RESET, HIGH);

    periph_power.begin();
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_DEEPSLEEP) {
      long wakeup_source = esp_sleep_get_ext1_wakeup_status();
      if (wakeup_source & (1 << P_LORA_DIO_1)) {  // received a LoRa packet (while in deep sleep)
        startup_reason = BD_STARTUP_RX_PACKET;
    }

      mesh::esp32::releaseRtcGpioIfSupported((gpio_num_t)P_LORA_NSS);
      mesh::esp32::deinitRtcGpioIfSupported((gpio_num_t)P_LORA_DIO_1);
    }
  }

  void MeshnologyW12Board::applyLedPolicy() {
    if (!mesh::leds::enabled()) neopixelWrite(NEOPIXEL_LED, 0, 0, 0);
  }

  void MeshnologyW12Board::onBeforeTransmit(void) {
    if (mesh::leds::enabled()) neopixelWrite(NEOPIXEL_LED, NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS);   // turn TX neopixel on (White)
  }

  void MeshnologyW12Board::onAfterTransmit(void) {
    neopixelWrite(NEOPIXEL_LED, 0, 0, 0);   // turn TX neopixel off
  }

  bool MeshnologyW12Board::prepareRadioFrequency(float frequency) {
    const bool high_band = mesh::lr2021::isHighBand(frequency);
    if (radio_band_initialized && high_band == radio_high_band) return true;

    // DIO controls must be LOW before either FEM loses its supply. A failed
    // transition remains uninitialized so even a same-band retry repeats it.
    radio_band_initialized = false;
    if (radio_rf_switch_handler && !radio_rf_switch_handler(false, false)) return false;

    // The W12 schematic connects PA_EN_M (GPIO4) to the GC1109 sub-GHz rail
    // and PA_EN_G (GPIO3) to the RFX2402E 2.4 GHz rail. Keep only the selected
    // FEM powered, and disable the old path before enabling the new one.
    digitalWrite(P_LORA_LF_PA_POWER, LOW);
    digitalWrite(P_LORA_HF_PA_POWER, LOW);
    digitalWrite(high_band ? P_LORA_HF_PA_POWER : P_LORA_LF_PA_POWER, HIGH);
    // PR #3579 uses a 1 ms FEM rail settling delay. Only after that delay
    // may the selected LF FEM receive its retained standby control bias.
    delay(1);
    if (radio_rf_switch_handler && !radio_rf_switch_handler(!high_band, false)) return false;

    radio_high_band = high_band;
    radio_band_initialized = true;
    return true;
  }

  bool MeshnologyW12Board::resetRadio() {
    // Recovery must pulse NRST before rail preparation, whose checked DIO
    // routing would otherwise depend on the SPI fault being recovered.
    radio_band_initialized = false;
    mesh::esp32::releaseOutputAfterDeepSleep((gpio_num_t)P_LORA_RESET, LOW);
    delay(10);
    digitalWrite(P_LORA_RESET, HIGH);
    // Match RadioLib's LRxxxx reset startup interval and bounded BUSY wait.
    delay(300);
    const uint32_t started = millis();
    while (digitalRead(P_LORA_BUSY)) {
      yield();
      if (millis() - started >= 3000) return false;
    }
    return true;
  }

  void MeshnologyW12Board::enterDeepSleep(uint32_t secs, int pin_wake_btn) {
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    // Make sure the DIO1 and NSS GPIOs are hold on required levels during deep sleep
    rtc_gpio_set_direction((gpio_num_t)P_LORA_DIO_1, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en((gpio_num_t)P_LORA_DIO_1);

    mesh::esp32::holdRtcGpioIfSupported((gpio_num_t)P_LORA_NSS);

    // Packet-wake sleep leaves the radio receiving. Retain the selected FEM
    // rail and the inactive rail's LOW level; both PA enable pins are RTC GPIOs
    // on this ESP32-S3. Terminal powerOff() deliberately retains both LOW.
    mesh::esp32::holdOutputForDeepSleep((gpio_num_t)P_LORA_LF_PA_POWER, radio_high_band ? LOW : HIGH);
    mesh::esp32::holdOutputForDeepSleep((gpio_num_t)P_LORA_HF_PA_POWER, radio_high_band ? HIGH : LOW);

    if (pin_wake_btn < 0) {
      esp_sleep_enable_ext1_wakeup( (1L << P_LORA_DIO_1), ESP_EXT1_WAKEUP_ANY_HIGH);  // wake up on: recv LoRa packet
    } else {
      esp_sleep_enable_ext1_wakeup( (1L << P_LORA_DIO_1) | (1L << pin_wake_btn), ESP_EXT1_WAKEUP_ANY_HIGH);  // wake up on: recv LoRa packet OR wake btn
    }

    if (secs > 0) {
      esp_sleep_enable_timer_wakeup(secs * 1000000);
    }

    // Finally set ESP32 into sleep
    esp_deep_sleep_start();   // CPU halts here and never returns!
  }

  void MeshnologyW12Board::shutdownPeripherals() {
    // Quiesce controls while the selected FEM is still powered, then let the
    // radio enter sleep before removing its FEM supplies.
    if (radio_rf_switch_handler) radio_rf_switch_handler(false, false);
    ESP32Board::shutdownPeripherals();
    // Stopping an active Morse carrier can restore a different packet band
    // during base shutdown. Base shutdown also holds NSS HIGH; release only
    // that hold for the final checked cold routing/sleep, then retain NSS again.
    mesh::esp32::releaseOutputAfterDeepSleep((gpio_num_t)P_LORA_NSS, HIGH);
    const bool controls_low = !radio_rf_switch_handler || radio_rf_switch_handler(false, true);
    mesh::esp32::holdOutputForDeepSleep((gpio_num_t)P_LORA_NSS, HIGH);
    if (!controls_low) {
      // A failed SPI response cannot prove that the controls went LOW. Hold
      // the transceiver in reset before removing the external FEM supplies.
      mesh::esp32::holdOutputForDeepSleep((gpio_num_t)P_LORA_RESET, LOW);
      delay(1);
    }
    radio_band_initialized = false;
    mesh::esp32::holdOutputForDeepSleep((gpio_num_t)P_LORA_LF_PA_POWER, LOW);
    mesh::esp32::holdOutputForDeepSleep((gpio_num_t)P_LORA_HF_PA_POWER, LOW);
  }

  void MeshnologyW12Board::powerOff() {
    // Base powerOff dispatches shutdownPeripherals() above, clears stale wake
    // sources, and enters terminal deep sleep with both FEM rails retained LOW.
    ESP32Board::powerOff();
  }

  uint16_t MeshnologyW12Board::getBattMilliVolts()  {
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    digitalWrite(PIN_ADC_CTRL, HIGH);
    delay(10);
    uint32_t raw = 0;
    for (int i = 0; i < 8; i++) {
      raw += analogRead(PIN_VBAT_READ);
    }
    raw = raw / 8;

    digitalWrite(PIN_ADC_CTRL, LOW);

    return (adc_mult * (3.3 / 4096.0) * raw) * 1000;
  }

  const char* MeshnologyW12Board::getManufacturerName() const {
    return "Meshnology W12";
  }
