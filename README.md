# Servo Flute MIDI Boop

**Instrument robotique modulaire pilote par MIDI, base sur ESP32.**

Un projet open-source pour transformer n'importe quel instrument a vent en instrument robotique autonome. Le systeme est entierement configurable : nombre de servos, type de gestion d'air, doigtes personnalises — le tout modifiable sans recompiler via une interface web embarquee.

Flute a bec, tin whistle, clarinette, cornemuse, orgue... le firmware s'adapte a l'instrument.

## Architecture modulaire

Le systeme repose sur une configuration dynamique en JSON, persistee sur LittleFS :

- **Jusqu'a 31 servos doigts** — sur 2 PCA9685 (32 canaux dont 1 pour l'airflow), chaque servo configurable independamment (canal, angle, direction, trou de pouce)
- **Nombre de notes illimite en pratique** — chaque note definit son doigte (pattern ouvert/ferme/demi-trou) et sa plage d'airflow
- **6 modes de gestion d'air** — solenoide, servo-valve, ventilateur, pompe(s) avec reservoir... ([details](docs/AIR_MANAGEMENT.md))
- **Extension multi-PCA9685** — pour les instruments a nombreuses cles ([details](docs/PCA9685_EXPANSION.md))

Tous les parametres sont editables a chaud depuis l'interface web. Voir [CONFIGURATION.md](docs/CONFIGURATION.md).

## Reception MIDI

Le firmware supporte trois canaux de reception MIDI qui fonctionnent en parallele :

| Canal | Protocole | Usage typique |
|-------|-----------|---------------|
| **Bluetooth** | BLE-MIDI (NimBLE) | iOS, macOS, Windows, Android — sans fil, faible latence |
| **WiFi** | rtpMIDI (AppleMIDI) | DAW via reseau local, compatible Apple MIDI Network |
| **Serial** | MIDI DIN (UART 31250 baud) | Connection filaire classique, modules MIDI hardware |

### Selection du mode sans fil

Un **interrupteur physique sur GPIO4** bascule entre Bluetooth et WiFi :

| GPIO4 | Mode actif |
|-------|------------|
| LOW   | BLE-MIDI — l'ESP32 s'annonce comme peripherique Bluetooth |
| HIGH  | WiFi rtpMIDI — connexion au reseau WiFi configure (mode STA) ou creation d'un hotspot (mode AP) |

Le **MIDI Serial** (UART) est independant de cet interrupteur : il reste actif en permanence et peut etre active/desactive ainsi que configure (pin RX) depuis l'interface web.

Les trois canaux convergent vers le meme `InstrumentManager` — une note recue par n'importe quel canal produit le meme resultat.

Pour plus de details, voir [WIFI_MODES.md](docs/WIFI_MODES.md).

### Lecture de fichiers MIDI

Le systeme accepte egalement des fichiers `.mid` (SMF Type 0 et 1) uploades via l'interface web pour une lecture autonome sans source MIDI externe.

## Interface web embarquee

Serveur web SPA accessible en WiFi avec 6 onglets :

| Onglet | Fonction |
|--------|----------|
| **Clavier** | Notes interactives avec representation visuelle (touch, souris, clavier AZERTY) |
| **MIDI** | Upload et lecture de fichiers .mid |
| **Calibration** | Test temps reel des servos, assistant doigts, sweep airflow, auto-calibration micro |
| **Config** | Tous les parametres editables, table des doigtes visuelle, sauvegarde persistante |
| **WiFi** | Scan reseaux, connexion depuis le hotspot |
| **Monitor** | Barres CC, heap memoire, journal en direct (WebSocket) |

## Auto-calibration (optionnel)

Avec un micro **INMP441** (I2S), le systeme peut automatiquement calibrer les positions d'airflow pour chaque note en detectant le son produit. Voir [CALIBRATION.md](docs/CALIBRATION.md).

## Materiel requis

- **ESP32-WROOM-32E** (DevKit v1 ou equivalent)
- **PCA9685** module I2C PWM 16 canaux
- **Servos SG90** — nombre selon l'instrument (doigts + airflow)
- **Systeme d'air** — solenoide, servo-valve, ventilateur ou pompe selon le mode choisi ([details](docs/AIR_MANAGEMENT.md))
- **Alimentation 5V** dimensionnee selon le nombre de servos
- **INMP441** micro MEMS I2S (optionnel, pour auto-calibration)

Le brochage complet et les instructions de montage sont dans le [README du firmware](Servo_flute_ESP32/README.md).

## Structure du projet

```
Servo-Flute-Midi-Boop/
  README.md                  <- Ce fichier
  docs/                      <- Documentation technique
    ARCHITECTURE.md           - Architecture modulaire et flux de donnees
    AIR_MANAGEMENT.md         - Systeme de gestion d'air (6 modes)
    API_WEB.md                - Reference API REST et WebSocket
    CALIBRATION.md            - Guide de calibration et auto-calibration
    CONFIGURATION.md          - Reference complete des parametres
    PCA9685_EXPANSION.md      - Etude d'extension multi-PCA9685
    WIFI_MODES.md             - Modes sans fil (BT, STA, AP)
  Servo_flute_ESP32/          <- Code source firmware Arduino
    Servo_flute_ESP32.ino      - Point d'entree (setup/loop)
    settings.h                 - Defines hardware et valeurs par defaut
    ...                        - Modules (voir ARCHITECTURE.md)
```

## Demarrage rapide

1. Installer les dependances Arduino (voir [README firmware](Servo_flute_ESP32/README.md#dependances-arduino))
2. Flasher le firmware sur l'ESP32
3. Au premier boot, le hotspot WiFi `ServoFlute-Setup` demarre
4. Se connecter au hotspot, ouvrir `192.168.4.1`
5. Configurer le nombre de doigts, les doigtes, le mode d'air, calibrer les servos
6. Connecter une source MIDI (BLE, WiFi ou Serial) et jouer

## Documentation

| Document | Description |
|----------|-------------|
| [Architecture](docs/ARCHITECTURE.md) | Diagramme des modules, flux de donnees, machine d'etats |
| [Gestion d'air](docs/AIR_MANAGEMENT.md) | Les 6 modes d'air, capteurs, pompes, ventilateurs |
| [API Web](docs/API_WEB.md) | Endpoints REST et protocole WebSocket |
| [Calibration](docs/CALIBRATION.md) | Guide pas-a-pas, auto-calibration micro |
| [Configuration](docs/CONFIGURATION.md) | Tous les parametres configurables |
| [Extension PCA9685](docs/PCA9685_EXPANSION.md) | Support multi-cartes pour instruments a nombreuses cles |
| [Modes WiFi](docs/WIFI_MODES.md) | BLE-MIDI, WiFi STA, hotspot AP, securite |
| [Firmware](Servo_flute_ESP32/README.md) | Brochage, materiel, dependances, structure du code |

## Licence

MIT
