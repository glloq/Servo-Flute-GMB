#include "CalibrationAirSupply.h"
#include "settings.h"
#include "ConfigStorage.h"
#include "PressureController.h"
#include "FanController.h"

CalibrationAirSupply::CalibrationAirSupply(PressureController& pressure, FanController& fan)
  : _pressure(pressure), _fan(fan), _error(CAL_AIR_OK), _prepareTime(0) {}

uint8_t CalibrationAirSupply::representativeDemand() const {
  switch (cfg.airMode) {
    case AIR_MODE_FAN_SERVO:
      // Enough fan speed to feed the loudest position, never below idle.
      return cfg.fanMaxNotePercent > cfg.fanDefaultPercent ? cfg.fanMaxNotePercent
                                                           : cfg.fanDefaultPercent;
    case AIR_MODE_PUMP_VALVE:
      return cfg.pumpDirectMaxPercent > cfg.pumpDirectIdlePercent ? cfg.pumpDirectMaxPercent
                                                                  : cfg.pumpDirectIdlePercent;
    case AIR_MODE_PUMP_RESERVOIR:
      return cfg.reservoirTargetPercent;
    default:
      return 0; // passive modes have no controllable source
  }
}

void CalibrationAirSupply::prepare() {
  _error = CAL_AIR_OK;
  _prepareTime = millis();
  switch (cfg.airMode) {
    case AIR_MODE_FAN_SERVO:
      // Mirror a sustained note so the fan does not ramp down to idle mid-sweep.
      _fan.onNoteOn();
      _fan.setSpeed(representativeDemand());
      break;
    case AIR_MODE_PUMP_VALVE:
      _pressure.setTargetPercent(representativeDemand());
      break;
    case AIR_MODE_PUMP_RESERVOIR:
      if (!_pressure.isSensorDetected()) {
        // A pressurised reservoir with no usable level/pressure sensor cannot be
        // regulated safely: refuse rather than run the pump blind. Best-effort
        // must be an explicit, user-assumed configuration, never the default.
        _error = CAL_AIR_SENSOR_FAULT;
        _pressure.stop();
        break;
      }
      _pressure.setTargetPercent(representativeDemand());
      break;
    default:
      break; // passive: nothing to start
  }
}

bool CalibrationAirSupply::isReady() {
  switch (cfg.airMode) {
    case AIR_MODE_FAN_SERVO:
      return _fan.isReady();
    case AIR_MODE_PUMP_VALVE:
      // Direct pump, no reservoir: usable as soon as the motor is turning.
      return _pressure.isPumpRunning();
    case AIR_MODE_PUMP_RESERVOIR:
      // Strict by default: without a usable sensor the reservoir is never "ready"
      // (getError() reports sensor_fault, and the calibrator aborts the run).
      if (!_pressure.isSensorDetected()) return false;
      {
        int target = (int)cfg.reservoirTargetPercent - AUTOCAL_RESERVOIR_READY_MARGIN;
        if (target < 0) target = 0;
        return (int)_pressure.getFillPercent() >= target;
      }
    default:
      return true; // passive supplies are always "ready"
  }
}

void CalibrationAirSupply::setDemandPercent(uint8_t percent) {
  if (percent > 100) percent = 100;
  // Map 0-100% of "calibration demand" onto the mode's representative range so the
  // source stays fed across the whole airflow sweep.
  uint16_t demand = ((uint16_t)representativeDemand() * percent) / 100;
  switch (cfg.airMode) {
    case AIR_MODE_FAN_SERVO:
      if (demand < cfg.fanDefaultPercent) demand = cfg.fanDefaultPercent;
      _fan.setSpeed((uint8_t)demand);
      break;
    case AIR_MODE_PUMP_VALVE:
      if (demand < cfg.pumpDirectIdlePercent) demand = cfg.pumpDirectIdlePercent;
      _pressure.setTargetPercent((uint8_t)demand);
      break;
    case AIR_MODE_PUMP_RESERVOIR:
      // Reservoir stays at its target pressure; the valve/servo do the modulation.
      _pressure.setTargetPercent(cfg.reservoirTargetPercent);
      break;
    default:
      break;
  }
}

void CalibrationAirSupply::stopSafe() {
  switch (cfg.airMode) {
    case AIR_MODE_FAN_SERVO:
      _fan.onNoteOff();
      _fan.stop();
      break;
    case AIR_MODE_PUMP_VALVE:
    case AIR_MODE_PUMP_RESERVOIR:
      _pressure.stop();
      break;
    default:
      break;
  }
}
