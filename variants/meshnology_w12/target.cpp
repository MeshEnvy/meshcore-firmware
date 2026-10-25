#include <Arduino.h>
#include "target.h"
#include <helpers/radiolib/TunableRadioHal.h>

MeshnologyW12Board board;

#if defined(P_LORA_SCLK)
  static SPIClass spi;
  static TunableRadioHal radio_hal(spi, 2000000);
  RADIO_CLASS radio = new Module(&radio_hal, P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
#else
  static TunableRadioHal radio_hal(SPI, 2000000, true);
  RADIO_CLASS radio = new Module(&radio_hal, P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
#endif

WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);

#if ENV_INCLUDE_GPS
  #include <helpers/sensors/MicroNMEALocationProvider.h>
  MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
  EnvironmentSensorManager sensors = EnvironmentSensorManager(nmea);
#else
  EnvironmentSensorManager sensors;
#endif

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display(NULL);
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

const uint32_t rfswitch_dios[] = {
  RADIOLIB_LR2021_DIO5,   // RFX2402E 2.4G_TX_EN
  RADIOLIB_LR2021_DIO6,   // RFX2402E 2.4G_RX_EN
  RADIOLIB_LR2021_DIO9,   // GC1109 CTX (Transmit mode)
  RADIOLIB_LR2021_DIO10,  // GC1109 CPS (Bypass mode)
  RADIOLIB_LR2021_DIO11,  // GC1109 CSD (Shutdown)
};

static const Module::RfSwitchMode_t rfswitch_table[] = {
  //                    DIO5   DIO6  DIO9  DIO10  DIO11
  // PR #3579 (7457cff): retain LF PA bias between packets to avoid a weak
  // packet opening. This table is selected only while the LF rail is powered.
  { LR2021::MODE_STBY,  { LOW,  LOW,  HIGH, HIGH, HIGH } },  // GC1109 stays awake
  { LR2021::MODE_RX,    { LOW,  LOW,  LOW,  HIGH, HIGH } },  // rx_lf: GC1109 LNA (CPS is don't-care in RX)
  { LR2021::MODE_TX,    { LOW,  LOW,  HIGH, HIGH, HIGH } },  // tx_lf: GC1109 full PA
  { LR2021::MODE_RX_HF, { LOW,  HIGH, LOW,  LOW,  LOW  } },  // rx_hf: 2G4 LNA, GC1109 off
  { LR2021::MODE_TX_HF, { HIGH, LOW,  LOW,  LOW,  LOW  } },  // tx_hf: 2G4 PA,  GC1109 off
  END_OF_MODE_TABLE,
};

// Keep both FEMs' controls low in standby while switching supplies or using
// the HF rail. GC1109 control voltage must not exceed its switched VBAT.
static const Module::RfSwitchMode_t rfswitch_table_cold[] = {
  { LR2021::MODE_STBY,  { LOW, LOW, LOW, LOW, LOW } },
  rfswitch_table[1],
  rfswitch_table[2],
  rfswitch_table[3],
  rfswitch_table[4],
  END_OF_MODE_TABLE,
};

static bool set_w12_rf_switch_standby(bool lf_awake, bool sleep_after) {
  if (radio.replaceRfSwitchTable(rfswitch_dios,
      lf_awake ? rfswitch_table : rfswitch_table_cold) != RADIOLIB_ERR_NONE) return false;
  return !sleep_after || radio.sleep() == RADIOLIB_ERR_NONE;
}

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);

#if defined(P_LORA_SCLK)
  int err = radio.std_init(&spi);
  if (err != 1) return err;
#else
  int err = radio.std_init();
  if (err != 1) return err;
#endif

#ifdef RF_SWITCH_TABLE
  // The callback is installed only after initialization: board.begin() may
  // select its first supply before the radio is ready to accept SPI commands.
  board.setRadioRfSwitchHandler(set_w12_rf_switch_standby);
  if (!set_w12_rf_switch_standby(!mesh::lr2021::isHighBand(radio.getFreqMHz()), false)) return false;
#endif

  return true;
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}
