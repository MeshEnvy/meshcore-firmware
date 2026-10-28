#pragma once

#include <helpers/SensorManager.h>
#include <helpers/sensors/LocationProvider.h>

class CardputerSensorManager : public SensorManager {
private:
  bool gps_active = false;
  bool gps_serial_open = false;
  uint32_t next_gps_update = 0;
  uint32_t standby_renew_at = 0;
  LocationProvider *_location;
  void armGpsPowerSavingCycle();

protected:
  bool telemetryGpsDetected() const override { return gps_serial_open; }
  bool telemetryGpsActive() const override { return gps_active; }
  void telemetryGpsStart() override { start_gps(); }
  void telemetryGpsStop() override { stop_gps(); }

public:
  CardputerSensorManager(LocationProvider &location) : _location(&location) {}
  bool begin() override;
  bool querySensors(uint8_t requester_permissions, CayenneLPP &telemetry) override;
  void loop() override;
  int getNumSettings() const override;
  const char *getSettingName(int i) const override;
  const char *getSettingValue(int i) const override;
  bool setSettingValue(const char *name, const char *value) override;
  void setPowerSavingEnabled(bool enabled) override;
  bool isGPSDetected() const override { return gps_serial_open; }
  // The cap has command-controlled standby, not a GPIO power switch. A
  // pinless LocationProvider::isEnabled() cannot report its logical state.
  bool isGpsPowered() const override { return gps_active; }
  bool gpsUsesSerialUart(uint8_t uart) const override { return uart == 1 && gps_serial_open; }
  void shutdownGpsTransportForDeepSleep() override;
  LocationProvider *getLocationProvider() override { return _location; }
  void start_gps();
  void stop_gps();
  void gps_standby(bool active);
};
