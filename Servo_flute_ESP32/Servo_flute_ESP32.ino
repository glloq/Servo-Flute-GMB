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
#include <esp_idf_version.h>
#include <LittleFS.h>
#include <new>   // std::nothrow : une allocation ratee doit rendre nullptr, pas abandonner

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
#include "DeviceSecrets.h"
#include "gmb/GmbRuntime.h"

// Instances globales
InstrumentManager* instrument = nullptr;
StatusLed statusLed(STATUS_LED_PIN);
HardwareInputs inputs(PAIRING_BUTTON_PIN, MODE_SWITCH_PIN);
WirelessManager* wireless = nullptr;
bool g_actuatorsEnabled = false;

bool initializeWatchdog() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WATCHDOG_TIMEOUT_MS,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_deinit();
  esp_err_t initErr = esp_task_wdt_init(&wdt_config);
#else
  esp_task_wdt_deinit();
  esp_err_t initErr = esp_task_wdt_init(WATCHDOG_TIMEOUT_MS / 1000, true);
#endif
  if (initErr != ESP_OK && initErr != ESP_ERR_INVALID_STATE) return false;
  esp_err_t addErr = esp_task_wdt_add(NULL);
  return addErr == ESP_OK || addErr == ESP_ERR_INVALID_STATE;
}

/**
 * Etat sur en cas de crash/redemarrage
 * Initialise le hardware en configuration sure AVANT toute autre operation.
 */
void initSafeState() {
  // Keep PCA9685 outputs disabled before LittleFS/configuration is available.
  pinMode(PIN_SERVOS_OFF, OUTPUT);
  digitalWrite(PIN_SERVOS_OFF, HIGH);  // OE HIGH = outputs disabled

  // Put known fixed boot-critical GPIOs in their safest electrical state only.
  pinMode(SOLENOID_PIN, OUTPUT);
  digitalWrite(SOLENOID_PIN, SOLENOID_ACTIVE_HIGH ? LOW : HIGH);

  // I2C is initialized here only for later hardware probing; no servo pulse is
  // emitted until ConfigStorage::load() and validation have completed.
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
}

void setup() {
  // Forcer l'etat sur des le demarrage
  initSafeState();

  // Port serie TOUJOURS ouvert. Il ne sert pas qu'au debug : c'est le seul canal
  // par lequel l'appareil communique ses secrets d'acces (cle WPA2 du hotspot et
  // mot de passe web, affiches au premier demarrage et apres une regeneration par
  // le bouton BOOT). Quand Serial.begin() etait conditionne par DEBUG, une
  // compilation avec DEBUG=0 rendait ces impressions muettes : l'appareil etait
  // alors inaccessible, sans aucun moyen de recuperer le mot de passe. Seuls les
  // journaux verbeux restent conditionnes par DEBUG.
  Serial.begin(115200);
  delay(SERIAL_STARTUP_DELAY_MS);
  if (DEBUG) {
    Serial.println();
    Serial.println("========================================");
    Serial.println("  SERVO FLUTE ESP32 - INITIALISATION");
    Serial.println("========================================");
  }

  // Monter LittleFS SANS formatage automatique (fail-safe, voir ConfigStorage.h).
  // Un LittleFS.begin(true) reformaterait la partition au premier echec de
  // montage : /config.json et les fichiers MIDI seraient perdus en silence et
  // l'instrument repartirait sur une configuration par defaut qui ne correspond
  // pas forcement au cablage reel. Un echec laisse donc le firmware en mode
  // recovery, actionneurs interdits, jusqu'a une action volontaire.
  bool fsMounted = ConfigStorage::beginFilesystem();
  if (DEBUG) {
    if (fsMounted) {
      Serial.print("DEBUG: LittleFS - OK (");
      Serial.print(LittleFS.totalBytes() / 1024);
      Serial.print("KB total, ");
      Serial.print(LittleFS.usedBytes() / 1024);
      Serial.println("KB utilise)");
    } else {
      Serial.println("ERREUR: LittleFS - montage impossible, MODE RECOVERY (aucun actionneur)");
    }
  }

  // Charger et valider la configuration depuis LittleFS avant tout mouvement servo.
  // La decision "cette configuration autorise-t-elle les actionneurs" vit dans
  // bootConfigMayDriveActuators() (ConfigStorage.h) pour etre testable sur hote :
  // en resume, seule une configuration que l'utilisateur a reellement ecrite
  // autorise a piloter, et les valeurs d'usine ne le font PAS.
  ConfigLoadStatus configStatus = ConfigStorage::loadWithStatus();
  ConfigValidationResult bootValidation = validateAndNormalizeConfig(cfg);
  bool bootConfigSafe = fsMounted &&
                        bootConfigMayDriveActuators(fsMounted, configStatus, bootValidation.valid);
  if (!bootConfigSafe) {
    digitalWrite(PIN_SERVOS_OFF, HIGH);
    // Message TOUJOURS imprime, pas seulement sous DEBUG : c'est la seule
    // explication que recoit l'utilisateur d'un appareil qui refuse de bouger, et
    // le port serie est de toute facon toujours ouvert (voir plus haut). Sans
    // elle, un instrument neuf est indiscernable d'un instrument casse.
    Serial.println("ATTENTION: actionneurs DESACTIVES (mode recovery).");
    if (!fsMounted) {
      Serial.println("  Cause: LittleFS non monte.");
      Serial.println("  Action: passer l'interrupteur en mode WiFi, ouvrir la page web,");
      Serial.println("          puis formater LittleFS depuis les diagnostics (destructif).");
    } else {
      switch (configStatus) {
        case CONFIG_DEFAULTS:
          Serial.println("  Cause: aucune configuration enregistree (/config.json absent).");
          Serial.println("  Les valeurs d'usine ne decrivent pas forcement le materiel branche :");
          Serial.println("  les piloter reviendrait a lancer sept servos vers des angles qui");
          Serial.println("  peuvent etre hors de la course reelle de cette mecanique.");
          Serial.println("  Action: passer l'interrupteur en mode WiFi, ouvrir la page web et");
          Serial.println("          terminer l'assistant de configuration. Le redemarrage qui");
          Serial.println("          suit rend les actionneurs.");
          break;
        case CONFIG_INVALID_FALLBACK:
        case CONFIG_STORAGE_ERROR:
        case CONFIG_LOADED:
          Serial.print("  Cause: configuration de demarrage refusee: ");
          Serial.println(ConfigStorage::lastLoadError().length() ? ConfigStorage::lastLoadError() : bootValidation.error);
          Serial.println("  Action: passer l'interrupteur en mode WiFi, ouvrir la page web et");
          Serial.println("          corriger la configuration (modifiable en recovery).");
          break;
      }
    }
  }

  if (DEBUG) {
    Serial.print("DEBUG: Config - Canal MIDI: ");
    Serial.println(cfg.midiChannel == 0 ? "Omni" : String(cfg.midiChannel).c_str());
    Serial.print("DEBUG: Config - Device: ");
    Serial.println(cfg.deviceName);
  }

  // Construire l'instantane de capacites General-Midi-Boop a partir de la
  // configuration ACTIVE et validee, puis le descripteur JSON mis en cache.
  // A faire avant WirelessManager::begin() : les transports MIDI s'enregistrent
  // comme ports GMB et peuvent recevoir une requete des la connexion.
  gmb::runtime::begin(bootConfigSafe);

  // Secrets d'acces (cle WPA2 du hotspot + mot de passe de l'interface web).
  // Generes aleatoirement au premier demarrage et conserves en NVS : ils ne
  // derivent ni du MAC ni du BSSID, qui ne sont pas des secrets.
  DeviceSecrets::begin();

  // Initialiser les entrees hardware (bouton + switch)
  inputs.begin();

  // Recuperation par presence physique : maintenir BOOT au demarrage regenere
  // les secrets d'acces et les affiche sur le port serie. Sans cela, un appareil
  // headless dont on a perdu le mot de passe serait definitivement inaccessible.
  if (digitalRead(PAIRING_BUTTON_PIN) == LOW) {
    unsigned long holdStart = millis();
    bool held = true;
    while (held && (millis() - holdStart) < SECRET_RESET_HOLD_MS) {
      held = (digitalRead(PAIRING_BUTTON_PIN) == LOW);
      delay(20);
    }
    if (held) {
      DeviceSecrets::regenerateApPassword();
      DeviceSecrets::regenerateAdminPassword();
      Serial.println("SECURITE: secrets d'acces regeneres (appui long sur BOOT au demarrage)");
    }
  }
  DeviceSecrets::printToSerial();

  // Initialiser la LED d'etat
  statusLed.begin();
  statusLed.setPattern(LED_BLINK_FAST);  // Demarrage en cours

  // Creer l'instrument manager seulement si le systeme de fichiers est monte ET
  // que la configuration chargee est valide : sinon l'OE des PCA9685 reste HAUT
  // et aucun actionneur ne peut etre pilote, quel que soit le chemin (web, MIDI,
  // calibration). Voir InstrumentManager::applyCommand().
  //
  // `std::nothrow` : un `new` ordinaire qui echoue sur ESP32 ne rend pas nullptr,
  // il abandonne et la carte redemarre - donc, au demarrage, elle reboucle. Le
  // mode "instrument absent" existe deja et est teste (loop() teste
  // `if (instrument)`, le serveur web repond hardware_not_ready) : une panne de
  // tas y tombe desormais au lieu de produire une carte qui parait morte.
  if (bootConfigSafe) {
    instrument = new (std::nothrow) InstrumentManager();
    g_actuatorsEnabled = (instrument != nullptr) && instrument->beginSafe();
    if (instrument == nullptr && DEBUG) {
      Serial.println("ERREUR: tas insuffisant pour InstrumentManager - actionneurs desactives");
    }
  } else {
    instrument = nullptr;
    g_actuatorsEnabled = false;
  }

  // Creer et initialiser le wireless manager
  // (inclut BLE/WiFi + serveur web + lecteur MIDI selon le mode)
  wireless = new (std::nothrow) WirelessManager(statusLed, inputs);
  if (wireless != nullptr) {
    wireless->begin(instrument);
  } else if (DEBUG) {
    Serial.println("ERREUR: tas insuffisant pour WirelessManager - aucun transport MIDI");
  }

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
    Serial.println(wireless ? wireless->getStatusText() : "INDISPONIBLE (tas insuffisant)");
    Serial.print("  - Watchdog: ");
    Serial.print(WATCHDOG_TIMEOUT_MS);
    Serial.println(" ms");
    Serial.print("  - Heap libre: ");
    Serial.print(ESP.getFreeHeap() / 1024);
    Serial.println(" KB");
    Serial.print("  - GMB instance_id: 0x");
    Serial.println(gmb::runtime::instanceId(), HEX);
    Serial.print("  - GMB revision: ");
    Serial.println(gmb::runtime::revision());
    Serial.println();
  }

  bool watchdogOk = initializeWatchdog();
  if (DEBUG) {
    Serial.println(watchdogOk ? "DEBUG: Watchdog ESP32 active" : "ERREUR: Watchdog ESP32 non initialise");
  }
}

void loop() {
  // Reinitialiser le watchdog
  esp_task_wdt_reset();

  // Lire les entrees hardware
  inputs.update();

  // Mettre a jour le wireless (BLE/WiFi MIDI + web server + lecteur MIDI)
  if (wireless) wireless->update();

  // Mettre a jour l'instrument (state machine + power management)
  if (instrument) instrument->update();

  // Mettre a jour la LED d'etat
  statusLed.update();
}
