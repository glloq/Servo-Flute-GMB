/***********************************************************************************************
 * ICalibrationAirSupply - air-source abstraction used by the auto-calibrator
 *
 * The flute can be fed by six different air architectures (settings.h AIR_MODE_*):
 *   0 solenoid + servo flow          (passive / external supply)
 *   1 servo valve  + servo flow      (passive / external supply)
 *   2 servo flow only                (passive / external supply)
 *   3 fan          + servo flow      (fan must be spun up)
 *   4 pump + valve + servo flow      (pump must be running, no reservoir)
 *   5 pump + reservoir + valve + ...  (reservoir must be pressurised, with sensor)
 *
 * For calibration to measure anything meaningful the air SOURCE must first be in a
 * representative running state: the pump spinning, the reservoir at pressure or the
 * fan at speed. Otherwise every note fails as "no sound" for the wrong reason, and
 * the noise floor is measured without the acoustic contribution of the running
 * pump/fan (which is exactly the noise the calibration must reject).
 *
 * This interface lets the AutoCalibrator drive any of the six modes without knowing
 * their controllers. The concrete implementation (CalibrationAirSupply.*) delegates
 * to the very same PressureController / FanController calls used during normal play,
 * so there is no duplicated air-handling logic. A fake implementation is used by the
 * native tests.
 *
 * All methods are non-blocking (no delay()); readiness is polled via isReady().
 ***********************************************************************************************/
#ifndef ICALIBRATION_AIR_SUPPLY_H
#define ICALIBRATION_AIR_SUPPLY_H

#include <Arduino.h>

enum CalAirSupplyError {
  CAL_AIR_OK = 0,
  CAL_AIR_NOT_READY,      // still spinning up / pressurising (transient)
  CAL_AIR_TIMEOUT,        // never reached a usable state
  CAL_AIR_SENSOR_FAULT,   // reservoir sensor missing/faulty: readiness is best-effort
  CAL_AIR_FAULT           // generic fault
};

class ICalibrationAirSupply {
public:
  virtual ~ICalibrationAirSupply() {}

  // Begin bringing the air source to a representative running state. Non-blocking:
  // returns immediately; the caller polls isReady(). Passive modes do nothing.
  virtual void prepare() = 0;

  // True once the source can sustain airflow for measurement (pump running,
  // reservoir at target, fan at speed). Passive modes are ready immediately.
  virtual bool isReady() = 0;

  // Set the source demand (0-100%) the valve/servo will modulate during a note.
  // Maps to the same controller calls used during play; passive modes ignore it.
  virtual void setDemandPercent(uint8_t percent) = 0;

  // Return the source to a safe idle/off state (pump/fan stopped).
  virtual void stopSafe() = 0;

  // Last error observed; CAL_AIR_OK when the supply is healthy.
  virtual CalAirSupplyError getError() const = 0;
};

#endif // ICALIBRATION_AIR_SUPPLY_H
