/***********************************************************************************************
 * ConfigStorage - Configuration persistante sur LittleFS (JSON)
 *
 * RuntimeConfig : structure contenant tous les parametres modifiables a chaud.
 * Supporte un nombre variable de doigts (1-31) et de notes (1-128).
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
  uint8_t pcaChannel;      // Canal PCA9685 (0-31, multi-PCA)
  uint16_t closedAngle;    // Angle position fermee (0-180)
  int8_t direction;        // +1 = horaire, -1 = anti-horaire
  bool isThumbHole;        // true = trou arriere (dessous de la flute)
  uint8_t halfPercent;     // 0 = utiliser global halfHolePercent, 1-100 = override per-doigt
};

struct NoteConfig {
  uint8_t midiNote;                          // Numero MIDI
  uint8_t fingerPattern[MAX_FINGER_SERVOS];  // Doigtes (0=ferme, 1=ouvert, 2=demi-ouvert)
  uint8_t airflowMinPercent;                 // % min servo flow (0-100)
  uint8_t airflowMaxPercent;                 // % max servo flow (0-100)
  uint8_t airflowNominalPercent;             // % recommande servo flow (min <= nominal <= max)
  uint8_t anglePercent;                      // Angle jet d'air % (0-100), trav uniquement
};

/*******************************************************************************
 * RuntimeConfig - Configuration complete modifiable a chaud
 ******************************************************************************/

struct RuntimeConfig {
  // --- Instrument (modulaire) ---
  uint8_t numFingers;                        // 1-31 (nombre effectif de doigts)
  uint8_t numNotes;                          // 1-128 (nombre effectif de notes)
  uint8_t airflowPcaChannel;                // Canal PCA9685 pour servo airflow
  uint8_t fingerAngleOpen;                   // Amplitude ouverture commune (degres)
  uint8_t halfHolePercent;                   // Pourcentage ouverture demi-trou (10-90, defaut 50)
  char embouchure[5];                        // Type: trav, bec, naf, end, oca
  FingerConfig fingers[MAX_FINGER_SERVOS];   // Config doigts (seuls [0..numFingers-1] actifs)
  NoteConfig notes[MAX_NOTES];               // Config notes (seuls [0..numNotes-1] actifs)

  // --- MIDI ---
  uint8_t midiChannel;

  // --- Serial MIDI (DIN input via UART) ---
  bool serialMidiEnabled;              // Activer l'entree MIDI serie
  uint8_t serialMidiRxPin;             // GPIO pour reception MIDI (RX)

  // --- Timing ---
  uint16_t servoToSolenoidDelayMs;
  uint16_t minNoteIntervalForValveCloseMs;
  uint16_t minNoteDurationMs;

  // --- Airflow servo ---
  uint16_t servoAirflowOff;
  uint16_t servoAirflowMin;
  uint16_t servoAirflowMax;

  // --- Angle servo (trav uniquement) ---
  uint16_t servoAngleOff;                    // Angle repos (centre)
  uint16_t servoAngleMin;                    // Angle min calibre
  uint16_t servoAngleMax;                    // Angle max calibre

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

  // --- Air delivery system (modulaire) ---
  uint8_t airMode;                   // 0-5 (voir AIR_MODE_* dans settings.h)
  uint8_t valveType;                 // 0=solenoide GPIO, 1=servo PCA
  uint8_t valveServoPcaChannel;      // Canal PCA9685 si valve=servo
  uint8_t valveServoCloseAngle;      // Angle ferme servo valve (0-180)
  uint8_t valveServoOpenAngle;       // Angle ouvert servo valve (0-180)
  // NB: valveServoDir et solenoidInterNoteMs ont ete supprimes. Le premier etait
  // redondant (les angles ferme/ouvert definissent deja le sens de course), le
  // second n'etait qu'une copie jamais relue de minNoteIntervalForValveCloseMs.
  // L'ancienne cle JSON "sol_inter" reste acceptee en lecture pour la
  // compatibilite ascendante.
  uint8_t motorType;                 // 0=PWM variable, 1=On/Off
  // Ventilateur (mode 3)
  uint8_t fanPin;                    // GPIO PWM ventilateur
  uint8_t fanMinPwm;                // PWM min (seuil demarrage)
  uint8_t fanMaxPwm;                // PWM max
  uint8_t fanIdlePercent;           // Vitesse idle entre notes (0-100%, 0=couper)
  uint16_t fanIdleTimeoutMs;        // Delai sans note avant coupure totale (ms, 0=jamais couper)
  uint8_t fanDefaultPercent;        // Autonomous pre-spin/idle default (0-100)
  uint8_t fanMaxNotePercent;        // Autonomous note demand ceiling (0-100)
  bool fanFollowAirflow;            // true = note demand follows airflow/velocity
  // Pompes (modes 4-5, 1 a 3 pompes)
  uint8_t numPumps;                  // 1-3
  uint8_t pumpPins[MAX_PUMPS];       // GPIO pour chaque pompe
  uint8_t pumpMinPwm[MAX_PUMPS];    // PWM min par pompe (si motorType=PWM)
  uint8_t pumpMaxPwm[MAX_PUMPS];    // PWM max par pompe (si motorType=PWM)
  uint8_t pumpCascadeThreshold;     // Seuil cascade (%) : pompe N+1 demarre quand demande > seuil (0=parallele)
  uint16_t pumpStaggerMs;           // Delai demarrage entre pompes (ms, anti-inrush)
  uint8_t pumpDirectIdlePercent;    // Autonomous direct-pump idle demand (0-100)
  uint8_t pumpDirectMaxPercent;     // Autonomous direct-pump note demand ceiling (0-100)
  bool pumpFollowAirflow;           // true = direct pump follows airflow/velocity
  uint8_t reservoirTargetPercent;   // Persistent reservoir target fill/pressure (0-100)
  bool reservoirAutoStart;          // true = regulate reservoir without web command
  uint8_t bangbangHysteresis;       // Hysteresis bang-bang (%) pour moteurs On/Off + capteur continu
  // Reservoir (mode 5) - capteur configurable
  uint8_t sensorType;               // 0=VL53L0X, 1=VL6180X, 2=Hall KY-024, 3=endstop meca, 4=endstop optique
  uint16_t sensorTargetMm;          // Hauteur cible (mm) - pour ToF
  uint16_t sensorMinMm;             // Hauteur min (vide) - pour ToF
  uint16_t sensorMaxMm;             // Hauteur max (plein) - pour ToF
  uint8_t pidKp;                     // Gain proportionnel PID (x10)
  uint8_t pidKi;                     // Gain integral PID (x10)
  // Endstop (sensorType 3 ou 4)
  uint8_t endstopPin;               // GPIO fin de course
  bool endstopActiveHigh;            // true = actif quand HIGH
  bool endstopPumpOn;                // true = pompe ON quand capteur actif (vider), false = remplir
  // Hall effect (sensorType 2)
  uint8_t hallPin;                   // GPIO analogique capteur Hall
  uint16_t hallThresholdLow;        // Seuil bas analogique
  uint16_t hallThresholdHigh;       // Seuil haut analogique
  // Servo angle (traversiere: oriente le jet d'air sur le biseau)
  bool angleServoEnabled;              // true = utiliser servo angle (traversiere)
  uint8_t angleServoPcaChannel;      // Canal PCA9685 servo angle (traversiere)

  // UI
  bool showAirSystem;                // Afficher schema pneumatique dans l'UI
  char resFormat[9];                 // "balloon" ou "bellows" (format visuel reservoir)

  // --- MIDI Storage ---
  uint16_t midiStorageLimitKb;       // Limite stockage total MIDI en Ko (defaut 500)

  // --- UI ---
  bool hideCalibration;              // Cacher l'onglet Calibration
  bool hideAir;                      // Cacher l'onglet Air
  uint8_t solenoidPin;               // GPIO solenoide (configurable)
  char instrumentColor[8];           // Couleur hex instrument "#RRGGBB"
  uint8_t kbdMode;                   // 0=flute (defaut), 1=piano
};


/*******************************************************************************
 * Centralized runtime configuration validation
 ******************************************************************************/
struct ConfigValidationResult {
  bool valid;
  bool restartRequired;
  bool corrected;
  String error;
  String warnings;
};

enum ConfigLoadStatus {
  CONFIG_DEFAULTS,
  CONFIG_LOADED,
  CONFIG_INVALID_FALLBACK,
  CONFIG_STORAGE_ERROR
};

// Etat du systeme de fichiers LittleFS.
//
// FAIL-SAFE : le firmware ne monte JAMAIS avec formatage automatique. Un
// LittleFS.begin(true) reformate la partition au premier echec de montage, ce
// qui, sur un instrument autonome, detruit silencieusement /config.json et les
// fichiers MIDI, puis redemarre sur la configuration par defaut - c'est-a-dire
// potentiellement sur un cablage hardware different (autre mode d'air, autres
// broches de pompe). Un montage impossible laisse donc la machine en mode
// recovery : OE des servos desactive, actionneurs interdits, diagnostic explicite,
// et formatage uniquement sur action volontaire de l'utilisateur.
enum FilesystemStatus {
  FS_NOT_MOUNTED,     // begin() pas encore appele
  FS_MOUNTED,         // monte, utilisable
  FS_MOUNT_FAILED,    // montage impossible (corruption / partition absente)
  FS_FORMATTED        // formate a la demande explicite de l'utilisateur, puis monte
};

#define CONFIG_MAX_POST_BYTES 32768
#define CONFIG_MIN_NOTE_DURATION_LIMIT_MS 0
#define CONFIG_MAX_NOTE_DURATION_LIMIT_MS 5000
#define CONFIG_MAX_SERVO_DELAY_MS 2000
#define CONFIG_MAX_GPIO 39
#define CONFIG_MAX_PWM 255
// Bornes de validation des flottants et des temporisations. Elles existent pour
// interdire les valeurs qui provoqueraient une division par zero, un debordement
// ou un mouvement de servo dangereux, pas pour brider un reglage musical.
#define CONFIG_MIN_VIBRATO_HZ 0.1f
#define CONFIG_MAX_VIBRATO_HZ 20.0f
#define CONFIG_MAX_VIBRATO_DEG 45.0f
#define CONFIG_MIN_CC2_CURVE 0.1f
#define CONFIG_MAX_CC2_CURVE 4.0f
#define CONFIG_MAX_INTERVAL_MS 5000
#define CONFIG_MAX_SOLENOID_PULSE_MS 5000
#define CONFIG_MAX_CC2_TIMEOUT_MS 60000
#define CONFIG_MIN_ATTACK_MS 10
#define CONFIG_MAX_ATTACK_MS 1000
#define CONFIG_MAX_FAN_IDLE_TIMEOUT_MS 60000
#define CONFIG_MAX_PUMP_STAGGER_MS 5000
#define CONFIG_MAX_UNPOWER_MS 60000
#define CONFIG_MAX_SENSOR_MM 4000
#define CONFIG_MAX_ADC_RAW 4095
#define CONFIG_MIN_MIDI_LIMIT_KB 50
#define CONFIG_MAX_MIDI_LIMIT_KB 2000

bool modeUsesPhysicalValve(uint8_t airMode);
bool configurationUsesSolenoidValve(const RuntimeConfig& config);
bool configurationUsesFan(const RuntimeConfig& config);
bool configurationUsesPumps(const RuntimeConfig& config);
bool configurationUsesReservoirSensor(const RuntimeConfig& config);
ConfigValidationResult validateAndNormalizeConfig(RuntimeConfig& config, const RuntimeConfig* previousConfig = nullptr);

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
  static ConfigLoadStatus loadWithStatus();
  static ConfigLoadStatus lastLoadStatus();
  static const String& lastLoadError();

  // Sauvegarde la config actuelle sur LittleFS
  static bool save();
  // Sauvegarde une configuration donnee (candidat transactionnel) sans toucher
  // a la configuration active.
  static bool saveFrom(const RuntimeConfig& source);

  // --- Systeme de fichiers (fail-safe, voir FilesystemStatus) ---------------
  // Monte LittleFS SANS formatage automatique. Retourne true si monte.
  static bool beginFilesystem();
  static FilesystemStatus filesystemStatus();
  static const String& filesystemError();
  static bool isFilesystemMounted();
  // Formatage VOLONTAIRE : detruit tout le contenu (config + MIDI). N'est jamais
  // appele automatiquement ; reserve au mode recovery declenche par l'utilisateur.
  static bool formatFilesystem();

  // Remet cfg aux valeurs par defaut et sauvegarde
  static bool resetToDefaults();

  // Reset usine: remet cfg aux defauts et supprime le fichier config
  static bool factoryReset();

  // Verifie si c'est le premier demarrage (pas de config sauvegardee)
  static bool isFirstBoot();
};

#endif
