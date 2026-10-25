#pragma once

#include <Mesh.h>

class ESPNOWRadio : public mesh::Radio {
protected:
  uint32_t n_recv, n_sent, n_recv_errors;
  bool initialized_ = false;
  bool wake_held_ = false;
  int8_t tx_power_dbm_ = 20;
  bool send_active_ = false;
  bool send_cancelled_ = false;

public:
  ESPNOWRadio() { n_recv = n_sent = n_recv_errors = 0; }

  uint32_t getRngSeed();

  mesh::RadioParamApplyResult trySetParams(float freq, float bw, uint8_t sf, uint8_t cr,
                                           const uint32_t* rx_ps_timings = nullptr) override {
    (void)freq;
    (void)bw;
    (void)sf;
    (void)cr;
    (void)rx_ps_timings;
    return mesh::RadioParamApplyResult::APPLIED;
  }
  bool setParams(float freq, float bw, uint8_t sf, uint8_t cr,
                 const uint32_t* rx_ps_timings = nullptr) {
    return trySetParams(freq, bw, sf, cr, rx_ps_timings) == mesh::RadioParamApplyResult::APPLIED;
  }
  void powerOff() { end(); }

  void init();
  void end();
  bool isEnabled() const { return initialized_; }
  int recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int len_bytes) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override;

  uint32_t getCADBusyCount() const { return 0; }  // ESP-NOW has no LoRa CAD

  uint32_t getPacketsRecv() const { return n_recv; }
  uint32_t getPacketsSent() const { return n_sent; }
  uint32_t getPacketsRecvErrors() const { return n_recv_errors; }
  void resetStats() { n_recv = n_sent = n_recv_errors = 0; }

  virtual float getLastRSSI() const override;
  virtual float getLastSNR() const override;

  float packetScore(float snr, int packet_len) override { return 0; }

  /**
   * These two functions do nothing for ESP-NOW, but are needed for the
   * Radio interface.
   */
  virtual bool setRxBoostedGainMode(bool) { return false; }
  virtual bool supportsRxBoostedGainMode() const { return false; }
  virtual bool getRxBoostedGainMode() const { return false; }

  // ESP-NOW has no LoRa RX power-saving watchdog or noise-floor calibration.
  uint32_t getRxPsWatchdogSoftCount() const { return 0; }
  uint32_t getRxPsWatchdogHardCount() const { return 0; }
  bool isWatchdogObserving() const { return false; }
  bool isCalibratingNoiseFloor() const { return false; }

  uint32_t intID();
  bool isTxPowerValid(int8_t dbm) const {
    // The SDK accepts 8..84 quarter-dBm units; the public API uses whole dBm.
    if (dbm < 2 || dbm > 21) return false;
#ifdef MIN_LORA_TX_POWER
    if (dbm < MIN_LORA_TX_POWER) return false;
#endif
#ifdef MAX_LORA_TX_POWER
    if (dbm > MAX_LORA_TX_POWER) return false;
#endif
    return true;
  }
  bool isTxPowerApplied(int8_t dbm) const {
    return initialized_ && tx_power_dbm_ == dbm;
  }
  bool setTxPower(int8_t dbm);
  mesh::RadioParamApplyResult trySetTxPower(int8_t dbm) {
    return isTxPowerValid(dbm) && setTxPower(dbm) ? mesh::RadioParamApplyResult::APPLIED
                           : mesh::RadioParamApplyResult::FAILED;
  }
};

#if ESPNOW_DEBUG_LOGGING && ARDUINO
  #include <Arduino.h>
  #define ESPNOW_DEBUG_PRINT(F, ...) do { if (mesh::isUsbDebugLoggingEnabled()) { mesh::usbLoggingPort().printf("ESP-Now: " F, ##__VA_ARGS__); } } while(0)
  #define ESPNOW_DEBUG_PRINTLN(F, ...) do { if (mesh::isUsbDebugLoggingEnabled()) { mesh::usbLoggingPort().printf("ESP-Now: " F "\n", ##__VA_ARGS__); } } while(0)
#else
  #define ESPNOW_DEBUG_PRINT(...) {}
  #define ESPNOW_DEBUG_PRINTLN(...) {}
#endif
