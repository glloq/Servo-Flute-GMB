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
//
// POURQUOI LE PARAMETRE EST SIGNE
// -------------------------------
// Il etait `uint16_t`. Le garde-fou `if (angle < SERVO_MIN_ANGLE)` etait donc du
// code mort (SERVO_MIN_ANGLE vaut 0, aucun non-signe n'est plus petit), et un
// angle calcule NEGATIF - un offset de vibrato soustrait sous le minimum, une
// soustraction d'angle de doigt qui passe sous zero - arrivait ici enroule en
// tres grand entier, donc sature vers SERVO_MAX_ANGLE : la commande la plus
// energique possible, course maximale, exactement l'inverse de ce que le calcul
// demandait. Avec un parametre signe, ce meme calcul arrive negatif et redescend
// vers le bas de la course, comme la ligne l'a toujours voulu.
//
// POURQUOI UN ARGUMENT ABERRANT ECHOUE VERS LE BAS
// ------------------------------------------------
// Un appelant qui a deja perdu le signe (conversion non signee faite chez lui,
// valeur corrompue) presente une valeur immense. La saturer vers SERVO_MAX_ANGLE
// revient a commander la course maximale sur la foi d'une valeur dont on sait
// qu'elle est fausse. Au-dela de SERVO_ANGLE_SANE_LIMIT (deux fois la course
// mecanique) la valeur ne peut plus provenir d'un calcul d'angle : elle est
// refusee et l'angle retombe sur SERVO_MIN_ANGLE.
//
// SERVO_MIN_ANGLE est le repli choisi parce qu'il est le seul point commun aux
// trois usages (souffle, valve, doigt) qui ne soit PAS une course a fond : cote
// souffle c'est le moins d'air, cote doigt le bord de reference de la course.
// Ce n'est pas "la position sure" de chaque actionneur - elle depend de la
// configuration (closedAngle, direction, valveServoCloseAngle) et seul le
// controleur la connait (setAirflowToRest(), closeAllFingers()). Un convertisseur
// sans etat ne peut que garantir de ne pas transformer une valeur fausse en
// commande extreme.
//
// Un depassement MODESTE (jusqu'a SERVO_ANGLE_SANE_LIMIT) reste sature vers
// SERVO_MAX_ANGLE : il vient d'une arithmetique d'angle qui a legerement deborde,
// et tous les appelants bornent deja avant d'appeler.
inline uint16_t servoAngleToPWM(int32_t angle) {
  if (angle > (int32_t)SERVO_ANGLE_SANE_LIMIT) angle = SERVO_MIN_ANGLE;
  if (angle < (int32_t)SERVO_MIN_ANGLE) angle = SERVO_MIN_ANGLE;
  if (angle > (int32_t)SERVO_MAX_ANGLE) angle = SERVO_MAX_ANGLE;

  uint16_t pulse = map(angle, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE,
                       SERVO_PULSE_MIN, SERVO_PULSE_MAX);

  float pulseDuration = (float)pulse / 1000000.0;
  float pwmValue = pulseDuration * SERVO_FREQUENCY * 4096.0;

  return (uint16_t)(pwmValue + 0.5f);
}

#endif
