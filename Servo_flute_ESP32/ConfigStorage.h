/***********************************************************************************************
 * ConfigStorage - Configuration persistante sur LittleFS (JSON)
 *
 * RuntimeConfig : structure contenant tous les parametres modifiables a chaud.
 * Supporte un nombre variable de doigts (1-15) et de notes (1-32).
 * Initialisee avec les valeurs par defaut de settings.h, puis surchargee par
 * le fichier /config.json sur LittleFS.
 *
 * Permet de modifier les parametres via la page web sans recompiler.
 * Un appel a save() persiste les changements. load() les restaure au boot.
 *
 * Dependances : ArduinoJson, LittleFS
 ***********************************************************************************************/
#ifndef CONFIG_STORAGE_H
#define CONFIG_STORAGE_H

#include <Arduino.h>
#include "settings.h"

#define CONFIG_FILE_PATH "/config.json"

/*******************************************************************************
 * Structures pour doigts et notes (runtime, taille MAX)
 ******************************************************************************/

struct FingerConfig {
  uint8_t pcaChannel;      // Canal PCA9685 (0-15)
  uint16_t closedAngle;    // Angle position fermee (0-180)
  int8_t direction;        // +1 = horaire, -1 = anti-horaire
  bool isThumbHole;        // true = trou arriere (dessous de la flute)
};

struct NoteConfig {
  uint8_t midiNote;                          // Numero MIDI
  uint8_t fingerPattern[MAX_FINGER_SERVOS];  // Doigtes (0=ferme, 1=ouvert, 2=demi-ouvert)
  uint8_t airflowMinPercent;                 // % min servo flow (0-100)
  uint8_t airflowMaxPercent;                 // % max servo flow (0-100)
};

/*******************************************************************************
 * RuntimeConfig - Configuration complete modifiable a chaud
 ******************************************************************************/

struct RuntimeConfig {
  // --- Instrument (modulaire) ---
  uint8_t numFingers;                        // 1-15 (nombre effectif de doigts)
  uint8_t numNotes;                          // 1-32 (nombre effectif de notes)
  uint8_t airflowPcaChannel;                // Canal PCA9685 pour servo airflow
  uint8_t fingerAngleOpen;                   // Amplitude ouverture commune (degres)
  uint8_t halfHolePercent;                   // Pourcentage ouverture demi-trou (10-90, defaut 50)
  char embouchure[5];                        // Type: trav, bec, naf, end, oca
  FingerConfig fingers[MAX_FINGER_SERVOS];   // Config doigts (seuls [0..numFingers-1] actifs)
  NoteConfig notes[MAX_NOTES];               // Config notes (seuls [0..numNotes-1] actifs)

  // --- MIDI ---
  uint8_t midiChannel;

  // --- Timing ---
  uint16_t servoToSolenoidDelayMs;
  uint16_t minNoteIntervalForValveCloseMs;
  uint16_t minNoteDurationMs;

  // --- Airflow servo ---
  uint16_t servoAirflowOff;
  uint16_t servoAirflowMin;
  uint16_t servoAirflowMax;

  // --- Vibrato ---
  float vibratoFrequencyHz;
  float vibratoMaxAmplitudeDeg;

  // --- CC defaults ---
  uint8_t ccVolumeDefault;
  uint8_t ccExpressionDefault;
  uint8_t ccModulationDefault;
  uint8_t ccBreathDefault;
  uint8_t ccBrightnessDefault;

  // --- CC2 Breath Controller ---
  bool cc2Enabled;
  uint8_t cc2SilenceThreshold;
  float cc2ResponseCurve;
  uint16_t cc2TimeoutMs;

  // --- Solenoide ---
  uint8_t solenoidPwmActivation;
  uint8_t solenoidPwmHolding;
  uint16_t solenoidActivationTimeMs;

  // --- Expression airflow (comportement noteOn) ---
  uint8_t airAttackMode;                     // 0=stable, 1=accent (fort→cible), 2=crescendo (faible→cible)
  uint8_t airAttackOffset;                   // 0-50 : ecart % par rapport a la cible
  uint16_t airAttackMs;                      // 10-1000 : duree transition attaque (ms)
  uint8_t airVelocityResponse;               // 0-100 : influence de la velocite sur le souffle

  // --- WiFi STA ---
  char wifiSsid[33];
  char wifiPassword[65];

  // --- Device ---
  char deviceName[32];

  // --- Power ---
  uint16_t timeUnpower;

  // --- Air delivery system ---
  uint8_t airMode;                   // 0=classique, 1=servo-valve, 2=pompe, 3=pompe+reservoir
  bool valveUseServo;                // true=servo PCA, false=solenoide GPIO
  uint8_t valveServoPcaChannel;      // Canal PCA9685 si valve=servo
  bool pumpEnabled;                  // Pompe active
  uint8_t pumpPin;                   // GPIO PWM pompe
  uint8_t pumpMinPwm;               // PWM min (seuil demarrage moteur)
  uint8_t pumpMaxPwm;               // PWM max
  bool reservoirEnabled;             // Reservoir (ballon) avec capteur
  uint8_t sensorType;               // 0=VL53L0X, 1=VL6180X
  uint16_t sensorTargetMm;          // Hauteur cible (mm)
  uint16_t sensorMinMm;             // Hauteur min (vide)
  uint16_t sensorMaxMm;             // Hauteur max (plein)
  uint8_t pidKp;                     // Gain proportionnel PID (x10)
  uint8_t pidKi;                     // Gain integral PID (x10)
  bool showAirSystem;                // Afficher schema pneumatique dans l'UI

  // --- MIDI Storage ---
  uint16_t midiStorageLimitKb;       // Limite stockage total MIDI en Ko (defaut 500)

  // --- UI ---
  bool hideCalibration;              // Cacher l'onglet Calibration
  uint8_t solenoidPin;               // GPIO solenoide (configurable)
  char instrumentColor[8];           // Couleur hex instrument "#RRGGBB"
  uint8_t kbdMode;                   // 0=flute (defaut), 1=piano
};

// Config globale accessible depuis tout le projet
extern RuntimeConfig cfg;

/*******************************************************************************
 * Fonctions utilitaires runtime (remplacent les anciennes de settings.h)
 ******************************************************************************/

// Cherche une note par numero MIDI dans cfg.notes[]
inline const NoteConfig* getNoteByMidi(uint8_t midiNote) {
  for (int i = 0; i < cfg.numNotes; i++) {
    if (cfg.notes[i].midiNote == midiNote) {
      return &cfg.notes[i];
    }
  }
  return nullptr;
}

// Retourne l'index d'une note dans cfg.notes[], ou -1
inline int getNoteIndex(uint8_t midiNote) {
  for (int i = 0; i < cfg.numNotes; i++) {
    if (cfg.notes[i].midiNote == midiNote) {
      return i;
    }
  }
  return -1;
}

/*******************************************************************************
 * ConfigStorage - Classe statique de gestion persistance
 ******************************************************************************/

class ConfigStorage {
public:
  // Initialise cfg avec les valeurs par defaut de settings.h
  static void initDefaults();

  // Charge la config depuis LittleFS (surcharge les defauts)
  static bool load();

  // Sauvegarde la config actuelle sur LittleFS
  static bool save();

  // Remet cfg aux valeurs par defaut et sauvegarde
  static void resetToDefaults();

  // Reset usine: remet cfg aux defauts et supprime le fichier config
  static void factoryReset();

  // Verifie si c'est le premier demarrage (pas de config sauvegardee)
  static bool isFirstBoot();
};

#endif
