/***********************************************************************************************
 * CalibrationAirSupply - concrete ICalibrationAirSupply over the real controllers
 *
 * Bridges the AutoCalibrator to the flute's actual air source for the current
 * cfg.airMode. It never re-implements pump/fan control: it delegates to the same
 * PressureController / FanController calls InstrumentManager uses while playing, so
 * calibration and play share one behaviour. InstrumentManager::update() keeps
 * ticking those controllers during calibration (the main loop calls it every pass),
 * so readiness converges without this class running its own PID/ramp.
 ***********************************************************************************************/
#ifndef CALIBRATION_AIR_SUPPLY_H
#define CALIBRATION_AIR_SUPPLY_H

#include "ICalibrationAirSupply.h"

class PressureController;
class FanController;

class CalibrationAirSupply : public ICalibrationAirSupply {
public:
  CalibrationAirSupply(PressureController& pressure, FanController& fan);

  void prepare() override;
  bool isReady() override;
  void setDemandPercent(uint8_t percent) override;
  void stopSafe() override;
  CalAirSupplyError getError() const override { return _error; }

private:
  PressureController& _pressure;
  FanController& _fan;
  CalAirSupplyError _error;
  unsigned long _prepareTime;

  // Representative source demand (%) for the current air mode: enough air to reach
  // the whole airflow sweep. Derived from the same config values used during play.
  uint8_t representativeDemand() const;
};

#endif // CALIBRATION_AIR_SUPPLY_H
