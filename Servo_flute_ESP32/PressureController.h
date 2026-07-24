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

  // Global pump mute (keyboard toggle). While disabled the pump is forced off in
  // update() regardless of demand; enabling resumes normal control.
  void setEnabled(bool enabled);
  bool isEnabled() const { return _enabled; }

  // Manual single-pump test: drive ONLY pump `index` at `percent` (bypassing the
  // global cascade/PID controller) until stopped. Lets the per-pump test button
  // exercise one pump instead of commanding them all.
  void testSinglePump(uint8_t index, uint8_t percent);
  void stopSinglePumpTest();

  // Accesseurs pour l'UI
  uint16_t getDistanceMm() const { return _distanceMm; }
  uint8_t  getFillPercent() const { return _fillPercent; }
  uint8_t  getPumpPwm() const { return _currentPumpPwm; }
  bool     isPumpRunning() const { return _currentPumpPwm > 0; }
  bool     isSensorDetected() const { return _sensorDetected; }
  uint8_t  getTargetPercent() const { return _targetPercent; }
  bool     isEndstopActive() const { return _endstopActive; }
  uint16_t getHallValue() const { return _hallValue; }
  uint8_t  getActivePumpCount() const { return _activePumpCount; }
  bool     isBangbangOn() const { return _bangbangPumpOn; }

private:
  bool _sensorDetected;
  uint8_t _sensorType;        // 0-4 (SENSOR_TYPE_*)

  // Etat capteur
  uint16_t _distanceMm;       // Derniere mesure distance (mm) - pour ToF
  uint16_t _hallValue;         // Derniere lecture analogique Hall
  bool _endstopActive;         // Etat endstop (meca/optique)
  uint8_t _fillPercent;        // Pourcentage remplissage (0-100)

  // Etat pompe
  uint8_t _targetPercent;      // Cible demandee (0-100)
  uint8_t _currentPumpPwm;    // PWM global (sortie PID, 0-255)
  bool _enabled;               // Mute global: false => pompe forcee a l'arret
  int8_t _testPumpIndex;       // >=0: test manuel d'une seule pompe (cet index)
  uint8_t _testPumpPercent;    // Consigne du test mono-pompe (0-100)

  // Cascade multi-pompes
  bool _pumpActive[MAX_PUMPS];           // Etat actif par pompe
  unsigned long _pumpActivateTime[MAX_PUMPS]; // Timestamp activation par pompe (stagger)
  uint8_t _activePumpCount;              // Nombre de pompes actives

  // Bang-bang hysteresis (moteurs On/Off + capteur continu)
  bool _bangbangPumpOn;                  // Etat courant bang-bang

  // PID state
  float _pidIntegral;
  float _pidLastError;
  unsigned long _lastPidTime;
  unsigned long _lastReadTime;

  // Lecture capteur selon type
  uint16_t readSensor();

  // Appliquer PWM global avec logique cascade multi-pompes
  void setPumpPwm(uint8_t pwm);

  // Ecrire le signal physique sur une pompe individuelle
  void writePumpHw(uint8_t index, uint8_t pwm);
};

#endif
