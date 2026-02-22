#ifndef FINGER_CONTROLLER_H
#define FINGER_CONTROLLER_H

#include <Arduino.h>
#include <Adafruit_PWMServoDriver.h>
#include "settings.h"

class FingerController {
public:
  FingerController(Adafruit_PWMServoDriver& pwm);

  void begin();

  // Applique un pattern de doigtes (0=ferme, 1=ouvert, 2=demi-ouvert)
  // Taille du pattern = cfg.numFingers
  void setFingerPattern(const uint8_t pattern[MAX_FINGER_SERVOS]);

  // Applique un pattern pour une note MIDI specifique
  void setFingerPatternForNote(byte midiNote);

  void closeAllFingers();
  void openAllFingers();

  // Calibration : positionner un doigt a un angle arbitraire
  void testFingerAngle(int fingerIndex, uint16_t angle);

private:
  Adafruit_PWMServoDriver& _pwm;

  uint16_t calculateServoAngle(int fingerIndex, uint8_t openState);
  void setServoAngle(int fingerIndex, uint16_t angle);
  uint16_t angleToPWM(uint16_t angle);
};

#endif
