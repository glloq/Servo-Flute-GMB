/***********************************************************************************************
----------------------------         SETTINGS ESP32            --------------------------------
Configuration modulaire pour instruments a vent robotises.
Supporte jusqu'a 2 PCA9685 (32 canaux) : 1 a 31 servos doigts + 1 servo airflow.
Version ESP32-WROOM avec BLE-MIDI / WiFi-MIDI / Hotspot
Architecture avec servo debit + solenoide valve + mode binaire (ouvert/ferme)
************************************************************************************************/
#ifndef SETTINGS_H
#define SETTINGS_H
#include "stdint.h"

#define DEBUG 1

/*******************************************************************************
-------------------------   LIMITES INSTRUMENT (compile)  --------------------
Dimensionnement memoire maximal. Les valeurs effectives sont dans RuntimeConfig.
******************************************************************************/

// Maximums (dimensionnement arrays)
#define MAX_FINGER_SERVOS 31   // 2x PCA9685 = 32 canaux, 1 reserve pour airflow

// Adresses I2C des cartes PCA9685
#define PCA_ADDR_BOARD0 0x40   // Carte 1 (defaut) — canaux 0-15
#define PCA_ADDR_BOARD1 0x41   // Carte 2 (optionnelle) — canaux 16-31
#define MAX_NOTES 128          // Notes jouables maximum (plage MIDI complète = 128)

// Valeurs par defaut (utilisees par initDefaults / preset "Flute irlandaise C")
#define DEFAULT_NUM_FINGERS 6
#define DEFAULT_NUM_NOTES 14
#define DEFAULT_AIRFLOW_PCA_CHANNEL 10

/*******************************************************************************
-----------------------   CONFIGURATION GPIO ESP32    ------------------------
ESP32-WROOM Pin Assignment
******************************************************************************/

// LED d'etat
#define STATUS_LED_PIN 2          // GPIO2 (LED integree sur la plupart des cartes)

// Bouton d'appairage
#define PAIRING_BUTTON_PIN 0      // GPIO0 (bouton BOOT sur les cartes de dev)

// Interrupteur BT/WiFi (HIGH = WiFi, LOW = Bluetooth)
#define MODE_SWITCH_PIN 4         // GPIO4

// I2C (PCA9685)
#define I2C_SDA_PIN 21            // GPIO21 (defaut ESP32)
#define I2C_SCL_PIN 22            // GPIO22 (defaut ESP32)

// Solenoide
#define SOLENOID_PIN 13           // GPIO13

// Alimentation servos (PCA9685 OE pin)
#define PIN_SERVOS_OFF 5          // GPIO5

/*******************************************************************************
---------------------------   INMP441 MICROPHONE     -------------------------
Optional I2S MEMS microphone for automatic calibration.
Set MIC_ENABLED to false if no mic is connected.
******************************************************************************/
#define MIC_ENABLED true

// I2S Pins for INMP441
#define MIC_PIN_BCLK  14         // GPIO14 - Bit Clock (SCK)
#define MIC_PIN_LRCLK 15         // GPIO15 - Word Select (WS)
#define MIC_PIN_DIN   32         // GPIO32 - Data In (SD)

// I2S Configuration
// INMP441 outputs 24-bit samples left-aligned in a 32-bit I2S word. A 32 kHz
// sample rate widens the usable pitch range (Nyquist 16 kHz) and improves YIN
// tau resolution for the flute's upper octave while keeping DMA buffers small.
#define MIC_I2S_PORT    I2S_NUM_0
#define MIC_SAMPLE_RATE 32000
#define MIC_BUFFER_SIZE 1024            // Samples per analysis frame (~32 ms @ 32 kHz)
#define MIC_DMA_BUF_COUNT 4             // Number of DMA descriptors
#define MIC_DMA_BUF_LEN   256           // Frames per DMA descriptor (4*256 = 1024 buffered)

// Audio analysis thresholds
#define MIC_RMS_THRESHOLD       0.02f   // Legacy fixed RMS gate (fallback / monitoring only)
#define MIC_RMS_ABSOLUTE_MIN    0.002f  // Absolute RMS floor: below this we never call it sound
#define MIC_PITCH_MIN_HZ        200.0f  // Lowest detectable pitch
#define MIC_PITCH_MAX_HZ        4000.0f // Highest detectable pitch
#define MIC_PITCH_TOLERANCE_CENTS 200   // (legacy) coarse pitch tolerance for monitoring
#define MIC_YIN_THRESHOLD       0.15f   // YIN absolute threshold (aperiodicity dip to accept)
#define MIC_YIN_CONFIDENCE_MIN  0.80f   // Min confidence (1 - yin[tau]) to accept a pitch
// Octave-up guard: only correct a detected lag to twice its value (the octave
// below) when d(2*tau) is at least this much deeper than d(tau). Kept strict so
// harmonic-rich tones are not pushed an octave down.
#define MIC_YIN_OCTAVE_RATIO    0.60f
// YIN lag buffer upper bound. tauMax = MIC_SAMPLE_RATE / MIC_PITCH_MIN_HZ.
// At 32 kHz / 200 Hz that is 160; 200 leaves headroom while keeping the buffer
// off the stack (stored as an AudioAnalyzer member, ~0.8 kB).
#define MIC_YIN_TAU_MAX         200
// If no fresh I2S frame has been analysed for this long, the analyzer marks its
// pitch measurement stale (pitchValid=false) so consumers never trust old data.
#define MIC_FRAME_STALE_MS      250

/*----------------------------------------------------------------------------
 * Auto-calibration (microphone-driven per-note airflow calibration)
 *--------------------------------------------------------------------------*/

// --- Adaptive noise floor (measured per note, valve closed, air at rest) ---
#define AUTOCAL_NOISE_MEASURE_MS 500    // Background-noise capture window (ms)
#define AUTOCAL_NOISE_RATIO      4.0f   // soundThreshold = max(MIC_RMS_ABSOLUTE_MIN, noiseRms * ratio)

// --- Pitch tolerances (cents) ---
// Wide tolerance is used only to detect the initial appearance of the note.
// Strict tolerance defines the stable plateau and the nominal value.
#define AUTOCAL_PITCH_TOLERANCE_ONSET_CENTS  80
#define AUTOCAL_PITCH_TOLERANCE_STABLE_CENTS 50

// --- Servo/audio synchronization (SET -> SETTLE -> COLLECT -> EVALUATE) ---
#define AUTOCAL_AIR_SETTLE_MS       120 // Wait after changing airflow before collecting audio
#define AUTOCAL_AUDIO_FRAMES_PER_STEP 5 // Audio frames collected per airflow position
#define AUTOCAL_REQUIRED_VALID_FRAMES 4 // Frames (of the above) that must match to accept a position
#define AUTOCAL_FRAME_SAMPLE_MS     40  // Interval between two sampled audio frames (~audio rate)

// --- Coarse then fine sweep ---
#define AUTOCAL_COARSE_STEP_PERCENT 4   // Airflow step (%) during the coarse pass
#define AUTOCAL_FINE_STEP_PERCENT   1   // Airflow step (%) during the fine pass
#define AUTOCAL_FINE_MARGIN_PERCENT 4   // How far below a coarse boundary the fine search restarts
#define AUTOCAL_LOSS_CONFIRM_STEPS  2   // Consecutive invalid positions needed to confirm loss/overblow

// --- Nominal airflow selection ---
#define AUTOCAL_NOMINAL_FRACTION    0.40f // Default nominal = min + fraction*(max-min)
#define AUTOCAL_NOMINAL_GUARD_PCT   5     // Keep nominal at least this far from min/max when possible

// --- Result acceptance ---
// A note whose final confidence is below this is refused (not written to config).
#define AUTOCAL_MIN_RESULT_CONFIDENCE 40

// --- Timeouts (firmware side, not browser side) ---
// Per-note budget: a note exceeding it fails (keeps its old calibration) and the
// sweep moves on to the next note.
#define AUTOCAL_TIMEOUT_PER_NOTE_MS   25000
// Extra slack added on top of numNotes * per-note for the computed global timeout.
#define AUTOCAL_GLOBAL_MARGIN_MS      20000
// While collecting a position, abort the collection if no fresh audio frame
// arrives within this window (frozen / disconnected source).
#define AUTOCAL_AUDIO_FRAME_TIMEOUT_MS 1500
// Absolute last-resort ceiling for the whole calibration. It must never be smaller
// than the sum of the per-note budgets for the largest possible note count, or a
// large configuration would be aborted by the global timeout before it could ever
// use the budget it was granted (this happened around 24 notes with the old fixed
// 600 s ceiling). Sized from MAX_NOTES so any valid note count fits.
#define AUTOCAL_GLOBAL_TIMEOUT_MS \
  ((unsigned long)MAX_NOTES * AUTOCAL_TIMEOUT_PER_NOTE_MS + AUTOCAL_GLOBAL_MARGIN_MS)
// Time allowed for the air supply (pump spin-up / reservoir fill / fan ramp) to
// reach a usable state before the whole calibration aborts with ACAL_FAIL_AIR_SUPPLY.
// Passive modes (solenoid / servo valve / servo only) report ready immediately.
#define AUTOCAL_AIR_READY_TIMEOUT_MS  20000
// Reservoir (mode 5) is considered ready once its fill level is within this many
// percent of the requested target (a perfect match is not required).
#define AUTOCAL_RESERVOIR_READY_MARGIN 5

// --- Broadcast / legacy timing (still used by range finder + WS throttling) ---
#define AUTOCAL_SETTLE_MS       300     // Range finder: wait after positioning servos
#define AUTOCAL_SILENCE_COUNT   3       // Range finder: consecutive silent steps = sound gone
#define AUTOCAL_AUDIO_INTERVAL_MS 100   // WebSocket audio/progress broadcast interval
#define AUTOCAL_PITCH_TOLERANCE_SEMI 3  // Range finder: coarse pitch tolerance in semitones
#define AUTOCAL_MIN_RANGE_PCT   5       // Minimum range pct between air_min and air_max
#define AUTOCAL_RF_MARGIN_DEG   3       // Safety margin (degrees) applied to range finder results
// Range finder now reuses the SET->SETTLE->COLLECT->EVALUATE engine and sweeps
// only a configurable safe angle window (never a blind 0-180 sweep).
#define AUTOCAL_RF_MIN_SAFE_ANGLE 30    // Lowest airflow servo angle the sweep visits
#define AUTOCAL_RF_MAX_SAFE_ANGLE 150   // Highest airflow servo angle the sweep visits
#define AUTOCAL_RF_STEP_DEG       3     // Angle step between evaluated positions

/*******************************************************************************
---------------------------   TIMING SETTINGS (ms)    ------------------------
******************************************************************************/
// Delai total entre positionnement servos et activation solenoide
#define SERVO_TO_SOLENOID_DELAY_MS  105

// Si deux notes sont espacees de moins que ce delai, on garde la valve ouverte
#define MIN_NOTE_INTERVAL_FOR_VALVE_CLOSE_MS  50

#define MIN_NOTE_DURATION_MS    10

/*******************************************************************************
---------------------------   EVENT QUEUE SETTINGS    ------------------------
******************************************************************************/
#define EVENT_QUEUE_SIZE 16

/*******************************************************************************
---------------------------     SOLENOID VALVE        ------------------------
******************************************************************************/
#define SOLENOID_ACTIVE_HIGH true

// MODE PWM (option pour reduction chaleur)
#define SOLENOID_USE_PWM true

// Parametres PWM (si SOLENOID_USE_PWM = true)
#define SOLENOID_PWM_ACTIVATION 255
#define SOLENOID_PWM_HOLDING    128
#define SOLENOID_ACTIVATION_TIME_MS 50

/*******************************************************************************
---------------------------   AIR FLOW SERVO          ------------------------
******************************************************************************/
#define SERVO_AIRFLOW_OFF 20      // Angle repos (pas de note)
#define SERVO_AIRFLOW_MIN 60      // Angle minimum absolu
#define SERVO_AIRFLOW_MAX 100     // Angle maximum absolu

// Pivot value (velocity / CC2, range 1-127) that maps to the calibrated nominal
// airflow in the two-segment playing curve. 64 = musical mid-dynamic.
#define AIRFLOW_SOURCE_PIVOT 64

/*******************************************************************************
---------------------------   AIR ANGLE SERVO (trav)   ----------------------
Servo d'angle du jet d'air — uniquement pour flute traversiere (embouchure "trav").
Controle l'inclinaison du flux d'air par rapport au biseau de l'embouchure.
******************************************************************************/
#define DEFAULT_ANGLE_PCA_CHANNEL 12   // Canal PCA9685 pour servo angle
#define SERVO_ANGLE_OFF  90            // Angle repos (centre)
#define SERVO_ANGLE_MIN  60            // Angle min calibre
#define SERVO_ANGLE_MAX  120           // Angle max calibre
#define DEFAULT_ANGLE_PERCENT 50       // % par defaut par note (milieu de plage)

/*******************************************************************************
-----------------------   AIR DELIVERY SYSTEM          ------------------------
Modes modulaires de gestion d'air. L'interface s'adapte au mode choisi.
  0 = Classique (solenoide + servo flow)
  1 = Servo-valve (servo PCA remplace solenoide)
  2 = Servo flow seul (pas de valve)
  3 = Ventilateur + servo flow
  4 = Pompe(s) + valve + servo flow (direct, sans reservoir)
  5 = Pompe(s) + reservoir + valve + servo flow (avec capteur)
******************************************************************************/
// Modes
#define AIR_MODE_SOLENOID_SERVO   0
#define AIR_MODE_SERVO_VALVE      1
#define AIR_MODE_SERVO_ONLY       2
#define AIR_MODE_FAN_SERVO        3
#define AIR_MODE_PUMP_VALVE       4
#define AIR_MODE_PUMP_RESERVOIR   5

// Legacy aliases
#define AIR_MODE_CLASSIC          AIR_MODE_SOLENOID_SERVO
#define AIR_MODE_PUMP             AIR_MODE_PUMP_VALVE
#define AIR_MODE_PUMP_ENDSTOP     5  // Fusionne dans mode 5 (sensor_type selectionne endstop)
#define AIR_MODE_PUMP_RESERVOIR_LEGACY AIR_MODE_PUMP_RESERVOIR

// Types de moteur (pompe/ventilateur)
#define MOTOR_TYPE_PWM    0     // Moteur PWM variable
#define MOTOR_TYPE_ONOFF  1     // Moteur On/Off simple (GPIO HIGH/LOW)

// Types de capteur reservoir (mode 5)
#define SENSOR_TYPE_TOF_VL53L0X    0   // Capteur distance VL53L0X
#define SENSOR_TYPE_TOF_VL6180X    1   // Capteur distance VL6180X
#define SENSOR_TYPE_HALL_KY024     2   // Capteur effet Hall lineaire KY-024
#define SENSOR_TYPE_ENDSTOP_MECH   3   // Fin de course mecanique
#define SENSOR_TYPE_ENDSTOP_OPT    4   // Fin de course optique

// Max pumps
#define MAX_PUMPS 3

// Defaults
#define DEFAULT_AIR_MODE            AIR_MODE_SOLENOID_SERVO
#define DEFAULT_VALVE_TYPE          0      // 0=solenoide GPIO, 1=servo PCA
#define DEFAULT_VALVE_SERVO_CH      11     // Canal PCA si valve=servo
#define DEFAULT_ANGLE_SERVO_CH      12     // Canal PCA servo angle (traversiere)
#define DEFAULT_MOTOR_TYPE          MOTOR_TYPE_PWM
#define DEFAULT_NUM_PUMPS           1
#define DEFAULT_PUMP_PIN            25     // GPIO25 (DAC capable)
#define DEFAULT_PUMP_MIN_PWM        80     // Seuil demarrage moteur DC
#define DEFAULT_PUMP_MAX_PWM        255
#define DEFAULT_FAN_PIN             26     // GPIO26 pour ventilateur
#define DEFAULT_FAN_MIN_PWM         60     // Seuil demarrage ventilateur
#define DEFAULT_FAN_MAX_PWM         255
#define DEFAULT_FAN_IDLE_PERCENT    20     // Vitesse idle entre notes (% du PWM range)
#define DEFAULT_FAN_IDLE_TIMEOUT_MS 5000   // Couper le ventilateur apres 5s sans note
#define DEFAULT_ENDSTOP_PIN         34     // GPIO34 (input only)
#define DEFAULT_ENDSTOP_ACTIVE_HIGH true
#define DEFAULT_HALL_PIN            36     // GPIO36 (ADC, input only)
#define DEFAULT_HALL_THRESHOLD_LOW  1500   // Seuil bas analogique Hall
#define DEFAULT_HALL_THRESHOLD_HIGH 2500   // Seuil haut analogique Hall
#define DEFAULT_RESERVOIR_ENABLED   false
#define DEFAULT_SENSOR_TYPE         SENSOR_TYPE_TOF_VL6180X
#define DEFAULT_SENSOR_TARGET_MM    50     // Hauteur cible ballon (mm)
#define DEFAULT_SENSOR_MIN_MM       10     // Hauteur min (vide)
#define DEFAULT_SENSOR_MAX_MM       150    // Hauteur max (plein)
#define DEFAULT_PID_KP              30     // Gain proportionnel (x10, soit 3.0)
#define DEFAULT_PID_KI              5      // Gain integral (x10, soit 0.5)
#define DEFAULT_SHOW_AIR_SYSTEM     false

// Gestion multi-pompes
#define DEFAULT_PUMP_CASCADE_THRESHOLD 80  // Seuil (%) pour activer pompe suivante (0=toutes en parallele)
#define DEFAULT_PUMP_STAGGER_MS     150    // Delai entre demarrage de chaque pompe (anti-inrush)
#define DEFAULT_BANGBANG_HYSTERESIS 5      // Bande hysteresis (%) pour moteurs On/Off avec capteur continu

// Timing capteur / PID
#define PRESSURE_READ_INTERVAL_MS   50     // Intervalle lecture capteur (ms)
#define PRESSURE_PID_INTERVAL_MS    100    // Intervalle boucle PID (ms)
#define PUMP_SAFETY_MIN_MM          5      // Distance min avant coupure securite (surgonflage)
#define PUMP_SAFETY_MAX_DIST_MM    300    // Distance max (30cm) : capteur hors portee, couper pompe

/*******************************************************************************
---------------------------   POWER MANAGEMENT        ------------------------
******************************************************************************/
#define TIMEUNPOWER 200

/*******************************************************************************
------------------   CONFIGURATION SERVOS DOIGTS (defauts)  ------------------
Valeurs par defaut pour preset "Flute irlandaise C" (6 trous).
Chargees au premier boot, puis surchargees par /config.json.

Structure : {PCA_channel, angle_ferme, sens_ouverture, trou_arriere}

ORDRE DES TROUS (Irish Flute standard) :
  0 = Trou 1 (main gauche - index)
  1 = Trou 2 (main gauche - majeur)
  2 = Trou 3 (main gauche - annulaire)
  3 = Trou 4 (main droite - index)
  4 = Trou 5 (main droite - majeur)
  5 = Trou 6 (main droite - annulaire)
******************************************************************************/
#define ANGLE_OPEN 30  // Angle d'ouverture du trou (degres) par defaut

struct DefaultFingerConfig {
  uint8_t pcaChannel;
  uint16_t closedAngle;
  int8_t direction;
  bool isThumbHole;
};

const DefaultFingerConfig DEFAULT_FINGERS[DEFAULT_NUM_FINGERS] = {
  // PCA  Ferme  Sens  Pouce
  {  0,   90,   -1,   false },  // Trou 1
  {  1,   95,   -1,   false },  // Trou 2
  {  2,   90,   -1,   false },  // Trou 3
  {  3,   100,  -1,   false },  // Trou 4
  {  4,   95,   -1,   false },  // Trou 5
  {  5,   90,   -1,   false }   // Trou 6
};

/*******************************************************************************
-----------------   NOTES JOUABLES PAR DEFAUT (preset)   --------------------
Preset "Flute irlandaise en C" - 14 notes de A#5 a G7
Structure : {MIDI, {doigtes[6]}, flow_min%, flow_max%}
******************************************************************************/

struct DefaultNoteConfig {
  uint8_t midiNote;
  bool fingerPattern[DEFAULT_NUM_FINGERS];
  uint8_t airflowMinPercent;
  uint8_t airflowMaxPercent;
  uint8_t airflowNominalPercent;   // Recommended airflow (min <= nominal <= max)
  uint8_t anglePercent;            // Angle jet d'air (0-100%), trav uniquement
};

const DefaultNoteConfig DEFAULT_NOTES[DEFAULT_NUM_NOTES] = {
  // MIDI  Doigtes            AirMin AirMax Nominal Angle
  // OCTAVE BASSE
  {  82,  {0,1,1,1,1,1},  10,  60,  30,  40  },  // A#5
  {  83,  {1,1,1,1,1,1},  0,   50,  20,  40  },  // B5

  // OCTAVE 1 - MEDIUM
  {  84,  {0,0,0,0,0,0},  20,  75,  42,  45  },  // C6
  {  86,  {0,0,0,0,0,1},  15,  70,  37,  45  },  // D6
  {  88,  {0,0,0,0,1,1},  10,  65,  32,  48  },  // E6
  {  89,  {0,0,0,1,1,1},  10,  60,  30,  48  },  // F6
  {  91,  {0,0,1,1,1,1},  5,   55,  25,  50  },  // G6
  {  93,  {0,1,1,1,1,1},  5,   50,  23,  50  },  // A6
  {  95,  {1,1,1,1,1,1},  0,   45,  18,  52  },  // B6

  // OCTAVE 2 - AIGU
  {  96,  {0,0,0,0,0,0},  50,  100, 70,  60  },  // C7
  {  98,  {0,0,0,0,0,1},  45,  95,  65,  62  },  // D7
  {  100, {0,0,0,0,1,1},  40,  90,  60,  65  },  // E7
  {  101, {0,0,0,1,1,1},  35,  85,  55,  68  },  // F7
  {  103, {0,0,1,1,1,1},  30,  80,  50,  70  }   // G7
};

/*******************************************************************************
-----------------------    SERVO PWM PARAMETERS       ------------------------
******************************************************************************/
#define SERVO_MIN_ANGLE 0
#define SERVO_MAX_ANGLE 180
const uint16_t SERVO_PULSE_MIN = 550;
const uint16_t SERVO_PULSE_MAX = 2450;
const uint16_t SERVO_FREQUENCY = 50;

/*******************************************************************************
-------------------------     MIDI SETTINGS           ------------------------
******************************************************************************/
// Canal MIDI (0 = omni mode, ecoute tous les canaux | 1-16 = canal specifique)
#define MIDI_CHANNEL 0

// Serial MIDI input (DIN MIDI via UART2)
#define DEFAULT_SERIAL_MIDI_ENABLED false
#define DEFAULT_SERIAL_MIDI_RX_PIN 16    // GPIO16 (RX2 par defaut sur ESP32)

/*******************************************************************************
-----------------------  CONTROL CHANGE (CC) SETTINGS  -----------------------
******************************************************************************/
#define CC_RATE_LIMIT_PER_SECOND 10

// Vibrato (CC1 - Modulation)
#define VIBRATO_FREQUENCY_HZ 6.0
#define VIBRATO_MAX_AMPLITUDE_DEG 8.0

// Valeurs par defaut CC au demarrage
#define CC_VOLUME_DEFAULT 127
#define CC_EXPRESSION_DEFAULT 127
#define CC_MODULATION_DEFAULT 0
#define CC_BREATH_DEFAULT 127
#define CC_BRIGHTNESS_DEFAULT 64

/*******************************************************************************
--------------------  BREATH CONTROLLER (CC2) SETTINGS  ----------------------
******************************************************************************/
#define CC2_ENABLED true
#define CC2_RATE_LIMIT_PER_SECOND 50
#define CC2_SILENCE_THRESHOLD 10
#define CC2_SMOOTHING_BUFFER_SIZE 5
#define CC2_RESPONSE_CURVE 1.4
#define CC2_TIMEOUT_MS 1000

/*******************************************************************************
-----------------------  WIRELESS SETTINGS (ESP32)    ------------------------
******************************************************************************/

// Nom du peripherique BLE-MIDI et WiFi
#define DEVICE_NAME "ServoFlute"

// WiFi AP mode (hotspot) settings
#define AP_SSID "ServoFlute-Setup"
#define AP_PASSWORD ""                // Pas de mot de passe par defaut (portail ouvert)
#define AP_CHANNEL 1
#define AP_MAX_CONNECTIONS 2

// WiFi STA connection timeout (ms) avant fallback AP
#define WIFI_CONNECT_TIMEOUT_MS 10000

// rtpMIDI port
#define RTPMIDI_PORT 5004

// Web server port
#define WEB_SERVER_PORT 80

// mDNS hostname (accessible via servo-flute.local)
#define MDNS_HOSTNAME "servo-flute"

/*******************************************************************************
-----------------------  HARDWARE INPUT SETTINGS      ------------------------
******************************************************************************/

// Debounce pour bouton et switch (ms)
#define BUTTON_DEBOUNCE_MS 50

// Duree appui long bouton (ms) pour forcer hotspot
#define BUTTON_LONG_PRESS_MS 3000

// Fenetre double appui bouton (ms) — ouvre tous les doigts
#define BUTTON_DOUBLE_PRESS_MS 500

// Intervalle lecture switch (ms)
#define SWITCH_READ_INTERVAL_MS 100

/*******************************************************************************
-----------------------  STATUS LED SETTINGS          ------------------------
******************************************************************************/

// Patterns LED (durees en ms)
#define LED_BLINK_FAST_MS 100         // Clignotement rapide (advertising/connexion)
#define LED_BLINK_SLOW_MS 1000        // Clignotement lent (connecte, attente MIDI)
#define LED_DOUBLE_FLASH_MS 150       // Double flash (WiFi STA connecte)
#define LED_DOUBLE_FLASH_PAUSE_MS 800 // Pause entre doubles flashs
#define LED_TRIPLE_FLASH_MS 150       // Triple flash (mode hotspot)
#define LED_TRIPLE_FLASH_PAUSE_MS 800 // Pause entre triples flashs

/*******************************************************************************
-----------------------  WEB CONFIGURATOR SETTINGS    ------------------------
******************************************************************************/

// WebSocket
#define WS_MAX_CLIENTS 4              // Max clients WebSocket simultanes
#define WS_CLEANUP_INTERVAL_MS 1000   // Intervalle nettoyage clients deconnectes
#define WS_STATUS_INTERVAL_MS 500     // Intervalle envoi status aux clients

/*******************************************************************************
-----------------------  MIDI FILE PLAYER SETTINGS    ------------------------
******************************************************************************/

#define MIDI_FILE_MAX_SIZE 102400     // Taille max fichier MIDI (100 KB)
#define MIDI_FILE_MAX_EVENTS 2000     // Nombre max d'evenements parses
#define MIDI_FILE_PATH "/midi_temp.mid"  // Chemin temporaire (compat, parsing)
#define MIDI_DIR "/midi"              // Repertoire stockage fichiers MIDI
#define DEFAULT_MIDI_STORAGE_LIMIT_KB 500  // Limite stockage total MIDI (Ko)

/*******************************************************************************
-----------------------  WATCHDOG SETTINGS (ESP32)    ------------------------
******************************************************************************/
#define WATCHDOG_TIMEOUT_MS 4000      // Timeout watchdog en ms

/*******************************************************************************
-----------------------  MIDI PROTOCOL CONSTANTS     -----------------------
Standard MIDI constants used across the codebase.
******************************************************************************/
#define MIDI_CC_MAX 127               // Maximum value for any MIDI CC or velocity
#define MIDI_VELOCITY_MAX 127         // Maximum MIDI velocity
#define MIDI_CC_MODULATION 1          // CC 1: Modulation (vibrato)
#define MIDI_CC_BREATH 2              // CC 2: Breath Controller
#define MIDI_CC_VOLUME 7              // CC 7: Channel Volume
#define MIDI_CC_EXPRESSION 11         // CC 11: Expression
#define MIDI_CC_ATTACK_TIME 73        // CC 73: Attack Time (mode attaque souffle)
#define MIDI_CC_BRIGHTNESS 74         // CC 74: Sound Brightness
#define MIDI_CC_ALL_SOUND_OFF 120     // CC 120: All Sound Off
#define MIDI_CC_RESET_ALL_CONTROLLERS 121 // CC 121: Reset All Controllers
#define MIDI_CC_ALL_NOTES_OFF 123     // CC 123: All Notes Off

/*******************************************************************************
-----------------------  RATE LIMITING CONSTANTS     -----------------------
******************************************************************************/
#define CC_RATE_WINDOW_MS 1000        // Window for CC rate limiting (ms)

/*******************************************************************************
-----------------------  PWM / SIGNAL CONSTANTS      -----------------------
******************************************************************************/
#define PWM_MAX_VALUE 255             // Max PWM value (8-bit)

/*******************************************************************************
---------------------  SINE LUT CONSTANTS (VIBRATO)  ----------------------
******************************************************************************/
#define SIN_LUT_SIZE 256              // Number of entries in sine lookup table
#define SIN_LUT_SCALE 127.0f          // Amplitude scale of sine LUT values

/*******************************************************************************
-----------------------  INIT / STARTUP DELAYS       -----------------------
******************************************************************************/
#define SAFE_STATE_SETTLE_MS 100      // Delay after safe state init (servo settle)
#define SERIAL_STARTUP_DELAY_MS 500   // Delay for serial port initialization
#define PWM_INIT_DELAY_MS 10          // Delay after PCA9685 frequency set

/*******************************************************************************
-----------------------  WEB INTERFACE CONSTANTS     -----------------------
******************************************************************************/
#define WEB_DEFAULT_VELOCITY 100      // Default velocity for web keyboard
#define TEST_NOTE_SOLENOID_MS 2000    // Solenoid open duration for note test (ms)
#define VU_METER_SCALE 500            // RMS to percentage scale for VU meter
#define PITCH_OK_CENTS 15             // Pitch tolerance (cents) shown as "OK"

#endif
