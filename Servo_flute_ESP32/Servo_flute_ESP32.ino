/***********************************************************************************************
 * SERVO FLUTE ESP32 - Automated Recorder Player
 *
 * Version ESP32-WROOM avec connectivite sans fil :
 * - BLE-MIDI (Bluetooth Low Energy) via NimBLE
 * - WiFi-MIDI (rtpMIDI / AppleMIDI)
 * - Mode hotspot autonome (fallback / force par bouton)
 * - Serveur web : clavier virtuel, lecteur fichiers MIDI, config, monitoring
 *
 * Hardware:
 * - ESP32-WROOM-32E
 * - PCA9685 PWM Driver (I2C : SDA=GPIO21, SCL=GPIO22)
 * - 6 servos SG90 pour les doigts
 * - 1 servo pour le debit d'air
 * - 1 solenoide pour valve on/off (GPIO13)
 * - 1 LED d'etat (GPIO2)
 * - 1 bouton d'appairage (GPIO0 - BOOT)
 * - 1 interrupteur BT/WiFi (GPIO4)
 *
 * Modes de fonctionnement :
 * - Switch BT  : BLE-MIDI, LED cligno rapide/lent
 * - Switch WiFi : rtpMIDI + serveur web
 *   - STA (reseau existant) : LED double flash, page web a servo-flute.local
 *   - AP  (hotspot)         : LED triple flash, page web a 192.168.4.1
 *   - Bouton long (3s)      : force AP
 *
 * Page web (mode WiFi) :
 * - Clavier virtuel 14 notes (touch + souris + raccourcis clavier)
 * - Lecteur de fichiers MIDI (upload drag&drop, play/pause/stop)
 * - Configuration instrument (lecture)
 * - Monitoring temps reel via WebSocket (CC, etat, heap)
 *
 * Auteur: Servo-Flute Project
 * Version: ESP32 1.1
 * Date: 2026-02-08
 ***********************************************************************************************/

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <esp_task_wdt.h>
#include <LittleFS.h>

#include "settings.h"
#include "ConfigStorage.h"
#include "EventQueue.h"
#include "FingerController.h"
#include "AirflowController.h"
#include "NoteSequencer.h"
#include "InstrumentManager.h"
#include "StatusLed.h"
#include "HardwareInputs.h"
#include "WirelessManager.h"

// Instances globales
InstrumentManager* instrument = nullptr;
StatusLed statusLed(STATUS_LED_PIN);
HardwareInputs inputs(PAIRING_BUTTON_PIN, MODE_SWITCH_PIN);
WirelessManager* wireless = nullptr;

/**
 * Etat sur en cas de crash/redemarrage
 * Initialise le hardware en configuration sure AVANT toute autre operation.
 */
void initSafeState() {
  // Initialiser I2C avec les pins ESP32
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // Creer une instance temporaire du PWM driver
  Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver();
  pwm.begin();
  pwm.setPWMFreq(SERVO_FREQUENCY);

  // Solenoide en etat sur (FERME)
  pinMode(SOLENOID_PIN, OUTPUT);
  digitalWrite(SOLENOID_PIN, LOW);

  // Servo airflow en position repos (utilise defaut avant chargement config)
  uint16_t pulseWidth = map(SERVO_AIRFLOW_OFF, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE, SERVO_PULSE_MIN, SERVO_PULSE_MAX);
  pwm.setPWM(DEFAULT_AIRFLOW_PCA_CHANNEL, 0, pulseWidth);

  // Tous les servos doigts en position fermee (utilise defauts)
  for (int i = 0; i < DEFAULT_NUM_FINGERS; i++) {
    pulseWidth = map(DEFAULT_FINGERS[i].closedAngle, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE, SERVO_PULSE_MIN, SERVO_PULSE_MAX);
    pwm.setPWM(DEFAULT_FINGERS[i].pcaChannel, 0, pulseWidth);
  }

  delay(SAFE_STATE_SETTLE_MS);
}

void setup() {
  // Forcer l'etat sur des le demarrage
  initSafeState();

  // Communication serie pour debug
  if (DEBUG) {
    Serial.begin(115200);
    delay(SERIAL_STARTUP_DELAY_MS);
    Serial.println();
    Serial.println("========================================");
    Serial.println("  SERVO FLUTE ESP32 - INITIALISATION");
    Serial.println("========================================");
  }

  // Initialiser LittleFS (pour stockage fichiers MIDI et config future)
  if (!LittleFS.begin(true)) {  // true = formater si premier usage
    if (DEBUG) {
      Serial.println("ERREUR: LittleFS - Echec initialisation!");
    }
  } else {
    if (DEBUG) {
      Serial.print("DEBUG: LittleFS - OK (");
      Serial.print(LittleFS.totalBytes() / 1024);
      Serial.print("KB total, ");
      Serial.print(LittleFS.usedBytes() / 1024);
      Serial.println("KB utilise)");
    }
  }

  // Charger la configuration depuis LittleFS (ou defauts si pas de fichier)
  ConfigStorage::load();

  if (DEBUG) {
    Serial.print("DEBUG: Config - Canal MIDI: ");
    Serial.println(cfg.midiChannel == 0 ? "Omni" : String(cfg.midiChannel).c_str());
    Serial.print("DEBUG: Config - Device: ");
    Serial.println(cfg.deviceName);
  }

  // Initialiser les entrees hardware (bouton + switch)
  inputs.begin();

  // Initialiser la LED d'etat
  statusLed.begin();
  statusLed.setPattern(LED_BLINK_FAST);  // Demarrage en cours

  // Creer l'instrument manager
  instrument = new InstrumentManager();
  instrument->begin();

  // Creer et initialiser le wireless manager
  // (inclut BLE/WiFi + serveur web + lecteur MIDI selon le mode)
  wireless = new WirelessManager(statusLed, inputs);
  wireless->begin(instrument);

  if (DEBUG) {
    Serial.println("========================================");
    Serial.println("   SYSTEME PRET");
    Serial.println("========================================");
    Serial.println();
    Serial.println("Configuration:");
    Serial.print("  - Notes jouables: ");
    Serial.print(cfg.numNotes);
    Serial.print(" (MIDI ");
    Serial.print(cfg.numNotes > 0 ? cfg.notes[0].midiNote : 0);
    Serial.print(" - ");
    Serial.print(cfg.numNotes > 0 ? cfg.notes[cfg.numNotes - 1].midiNote : 0);
    Serial.println(")");
    Serial.print("  - Servos doigts: ");
    Serial.println(cfg.numFingers);
    Serial.print("  - Delai servos->solenoide: ");
    Serial.print(SERVO_TO_SOLENOID_DELAY_MS);
    Serial.println(" ms");
    Serial.print("  - Mode: ");
    Serial.println(wireless->getStatusText());
    Serial.print("  - Watchdog: ");
    Serial.print(WATCHDOG_TIMEOUT_MS);
    Serial.println(" ms");
    Serial.print("  - Heap libre: ");
    Serial.print(ESP.getFreeHeap() / 1024);
    Serial.println(" KB");
    Serial.println();
  }

  // Configurer le watchdog ESP32 (Task WDT)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WATCHDOG_TIMEOUT_MS,
    .idle_core_mask = 0,       // Ne pas surveiller les taches idle
    .trigger_panic = true      // Redemarrage si timeout
  };
  esp_task_wdt_deinit();               // Reinitialiser si deja configure
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);              // Inscrire la tache courante (loopTask)

  if (DEBUG) {
    Serial.println("DEBUG: Watchdog ESP32 active");
  }
}

void loop() {
  // Reinitialiser le watchdog
  esp_task_wdt_reset();

  // Lire les entrees hardware
  inputs.update();

  // Mettre a jour le wireless (BLE/WiFi MIDI + web server + lecteur MIDI)
  wireless->update();

  // Mettre a jour l'instrument (state machine + power management)
  instrument->update();

  // Mettre a jour la LED d'etat
  statusLed.update();
}
