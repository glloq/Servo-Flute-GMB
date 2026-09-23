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
#include "TofSensor.h"

// Le stub de test hote ne connait pas INPUT_PULLDOWN (le core ESP32 le definit a
// 0x09). Repli distinct des autres modes du stub, uniquement pour que le fichier
// compile hors ESP32 ; sur cible c'est la valeur du core qui est utilisee.
#ifndef INPUT_PULLDOWN
#define INPUT_PULLDOWN 3
#endif

// Ce que l'on SAIT du capteur de reservoir, par opposition a ce que l'on suppose.
// Un capteur analogique (Hall) ou un contact sec ne s'auto-identifient pas : rien
// sur une entree ADC ou une entree logique ne dit "KY-024" ou "fin de course".
// Confondre "broche configuree" et "capteur detecte" est exactement ce qui
// laissait la pompe reguler sur un capteur inexistant.
enum ReservoirSensorPresence {
  SENSOR_PRESENCE_ABSENT,    // preuve positive d'absence (ToF : rien sur le bus, mauvais Model ID, init KO)
  SENSOR_PRESENCE_PRESUMED,  // broche configuree, identification IMPOSSIBLE (Hall, fins de course)
  SENSOR_PRESENCE_DETECTED   // identifie ET initialise (ToF uniquement)
};

class PressureController {
public:
  PressureController();

  // Configure la pompe et le capteur. Retourne true si la chaine de mesure est
  // exploitable : pour un ToF cela veut dire IDENTIFIE et initialise ; pour un
  // Hall ou une fin de course cela veut seulement dire "broche configuree et
  // rien ne prouve l'absence" (voir sensorPresence()). Dans tous les cas la
  // pompe ne tournera qu'avec une mesure acceptee et fraiche : la valeur de
  // retour pilote l'autostart, jamais la securite.
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
  // ATTENTION : 0 % ne veut pas dire "vide", il veut aussi dire "jamais mesure".
  // Qualifier avec isMeasurementValid() / isMeasurementStale() avant d'afficher.
  uint8_t  getFillPercent() const { return _fillPercent; }
  uint8_t  getPumpPwm() const { return _currentPumpPwm; }
  bool     isPumpRunning() const { return _currentPumpPwm > 0; }
  // ATTENTION AU NOM (conserve pour ses appelants) : "detected" veut dire
  // "chaine de mesure utilisable pour reguler", PAS "capteur identifie".
  // L'identification positive n'existe que pour les ToF (Model ID + init).
  // Pour le Hall et les fins de course l'etat honnete est PRESUMED : c'est
  // sensorPresence() / sensorStateName() qui disent la verite, pas ce booleen.
  bool     isSensorDetected() const { return _sensorDetected; }
  // Ce que l'on sait reellement du capteur (detecte / presume / absent).
  ReservoirSensorPresence sensorPresence() const { return _sensorPresence; }
  // Verrou d'emballement : la pompe a tourne PUMP_MAX_RUN_MS sans que la mesure
  // bouge. Capteur mort, fuite, valve fermee : dans tous les cas la pompe ne
  // sert a rien et on ne peut plus la laisser tourner.
  bool     isPumpRunawayLatched() const { return _runawayLatched; }
  // Rearmement explicite apres intervention (a cabler sur l'UI). Le verrou se
  // rearme aussi tout seul, mais UNIQUEMENT sur une preuve que le capteur vit
  // encore (la mesure bouge pendant que la pompe est a l'arret).
  void     clearPumpRunawayFault();
  // Sens du rappel interne d'une entree fin de course. Le rappel doit produire
  // le niveau INACTIF declare : c'est le seul choix qui fasse fonctionner un
  // contact sec, qui n'impose qu'un seul des deux niveaux. Expose (et donc
  // verrouillable par un test) parce qu'il decide de ce que lit une entree
  // debranchee. Ce n'est PAS une securite : voir PUMP_MAX_RUN_MS.
  static uint8_t endstopPinModeFor(bool activeHigh) {
    return activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP;
  }
  // Etats distincts exposes par les diagnostics (§9 de l'audit) :
  //   present sur I2C / initialise / mesure valide / mesure perimee / en erreur.
  bool     isSensorPresentOnBus() const { return _tof.isPresentOnBus(); }
  bool     isSensorInitialized() const { return _tof.isInitialized(); }
  // Nom d'etat HONNETE par famille. Il rendait l'etat du pilote ToF quelle que
  // soit la famille : un Hall en pleine regulation s'affichait "absent".
  const char* sensorStateName() const;
  TofSensorState sensorState() const { return _tof.state(); }
  bool     isMeasurementValid() const { return _measurementValid; }
  bool     isMeasurementStale() const;
  bool     usesTofSensor() const;
  uint8_t  getTargetPercent() const { return _targetPercent; }
  bool     isEndstopActive() const { return _endstopActive; }
  uint16_t getHallValue() const { return _hallValue; }
  uint8_t  getActivePumpCount() const { return _activePumpCount; }
  bool     isBangbangOn() const { return _bangbangPumpOn; }

private:
  bool _sensorDetected;
  ReservoirSensorPresence _sensorPresence;  // ce qui est SU, pas ce qui est suppose
  uint8_t _sensorType;        // 0-4 (SENSOR_TYPE_*)

  TofSensor _tof;             // pilote ToF (VL53L0X / VL6180X), etats separes

  // Etat capteur
  uint16_t _distanceMm;       // Derniere mesure distance (mm) - pour ToF
  uint16_t _hallValue;         // Derniere lecture analogique Hall
  bool _endstopActive;         // Etat endstop (meca/optique)
  uint8_t _fillPercent;        // Pourcentage remplissage (0-100)

  // Non-blocking ToF state machine (single-shot: start -> poll once per update).
  unsigned long _tofRangeStartTime; // Debut de la mesure en cours (pour le timeout)
  bool _measurementValid;           // La derniere tentative de mesure a ete ACCEPTEE (toutes familles)
  unsigned long _lastValidReadTime; // Horodatage de la derniere mesure acceptee (toutes familles)
  bool _everMeasured;               // Au moins une mesure acceptee depuis begin()
  uint16_t _tofErrorCount;          // Compteur d'erreurs/timeouts consecutifs

  // Etat pompe
  uint8_t _targetPercent;      // Cible demandee (0-100)
  uint8_t _currentPumpPwm;    // PWM global (sortie PID, 0-255)
  bool _enabled;               // Mute global: false => pompe forcee a l'arret
  int8_t _testPumpIndex;       // >=0: test manuel d'une seule pompe (cet index)
  uint8_t _testPumpPercent;    // Consigne du test mono-pompe (0-100)
  unsigned long _testPumpStart; // Debut du test mono-pompe (borne par PUMP_TEST_MAX_MS)

  // Chien de garde de marche : une pompe qui tourne sans que la mesure bouge ne
  // sert a rien et finit par casser quelque chose.
  bool _runawayLatched;             // Verrou d'emballement arme
  unsigned long _pumpRunSince;      // Debut de la marche continue en cours (0 = arretee)
  uint8_t _pumpRunRefFill;          // Remplissage au debut de la fenetre (reference a cliquet)
  uint8_t _runawayFill;             // Remplissage au moment du verrouillage (preuve de vie)

  // Cascade multi-pompes
  bool _pumpActive[MAX_PUMPS];           // Etat actif par pompe
  unsigned long _pumpActivateTime[MAX_PUMPS]; // Timestamp activation par pompe (stagger)
  uint8_t _activePumpCount;              // Nombre de pompes actives

  // Bang-bang hysteresis (moteurs On/Off + capteur continu)
  bool _bangbangPumpOn;                  // Etat courant bang-bang

  // PID state
  float _pidIntegral;
  unsigned long _lastPidTime;
  unsigned long _lastReadTime;

  // Lecture ToF non bloquante: demarre une mesure single-shot puis interroge son
  // etat une fois par appel (pas de delay). Retourne true une seule fois quand une
  // nouvelle distance valide est prete (dans _distanceMm).
  bool serviceTofMeasurement();

  // Acquisition par famille de capteur. Chacune n'horodate _lastValidReadTime
  // que si la mesure est ACCEPTEE : c'est ce qui fait marcher la peremption
  // commune pour toutes les familles, pas seulement pour le ToF.
  void serviceHallMeasurement(unsigned long now);
  // Unique point d'acceptation d'une lecture Hall (sondage de begin() et lecture
  // periodique). Rend false si la lecture n'est pas une mesure.
  bool acceptHallReading(uint16_t raw, unsigned long now);
  void serviceEndstopMeasurement(unsigned long now);
  // Une lecture ADC brute est-elle une mesure, ou un rail / une echelle absente ?
  bool isHallReadingPlausible(uint16_t raw) const;

  // Chien de garde de marche (toutes familles) + rearmement sur preuve de vie.
  void serviceRunawayWatchdog(unsigned long now);
  // La mesure a-t-elle bouge d'au moins PUMP_PROGRESS_MIN_PERCENT depuis `ref`,
  // dans le sens ou la pompe est censee la pousser ?
  bool fillProgressedFrom(uint8_t ref) const;

  // Coupure de securite : la pompe s'arrete ET l'etat du regulateur est jete
  // (un integrateur conserve se dechargerait d'un coup au retour de la mesure).
  void cutPumpForSafety();

  // Appliquer PWM global avec logique cascade multi-pompes
  void setPumpPwm(uint8_t pwm);

  // Ecrire le signal physique sur une pompe individuelle
  void writePumpHw(uint8_t index, uint8_t pwm);
};

#endif
