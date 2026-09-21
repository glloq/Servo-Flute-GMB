/***********************************************************************************************
 * TofSensor - Capteurs de distance I2C du reservoir (VL53L0X / VL6180X)
 *
 * POURQUOI CE MODULE
 * ------------------
 * La branche VL53L0X ne faisait AUCUNE initialisation : elle se contentait de
 * verifier qu'un peripherique acquittait a l'adresse 0x29, puis ecrivait 0x01
 * dans SYSRANGE_START et lisait le registre de resultat. Un VL53L0X sortant de
 * reset n'est pas configure (SPAD de reference non selectionnes, tuning non
 * charge, calibrations VHV/phase non faites) : les valeurs lues n'ont aucune
 * signification metrique. Pire, n'importe quel composant repondant a 0x29 etait
 * annonce comme "capteur detecte et initialise".
 *
 * Ce module implemente la sequence d'initialisation complete du VL53L0X
 * (verification du Model ID, data init, information SPAD de reference, tuning
 * par defaut, configuration d'interruption GPIO, calibrations de reference) et
 * separe explicitement les etats :
 *
 *   TOF_ABSENT       le peripherique n'acquitte pas sur le bus
 *   TOF_UNSUPPORTED  quelque chose repond a l'adresse mais ce n'est pas le capteur
 *   TOF_INIT_FAILED  le capteur a repondu mais son initialisation a echoue
 *   TOF_READY        capteur initialise, mesures exploitables
 *   TOF_FAULT        capteur perdu en cours de route (timeouts repetes)
 *
 * La lecture reste NON BLOQUANTE : startMeasurement() lance un single-shot,
 * pollMeasurement() interroge l'etat une fois par appel. Aucun delay() en
 * fonctionnement (seule l'initialisation, au boot, attend avec des timeouts).
 ***********************************************************************************************/
#ifndef TOF_SENSOR_H
#define TOF_SENSOR_H

#include <Arduino.h>

#define TOF_I2C_ADDRESS 0x29

enum TofSensorState {
  TOF_ABSENT,
  TOF_UNSUPPORTED,
  TOF_INIT_FAILED,
  TOF_READY,
  TOF_FAULT
};

enum TofSensorModel {
  TOF_MODEL_VL53L0X,
  TOF_MODEL_VL6180X
};

class TofSensor {
public:
  TofSensor();

  // Sonde le bus puis initialise le capteur. Retourne true uniquement si le
  // capteur est reellement INITIALISE (pas seulement present sur le bus).
  bool begin(TofSensorModel model);

  // Demarre une mesure single-shot. Retourne false si le capteur n'est pas pret.
  bool startMeasurement();
  // Interroge une mesure en cours. Retourne true quand une distance fraiche est
  // disponible dans distanceMm(). N'attend jamais.
  bool pollMeasurement();
  // Abandon de la mesure en cours (timeout cote appelant).
  void abortMeasurement();

  // Signale un timeout de mesure ; au-dela de `maxConsecutive` le capteur passe
  // en TOF_FAULT et n'est plus considere comme utilisable.
  void reportTimeout(uint16_t maxConsecutive);

  TofSensorState state() const { return _state; }
  bool isPresentOnBus() const { return _presentOnBus; }
  bool isInitialized() const { return _state == TOF_READY; }
  bool isRanging() const { return _ranging; }
  uint16_t distanceMm() const { return _distanceMm; }
  // Statut de plage renvoye par le VL53L0X (11 = mesure valide). 0xFF si inconnu.
  uint8_t lastRangeStatus() const { return _lastRangeStatus; }
  const char* stateName() const;
  static const char* stateName(TofSensorState state);

private:
  TofSensorModel _model;
  TofSensorState _state;
  bool _presentOnBus;
  bool _ranging;
  uint16_t _distanceMm;
  uint8_t _lastRangeStatus;
  uint8_t _stopVariable;
  uint16_t _timeoutCount;

  bool initVl53l0x();
  bool initVl6180x();
  bool getSpadInfo(uint8_t& count, bool& isAperture);
  bool performSingleRefCalibration(uint8_t vhvInitByte);
};

#endif
