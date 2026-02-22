/***********************************************************************************************
 * PressureController - Gestion pompe + reservoir + capteur distance VL53L0X/VL6180X
 *
 * Mesure la hauteur du ballon/reservoir via capteur ToF I2C, pilote une pompe
 * en PWM via boucle PID pour maintenir une pression cible.
 *
 * Modes supportes :
 * - Pompe directe (sans capteur) : PWM proportionnel a la demande
 * - Pompe + reservoir : boucle PID sur la distance capteur
 *
 * Bus I2C partage avec PCA9685 (adresses differentes).
 ***********************************************************************************************/
#ifndef PRESSURE_CONTROLLER_H
#define PRESSURE_CONTROLLER_H

#include <Arduino.h>
#include "settings.h"

class PressureController {
public:
  PressureController();

  // Initialise le capteur et la pompe. Retourne true si capteur detecte
  bool begin();

  // Appeler dans loop() - lit le capteur, execute le PID, pilote la pompe
  void update();

  // Definir la pression cible (0-100%). 0=pompe arretee
  void setTargetPercent(uint8_t percent);

  // Arreter la pompe immediatement
  void stop();

  // Accesseurs pour l'UI
  uint16_t getDistanceMm() const { return _distanceMm; }
  uint8_t  getFillPercent() const { return _fillPercent; }
  uint8_t  getPumpPwm() const { return _currentPumpPwm; }
  bool     isPumpRunning() const { return _currentPumpPwm > 0; }
  bool     isSensorDetected() const { return _sensorDetected; }
  uint8_t  getTargetPercent() const { return _targetPercent; }

private:
  bool _sensorDetected;
  uint8_t _sensorType;        // 0=VL53L0X, 1=VL6180X

  // Etat capteur
  uint16_t _distanceMm;       // Derniere mesure distance (mm)
  uint8_t _fillPercent;        // Pourcentage remplissage (0-100)

  // Etat pompe
  uint8_t _targetPercent;      // Cible demandee (0-100)
  uint8_t _currentPumpPwm;    // PWM actuel envoye a la pompe

  // PID state
  float _pidIntegral;
  float _pidLastError;
  unsigned long _lastPidTime;
  unsigned long _lastReadTime;

  // Lecture capteur selon type
  uint16_t readSensor();

  // Appliquer PWM pompe
  void setPumpPwm(uint8_t pwm);
};

#endif
