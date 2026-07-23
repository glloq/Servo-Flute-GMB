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

  // Ecriture PWM multi-PCA9685 : route vers la bonne carte (canal 0-15 = carte 0, 16-31 = carte 1)
  void setPWM(uint8_t channel, uint16_t on, uint16_t off);

  // Calibration : acces direct aux controleurs
  FingerController& getFingerCtrl() { return _fingerCtrl; }
  AirflowController& getAirflowCtrl() { return _airflowCtrl; }
  PressureController& getPressureCtrl() { return _pressureCtrl; }
  FanController& getFanCtrl() { return _fanCtrl; }
  HardwareInitStatus hardwareInitStatus() const { return _hardwareInitStatus; }
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
  NoteSequencer _sequencer;

  unsigned long _lastActivityTime;
  bool _servosPowered;

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
