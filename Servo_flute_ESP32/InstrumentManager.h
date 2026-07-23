#ifndef INSTRUMENT_MANAGER_H
#define INSTRUMENT_MANAGER_H

#include <Arduino.h>
#include <Wire.h>   // must precede Adafruit_PWMServoDriver.h (declares TwoWire)
#include <Adafruit_PWMServoDriver.h>
#include "EventQueue.h"
#include "FingerController.h"
#include "AirflowController.h"
#include "PressureController.h"
#include "FanController.h"
#include "NoteSequencer.h"
#include "CalibrationAirSupply.h"
#include "settings.h"
#include "ConfigStorage.h"

enum HardwareInitStatus {
  HW_INIT_OK,
  HW_PCA0_MISSING,
  HW_PCA1_MISSING,
  HW_I2C_ERROR,
  HW_CONFIG_INVALID
};

struct ConfigApplyResult {
  bool applied;
  bool restartRequired;
  String reinitialized;
  String warnings;
};

class InstrumentManager {
public:
  InstrumentManager();

  void begin();
  bool beginSafe();
  void initializeSafeOutputs();
  void update();

  // Interface MIDI (appelee par BleMidiHandler ou WifiMidiHandler)
  void noteOn(byte midiNote, byte velocity);
  void noteOff(byte midiNote);

  bool isNotePlayable(byte midiNote) const;
  NoteSequencer& getSequencer();

  // Gere les Control Change MIDI
  void handleControlChange(byte ccNumber, byte ccValue);

  // Accesseurs CC
  byte getCCVolume() const { return _ccVolume; }
  byte getCCExpression() const { return _ccExpression; }
  byte getCCModulation() const { return _ccModulation; }
  byte getCCBreath() const { return _ccBreath; }
  byte getCCBrightness() const { return _ccBrightness; }

  void allSoundOff();
  void resetAllControllers();
  void powerOnServos();
  void ensureServosPowered();
  void registerActuatorActivity();
  // While true, the idle power-down is inhibited and the servos are kept powered
  // (used by the auto-calibrator / range finder, which drive actuators and read
  // audio outside the MIDI sequencer that managePower() watches).
  void setActuatorSessionActive(bool active);
  bool isActuatorSessionActive() const { return _actuatorSessionActive; }

  // Ecriture PWM multi-PCA9685 : route vers la bonne carte (canal 0-15 = carte 0, 16-31 = carte 1)
  void setPWM(uint8_t channel, uint16_t on, uint16_t off);

  // Calibration : acces direct aux controleurs
  FingerController& getFingerCtrl() { return _fingerCtrl; }
  AirflowController& getAirflowCtrl() { return _airflowCtrl; }
  PressureController& getPressureCtrl() { return _pressureCtrl; }
  FanController& getFanCtrl() { return _fanCtrl; }
  // Air-source bridge for the auto-calibrator (drives pump/fan/reservoir per air mode).
  ICalibrationAirSupply& getCalibrationAirSupply() { return _calAirSupply; }
  HardwareInitStatus hardwareInitStatus() const { return _hardwareInitStatus; }
  // True only after a fully successful safe init. When false (PCA missing / config
  // invalid), all actuator paths (noteOn/noteOff/CC/setPWM/power/update) are no-ops
  // so a failed board can never be driven.
  bool isHardwareReady() const { return _hardwareInitStatus == HW_INIT_OK; }
  bool isSecondBoardEnabled() const { return _secondBoardEnabled; }
  ConfigApplyResult applyRuntimeConfig(const RuntimeConfig& oldConfig, const RuntimeConfig& newConfig);

private:
  Adafruit_PWMServoDriver _pwm0;    // Carte 1 (0x40) — toujours active
  Adafruit_PWMServoDriver _pwm1;    // Carte 2 (0x41) — active si canaux >= 16
  bool _secondBoardEnabled;
  HardwareInitStatus _hardwareInitStatus;
  EventQueue _eventQueue;
  FingerController _fingerCtrl;
  AirflowController _airflowCtrl;
  PressureController _pressureCtrl;
  FanController _fanCtrl;
  CalibrationAirSupply _calAirSupply;
  NoteSequencer _sequencer;

  unsigned long _lastActivityTime;
  bool _servosPowered;
  bool _actuatorSessionActive;   // calibration/range-finder holds power (see managePower)
  bool _initializingHardware;    // during beginSafe(): write safe PWM but never enable OE

  // Valeurs Control Change MIDI
  byte _ccVolume;
  byte _ccExpression;
  byte _ccModulation;
  byte _ccBreath;
  byte _ccBrightness;

  // Rate limiting
  unsigned long _lastCCTime;
  uint16_t _ccCount;
  unsigned long _ccWindowStart;
  uint16_t _cc2Count;
  unsigned long _cc2WindowStart;

  void managePower();
  void powerOffServos();
  bool detectPca(uint8_t address) const;
  bool requiresSecondPca() const;

  // Fan idle: track sequencer state transitions
  NoteState _prevSequencerState;
};

#endif
