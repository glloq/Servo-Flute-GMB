# Servo Flute MIDI Boop

**Flute robotique pilotee par MIDI, basee sur ESP32 avec connectivite sans fil et interface web embarquee.**

Un projet open-source qui transforme une flute a bec en instrument robotique autonome : des servos controlent les doigts et le souffle, le tout pilotable via BLE-MIDI, WiFi rtpMIDI ou une interface web complete.

## Capacites

### Controle instrumental
- **6 servos doigts** via PCA9685 (I2C PWM 16 canaux) pour les doigtes
- **Servo airflow** pour le controle continu du debit d'air
- **Solenoide 5V** pour l'ouverture/fermeture rapide de la valve
- **Vibrato automatique** configurable (amplitude, vitesse)
- **Breath controller** (CC2) avec courbe de reponse ajustable
- **6 modes de gestion d'air** : solenoide classique, servo-valve, ventilateur, pompe(s) avec reservoir... ([details](docs/AIR_MANAGEMENT.md))

### Connectivite MIDI
- **BLE-MIDI** via NimBLE (compatible iOS, macOS, Windows, Android)
- **WiFi rtpMIDI** (compatible Apple MIDI, protocole standard)
- **Basculement BT/WiFi** par interrupteur physique (GPIO4)
- **Lecture de fichiers MIDI** (.mid SMF Type 0 et 1) uploades via le web

### Interface web embarquee
Serveur web SPA accessible en WiFi avec 6 onglets :

| Onglet | Fonction |
|--------|----------|
| **Clavier** | Notes interactives avec representation visuelle de la flute (touch, souris, clavier AZERTY) |
| **MIDI** | Upload et lecture de fichiers .mid |
| **Calibration** | Test temps reel des servos, assistant doigts, sweep airflow, auto-calibration micro |
| **Config** | Tous les parametres editables, table des doigtes visuelle, sauvegarde persistante |
| **WiFi** | Scan reseaux, connexion depuis le hotspot |
| **Monitor** | Barres CC, heap memoire, journal en direct (WebSocket) |

### Auto-calibration (optionnel)
Avec un micro **INMP441** (I2S), le systeme peut automatiquement calibrer les positions d'airflow pour chaque note en detectant le son produit. Voir [CALIBRATION.md](docs/CALIBRATION.md).

### Configuration persistante
Tous les parametres sont modifiables a chaud via l'interface web et sauvegardes en JSON sur LittleFS. Pas besoin de recompiler pour ajuster le comportement. Voir [CONFIGURATION.md](docs/CONFIGURATION.md).

## Materiel requis

- **ESP32-WROOM-32E** (DevKit v1 ou equivalent)
- **PCA9685** module I2C PWM 16 canaux
- **6x servos SG90** (doigts) + **1x servo SG90** (airflow)
- **1x solenoide 5V** + MOSFET + diode de roue libre
- **Alimentation 5V 5A** minimum
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
5. Configurer le WiFi, calibrer les servos, jouer !

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
