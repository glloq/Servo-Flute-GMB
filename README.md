# Servo Flute MIDI Boop

**Instrument robotique modulaire pilote par MIDI, base sur ESP32.**

Transforme une flute, un tin whistle, un ocarina ou toute flute a souffle simple en instrument robotique autonome. Le firmware s'adapte a l'instrument : nombre de doigts, type de gestion d'air, doigtes personnalises — tout est configurable a chaud depuis une interface web, sans recompiler.

> **Perimetre :** Instruments a souffle simple (flute a bec, tin whistle, NAF, ocarina...). Les instruments a anche ou a buzzing (clarinette, trompette...) sont hors perimetre.

## Fonctionnalites

- **Jusqu'a 31 servos** sur 2 cartes PCA9685, chacun configurable independamment
- **3 entrees MIDI simultanees** — Bluetooth (BLE-MIDI), WiFi (rtpMIDI), Serial (UART)
- **6 modes de gestion d'air** — solenoide, servo-valve, ventilateur, pompe...
- **Interface web** — clavier virtuel, lecteur MIDI, calibration, gestion d'air + page reglages
- **Auto-calibration** optionnelle par micro INMP441 (I2S)
- **Lecture de fichiers MIDI** (.mid SMF Type 0/1) sans source externe
- **Configuration persistante** en JSON sur LittleFS, editable a chaud

## Materiel

| Composant | Role |
|-----------|------|
| ESP32-WROOM-32E | Microcontroleur principal |
| PCA9685 (x1 ou x2) | PWM 16 canaux pour servos |
| Servos SG90 | Doigts + airflow |
| Systeme d'air | Solenoide, servo-valve, ventilateur ou pompe |
| Alimentation 5V | Dimensionnee selon le nombre de servos |
| INMP441 (optionnel) | Micro I2S pour auto-calibration |

Brochage complet : [Configuration](docs/CONFIGURATION.md)

## Demarrage rapide

1. Installer les dependances Arduino (voir `Servo_flute_ESP32/settings.h`)
2. Flasher le firmware sur l'ESP32
3. Au premier boot, le hotspot WiFi `ServoFlute-Setup` demarre
4. Se connecter au hotspot — la page de configuration s'ouvre automatiquement (captive portal)
5. Configurer le nombre de doigts, les doigtes, le mode d'air
6. Calibrer les servos (onglet Calibration)
7. Connecter une source MIDI (BLE, WiFi ou Serial) et jouer

## Commandes physiques

### Interrupteur BT/WiFi (GPIO4)

| Position | Mode |
|----------|------|
| LOW | Bluetooth (BLE-MIDI) |
| HIGH | WiFi (rtpMIDI + interface web) |

### Bouton BOOT (GPIO0)

| Action | Effet |
|--------|-------|
| Appui court | BLE : restart advertising / WiFi : affiche l'IP |
| Double appui (< 500ms) | Ouvre tous les doigts (quel que soit le mode) |
| Appui long (3s) | WiFi : force le mode hotspot (AP) |

## Reception MIDI

Les trois canaux fonctionnent en parallele et convergent vers le meme `InstrumentManager` :

| Canal | Protocole | Usage |
|-------|-----------|-------|
| Bluetooth | BLE-MIDI (NimBLE) | iOS, macOS, Windows, Android |
| WiFi | rtpMIDI (AppleMIDI) | DAW via reseau local |
| Serial | MIDI DIN (UART 31250 baud) | Modules MIDI hardware |

Details : [WIFI_MODES.md](docs/WIFI_MODES.md)

## Interface web

Accessible en mode WiFi — application web SPA avec 4 onglets et une page reglages :

| Section | Fonction |
|---------|----------|
| **Clavier** | Notes interactives (touch, souris, clavier AZERTY) |
| **MIDI** | Upload et lecture de fichiers .mid |
| **Air** | Controle du systeme d'air, monitoring temps reel |
| **Calibration** | Test servos, assistant doigts, sweep airflow, auto-calibration |
| **Reglages** (engrenage) | Parametres, table des doigtes, WiFi, sauvegarde persistante |

API REST et WebSocket : [API_WEB.md](docs/API_WEB.md)

## Documentation

| Document | Description |
|----------|-------------|
| [Architecture](docs/ARCHITECTURE.md) | Modules, flux de donnees, machine d'etats |
| [Gestion d'air](docs/AIR_MANAGEMENT.md) | Les 6 modes d'air, capteurs, pompes, ventilateurs |
| [API Web](docs/API_WEB.md) | Endpoints REST et protocole WebSocket |
| [Auto-calibration](docs/AUTO_CALIBRATION.md) | Calibration automatique avec micro INMP441 |
| [Calibration](docs/CALIBRATION.md) | Guide de calibration manuelle pas-a-pas |
| [Configuration](docs/CONFIGURATION.md) | Tous les parametres configurables |
| [Extension PCA9685](docs/PCA9685_EXPANSION.md) | Support multi-cartes (jusqu'a 31 servos) |
| [Modes WiFi](docs/WIFI_MODES.md) | BLE-MIDI, WiFi STA, hotspot AP |
| [MIDI Serial](docs/MIDI_SERIAL.md) | Branchement MIDI DIN (optocoupler, GPIO) |
| [Servo Angle](docs/SERVO_ANGLE.md) | Montage et calibration servos, flute traversiere |

## Structure du projet

```
Servo-Flute-Midi-Boop/
  README.md                   <- Ce fichier
  docs/                       <- Documentation technique
  Servo_flute_ESP32/           <- Firmware Arduino (ESP32)
    Servo_flute_ESP32.ino       - Point d'entree (setup/loop)
    settings.h                  - Defines hardware et valeurs par defaut
    ...                         - Modules (voir ARCHITECTURE.md)
```

## Licence

MIT
