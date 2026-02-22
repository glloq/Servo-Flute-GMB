#ifndef INSTRUMENT_MANAGER_H
#define INSTRUMENT_MANAGER_H

#include <Arduino.h>
#include <Adafruit_PWMServoDriver.h>
#include "EventQueue.h"
#include "FingerController.h"
#include "AirflowController.h"
#include "PressureController.h"
#include "NoteSequencer.h"
#include "settings.h"

class InstrumentManager {
public:
  InstrumentManager();

  void begin();
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

  // Calibration : acces direct aux controleurs
  FingerController& getFingerCtrl() { return _fingerCtrl; }
  AirflowController& getAirflowCtrl() { return _airflowCtrl; }
  PressureController& getPressureCtrl() { return _pressureCtrl; }

private:
  Adafruit_PWMServoDriver _pwm;
  EventQueue _eventQueue;
  FingerController _fingerCtrl;
  AirflowController _airflowCtrl;
  PressureController _pressureCtrl;
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
  void powerOnServos();
  void powerOffServos();
};

#endif
