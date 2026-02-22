/***********************************************************************************************
----------------------------         SETTINGS ESP32            --------------------------------
Configuration modulaire pour instruments a vent robotises.
Supporte 1 a 15 servos doigts + 1 servo airflow sur PCA9685.
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
#define MAX_FINGER_SERVOS 15   // PCA9685 = 16 canaux, 1 reserve pour airflow
#define MAX_NOTES 32           // Notes jouables maximum

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
#define MIC_I2S_PORT    I2S_NUM_0
#define MIC_SAMPLE_RATE 16000
#define MIC_BUFFER_SIZE 1024
#define MIC_DMA_BUF_COUNT 4
#define MIC_DMA_BUF_LEN   256

// Audio analysis thresholds
#define MIC_RMS_THRESHOLD       0.02f   // Min RMS for "sound detected"
#define MIC_PITCH_MIN_HZ        200.0f  // Lowest detectable pitch
#define MIC_PITCH_MAX_HZ        4000.0f // Highest detectable pitch
#define MIC_PITCH_TOLERANCE_CENTS 200   // Pitch tolerance for auto-cal
#define MIC_YIN_THRESHOLD       0.15f   // YIN confidence threshold

// Auto-calibration timing
#define AUTOCAL_SETTLE_MS       300     // Wait after positioning servos
#define AUTOCAL_STEP_MS         80      // Time per airflow step
#define AUTOCAL_SILENCE_COUNT   3       // Consecutive silent steps = sound gone
#define AUTOCAL_AUDIO_INTERVAL_MS 100   // Audio broadcast interval
#define AUTOCAL_SWEEP_OVERSHOOT 5       // Degrees past max to check during sweep
#define AUTOCAL_PITCH_TOLERANCE_SEMI 3  // Pitch tolerance in semitones
#define AUTOCAL_MIN_RANGE_PCT   5       // Minimum range pct between air_min and air_max
#define AUTOCAL_STORE_DELAY_MS  10      // Delay before storing result in NOTE_DONE state
#define AUTOCAL_NOTE_INTERVAL_MS 200    // Pause between notes during auto-cal

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

/*******************************************************************************
-----------------------   AIR DELIVERY SYSTEM          ------------------------
Modes : 0=classique (solenoide), 1=servo-valve, 2=pompe directe,
        3=pompe + reservoir avec capteur distance
******************************************************************************/
// Modes
#define AIR_MODE_CLASSIC    0
#define AIR_MODE_SERVO_VALVE 1
#define AIR_MODE_PUMP       2
#define AIR_MODE_PUMP_RESERVOIR 3

// Defaults
#define DEFAULT_AIR_MODE            AIR_MODE_CLASSIC
#define DEFAULT_VALVE_USE_SERVO     false
#define DEFAULT_VALVE_SERVO_CH      11     // Canal PCA si valve=servo
#define DEFAULT_PUMP_ENABLED        false
#define DEFAULT_PUMP_PIN            25     // GPIO25 (DAC capable)
#define DEFAULT_PUMP_MIN_PWM        80     // Seuil demarrage moteur DC
#define DEFAULT_PUMP_MAX_PWM        255
#define DEFAULT_RESERVOIR_ENABLED   false
#define DEFAULT_SENSOR_TYPE         1      // 0=VL53L0X, 1=VL6180X
#define DEFAULT_SENSOR_TARGET_MM    50     // Hauteur cible ballon (mm)
#define DEFAULT_SENSOR_MIN_MM       10     // Hauteur min (vide)
#define DEFAULT_SENSOR_MAX_MM       150    // Hauteur max (plein)
#define DEFAULT_PID_KP              30     // Gain proportionnel (x10, soit 3.0)
#define DEFAULT_PID_KI              5      // Gain integral (x10, soit 0.5)
#define DEFAULT_SHOW_AIR_SYSTEM     false

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
};

const DefaultNoteConfig DEFAULT_NOTES[DEFAULT_NUM_NOTES] = {
  // OCTAVE BASSE
  {  82,  {0,1,1,1,1,1},  10,  60  },  // A#5
  {  83,  {1,1,1,1,1,1},  0,   50  },  // B5

  // OCTAVE 1 - MEDIUM
  {  84,  {0,0,0,0,0,0},  20,  75  },  // C6
  {  86,  {0,0,0,0,0,1},  15,  70  },  // D6
  {  88,  {0,0,0,0,1,1},  10,  65  },  // E6
  {  89,  {0,0,0,1,1,1},  10,  60  },  // F6
  {  91,  {0,0,1,1,1,1},  5,   55  },  // G6
  {  93,  {0,1,1,1,1,1},  5,   50  },  // A6
  {  95,  {1,1,1,1,1,1},  0,   45  },  // B6

  // OCTAVE 2 - AIGU
  {  96,  {0,0,0,0,0,0},  50,  100 },  // C7
  {  98,  {0,0,0,0,0,1},  45,  95  },  // D7
  {  100, {0,0,0,0,1,1},  40,  90  },  // E7
  {  101, {0,0,0,1,1,1},  35,  85  },  // F7
  {  103, {0,0,1,1,1,1},  30,  80  }   // G7
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
