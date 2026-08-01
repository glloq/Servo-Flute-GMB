/***********************************************************************************************
 * ServoMath - Conversion angle -> compte PWM (12 bits) pour PCA9685
 *
 * La formule etait dupliquee a l'identique dans FingerController et
 * AirflowController. Centralisee ici (header-only, sans etat) pour une source
 * unique et une couverture de test directe.
 ***********************************************************************************************/
#ifndef SERVO_MATH_H
#define SERVO_MATH_H

#include <Arduino.h>
#include "settings.h"

// Convertit un angle (degres) en compte PWM 12 bits attendu par le PCA9685,
// en bornant l'angle a la plage servo et via la largeur d'impulsion configuree.
inline uint16_t servoAngleToPWM(uint16_t angle) {
  if (angle < SERVO_MIN_ANGLE) angle = SERVO_MIN_ANGLE;
  if (angle > SERVO_MAX_ANGLE) angle = SERVO_MAX_ANGLE;

  uint16_t pulse = map(angle, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE,
                       SERVO_PULSE_MIN, SERVO_PULSE_MAX);

  float pulseDuration = (float)pulse / 1000000.0;
  float pwmValue = pulseDuration * SERVO_FREQUENCY * 4096.0;

  return (uint16_t)(pwmValue + 0.5f);
}

#endif
