# Servo Flute ESP32

**Version ESP32-WROOM de la flute robotique avec connectivite sans fil et interface web**

## Vue d'ensemble

Portage de la Servo Flute v3 (Arduino Leonardo) vers l'ESP32-WROOM-32E, ajoutant le BLE-MIDI, le WiFi rtpMIDI, un serveur web embarque et la configuration persistante.

### Differences avec la v3 (Arduino Leonardo)

| Fonctionnalite | v3 (Leonardo) | ESP32 |
|---|---|---|
| Connexion MIDI | USB filaire | BLE-MIDI + WiFi rtpMIDI |
| Configuration | `#define` (recompilation) | JSON sur LittleFS (web) |
| Interface | Serial Monitor | Page web SPA 6 onglets |
| Calibration | Serial Monitor interactif | Interface web temps reel + auto-calibration micro |
| WiFi | Non | STA + AP hotspot |
| Playback MIDI | Non | Upload + lecture fichier .mid |
| Auto-calibration | Non | INMP441 I2S (optionnel) |

## Materiel requis

### Electronique
- **ESP32-WROOM-32E** (DevKit v1 ou equivalent)
- **PCA9685** module I2C PWM 16 canaux
- **6x servos SG90** (doigts, engrenages metalliques recommandes)
- **1x servo SG90** (debit d'air)
- **1x solenoide 5V** (valve on/off, ~300-500mA)
- **MOSFET** de commutation (2N7000 ou IRLZ44N selon courant)
- **Diode de roue libre** (1N4007)
- **Alimentation 5V 5A** minimum
- **Interrupteur** BT/WiFi (optionnel, GPIO4)
- **INMP441** micro MEMS I2S (optionnel, pour auto-calibration)

### Brochage ESP32

| GPIO | Fonction |
|------|----------|
| 2 | LED d'etat (integree) |
| 0 | Bouton appairage (BOOT) |
| 4 | Switch BT/WiFi |
| 13 | Solenoide (PWM) |
| 5 | PCA9685 OE (power servos) |
| 21 | I2C SDA (PCA9685) |
| 22 | I2C SCL (PCA9685) |
| 14 | INMP441 BCLK (optionnel) |
| 15 | INMP441 LRCLK/WS (optionnel) |
| 32 | INMP441 DIN/SD (optionnel) |

### Canaux PCA9685

| Canal | Fonction |
|-------|----------|
| 0-5 | Servos doigts (trou 1-6) |
| 10 | Servo airflow |

## Modes de fonctionnement

### Mode Bluetooth (switch LOW)
- BLE-MIDI via NimBLE
- Compatible iOS, macOS, Windows, Android
- LED rapide = advertising, LED lente = connecte
- Bouton court = restart advertising

### Mode WiFi (switch HIGH)
- **STA** (reseau existant) : rtpMIDI + web a `servo-flute.local`
- **AP** (hotspot) : WiFi `ServoFlute-Setup`, web a `192.168.4.1`
- LED double flash = STA, triple flash = AP
- Bouton long (3s) = forcer hotspot

## Interface web

Accessible en mode WiFi, 6 onglets :

1. **Clavier** - Notes dynamiques avec representation flute, touch/souris/clavier AZERTY
2. **MIDI** - Upload et lecture de fichiers .mid (SMF Type 0/1)
3. **Calibration** - Test temps reel servos, assistant doigts, sweep airflow, auto-calibration micro
4. **Config** - Tous les parametres editables, table des doigtes visuelle, sauvegarde persistante
5. **WiFi** - Scan reseaux, connexion depuis le hotspot
6. **Monitor** - CC bars, heap, journal en direct (WebSocket)

## Configuration

Tous les parametres sont modifiables a chaud via l'interface web et persistes en JSON sur LittleFS (`/config.json`).

Les valeurs par defaut sont definies dans `settings.h` et chargees au premier demarrage.

Voir [docs/CONFIGURATION.md](docs/CONFIGURATION.md) pour la liste complete des parametres.

## API REST

| Methode | Endpoint | Description |
|---------|----------|-------------|
| GET | `/` | Page web SPA |
| GET | `/api/status` | Etat JSON (CC, player, mode) |
| GET | `/api/config` | Configuration complete |
| POST | `/api/config` | Modifier parametres (JSON partiel) |
| POST | `/api/config/reset` | Reset aux defauts |
| POST | `/api/midi` | Upload fichier MIDI (multipart) |
| GET | `/api/wifi/scan` | Lancer scan WiFi async |
| GET | `/api/wifi/results` | Resultats du scan |
| POST | `/api/wifi/connect` | Connexion reseau (JSON `{ssid,pass}`) |
| GET | `/api/wifi/status` | Etat WiFi (IP, RSSI, mode) |

## Protocole WebSocket (`/ws`)

Messages JSON Client -> Serveur :

```json
{"t":"non","n":82,"v":100}     // Note On
{"t":"nof","n":82}              // Note Off
{"t":"cc","c":7,"v":100}        // Control Change
{"t":"velocity","v":100}        // Velocity par defaut
{"t":"play"}                    // Play fichier MIDI
{"t":"pause"}                   // Pause
{"t":"stop"}                    // Stop
{"t":"panic"}                   // All Sound Off
{"t":"test_finger","i":0,"a":90}  // Test servo doigt
{"t":"test_air","a":60}           // Test servo airflow
{"t":"test_sol","o":1}             // Test solenoide
{"t":"test_note","n":84}           // Test position note complete
{"t":"mic_mon","on":true}          // Activer/desactiver monitoring micro
{"t":"auto_cal","mode":"air"}      // Demarrer auto-calibration airflow
{"t":"auto_cal","mode":"stop"}     // Arreter auto-calibration
```

Messages JSON Serveur -> Client (micro INMP441, si detecte) :

```json
{"t":"audio","rms":0.12,"hz":440,"midi":69,"cents":-5}  // Monitoring audio temps reel
{"t":"acal_prog","idx":3,"note":"C6","total":14,"st":3}  // Progression auto-calibration
{"t":"acal_done","ok":true,"results":[...]}               // Resultats auto-calibration
```

## Dependances Arduino

### Board package

Dans Arduino IDE : **Fichier** > **Preferences** > **URL de gestionnaire de cartes supplementaires** :
```
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```
Puis **Outils** > **Type de carte** > **Gestionnaire de cartes** > installer **esp32** par Espressif.

### Bibliotheques

Installer via **Croquis** > **Inclure une bibliotheque** > **Gerer les bibliotheques** :

| Bibliotheque | Auteur | Installation |
|---|---|---|
| NimBLE-Arduino | h2zero | Gestionnaire de bibliotheques |
| BLE-MIDI | lathoub | Gestionnaire de bibliotheques |
| AppleMIDI | lathoub | Gestionnaire de bibliotheques |
| MIDI Library | FortySevenEffects | Gestionnaire de bibliotheques |
| Adafruit PWM Servo Driver | Adafruit | Gestionnaire de bibliotheques |
| ArduinoJson | Benoit Blanchon | Gestionnaire de bibliotheques |
| AsyncTCP | dvarrel (ou me-no-dev) | ZIP GitHub* |
| ESPAsyncWebServer | dvarrel (ou me-no-dev) | ZIP GitHub* |

> **Important** : NimBLE-Arduino doit etre installe **avant** BLE-MIDI.
> BLE-MIDI utilise NimBLE comme stack BLE sur ESP32.

**LittleFS** et **ESPmDNS** sont integres au board package ESP32 (rien a installer).

*Les bibliotheques AsyncTCP et ESPAsyncWebServer ne sont pas dans le gestionnaire standard.
Installation manuelle :
1. Telecharger le ZIP depuis GitHub (ex: https://github.com/dvarrel/AsyncTCP et https://github.com/dvarrel/ESPAsyncWebSrv)
2. **Croquis** > **Inclure une bibliotheque** > **Ajouter une bibliotheque .ZIP**
3. Selectionner le fichier ZIP telecharge

### Configuration carte

- **Board** : ESP32 Dev Module (ou ESP32-WROOM-DA Module)
- **Partition Scheme** : Default 4MB with spiffs (ou "Default 4MB with ffat" si disponible)
- **Flash Size** : 4MB
- **Upload Speed** : 921600

## Structure des fichiers

```
Servo_flute_ESP32/
  Servo_flute_ESP32.ino  - Point d'entree, setup/loop
  settings.h             - Configuration hardware et defines
  ConfigStorage.h/.cpp   - Config persistante JSON (LittleFS)
  FingerController.h/.cpp - Controle 6 servos doigts (PCA9685)
  AirflowController.h/.cpp - Servo airflow + solenoide + vibrato + CC2
  NoteSequencer.h/.cpp   - Machine d'etat note (positioning/playing/stopping)
  InstrumentManager.h/.cpp - Orchestrateur instrument + power management
  EventQueue.h/.cpp      - File d'evenements temporises
  BleMidiHandler.h/.cpp  - BLE-MIDI via NimBLE
  WifiMidiHandler.h/.cpp - WiFi rtpMIDI + scan WiFi
  WirelessManager.h/.cpp - Orchestrateur modes BT/WiFi/AP
  WebConfigurator.h/.cpp - Serveur web + WebSocket + API REST
  MidiFilePlayer.h/.cpp  - Parser et lecteur SMF (Type 0/1)
  AudioAnalyzer.h/.cpp   - Driver I2S INMP441 + detection RMS + pitch YIN
  AutoCalibrator.h/.cpp  - Machine d'etat auto-calibration airflow
  StatusLed.h/.cpp       - Patterns LED d'etat
  HardwareInputs.h/.cpp  - Bouton + switch avec debounce
  web_content.h          - HTML/CSS/JS embarque en PROGMEM
  docs/                  - Documentation technique
```

## Premiere utilisation

1. Flasher le firmware sur l'ESP32
2. Au premier boot, LittleFS est formate et la config par defaut est creee
3. Le mode WiFi demarre en hotspot (`ServoFlute-Setup`)
4. Se connecter au hotspot depuis un telephone
5. Ouvrir `192.168.4.1` dans le navigateur
6. Onglet **WiFi** : scanner et se connecter a votre reseau
7. Onglet **Calibration** : regler les positions de chaque servo doigt
8. Onglet **Config** : ajuster les parametres et sauvegarder

## Licence

MIT - Voir [LICENSE](../LICENSE) dans le repertoire racine.
