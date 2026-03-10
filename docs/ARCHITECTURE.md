# Architecture ESP32

## Diagramme des modules

```
                        Servo_flute_ESP32.ino
                               |
                    +----------+----------+
                    |                     |
             InstrumentManager      WirelessManager
              |     |     |          |    |    |    |
   FingerCtrl | AirflowCtrl |    BleMidi | WifiMidi |
              |             |           |          |
         NoteSequencer  EventQueue   WebConfigurator
                                     |    |    |
                              MidiFilePlayer | ConfigStorage
                                             |
                                    [si MIC_ENABLED]
                                    AudioAnalyzer
                                         |
                                   AutoCalibrator
```

## Flux de donnees

### Reception MIDI
```
BLE-MIDI ou rtpMIDI
  -> BleMidiHandler / WifiMidiHandler
    -> isChannelAccepted(channel)
    -> InstrumentManager.noteOn(note, velocity)
      -> NoteSequencer.playNote(note, velocity)
        -> FingerController.setFingerPatternForNote(note)
        -> EventQueue.schedule(solenoid open, delay)
        -> AirflowController.setAirflowForNote(note, velocity)
```

### Clavier virtuel web
```
Navigateur (touch/clic/clavier)
  -> WebSocket {"t":"non","n":82,"v":100}
    -> WebConfigurator.processWsMessage()
      -> InstrumentManager.noteOn(82, 100)
        -> (meme chaine que MIDI)
```

### Configuration
```
Page web Config -> POST /api/config (JSON partiel)
  -> WebConfigurator.handleApiConfigPost()
    -> Mise a jour RuntimeConfig cfg
    -> ConfigStorage::save() -> /config.json (LittleFS)

Au demarrage:
  LittleFS.begin()
  -> ConfigStorage::load() -> lit /config.json -> surcharge cfg
```

## Machine d'etat NoteSequencer

```
IDLE --> POSITIONING --> PLAYING --> STOPPING --> IDLE
  |         |              |            |
  |   setFingerPattern  openSolenoid  closeSolenoid
  |   setAirflow        (via delay)   setAirflowToRest
  |
  +-- noteOn() declenche la transition
```

- **IDLE** : aucune note en cours
- **POSITIONING** : servos en mouvement, attente `servoToSolenoidDelayMs`
- **PLAYING** : solenoide ouvert, note produite
- **STOPPING** : solenoide ferme, retour au repos

## Gestion de l'alimentation

InstrumentManager surveille `_lastActivityTime` :
- Apres `cfg.timeUnpower` ms sans activite -> `powerOffServos()` (PCA9685 OE HIGH)
- A la prochaine note -> `powerOnServos()` (PCA9685 OE LOW)

## WebSocket broadcast

Toutes les 500ms (`WS_STATUS_INTERVAL_MS`), WebConfigurator envoie a tous les clients :
```json
{
  "t": "status",
  "playing": true,
  "state": 2,
  "cc7": 127, "cc11": 127, "cc1": 0, "cc2": 127,
  "ps": 1, "pp": 45.2, "ppos": 12500,
  "heap": 185000
}
```

## Audio et auto-calibration (optionnel)

Si `MIC_ENABLED` est `true` dans `settings.h` et qu'un micro INMP441 est detecte au demarrage :

### AudioAnalyzer

- Driver I2S en mode `I2S_MODE_MASTER | I2S_MODE_RX`, 16kHz, 32-bit, canal gauche
- Detection de presence du micro : lecture de 256 samples, >10% non-zero = micro present
- Analyse a ~25Hz (toutes les 40ms) : calcul RMS + detection pitch via algorithme YIN simplifie
- Si le micro n'est pas detecte au `begin()`, le driver I2S est desinstalle pour liberer les ressources

### AutoCalibrator

Machine d'etat pour la calibration automatique du debit d'air :

```
IDLE --> PREPARE --> SETTLE --> SWEEP --> NOTE_DONE --> (note suivante ou COMPLETE)
```

- **PREPARE** : positionne les doigts pour la note, ouvre le solenoide
- **SETTLE** : attend la stabilisation des servos (`AUTOCAL_SETTLE_MS`)
- **SWEEP** : augmente progressivement l'angle airflow, detecte le debut du son (RMS > seuil + pitch correct) et la fin (silence apres son)
- **NOTE_DONE** : stocke air_min/air_max, passe a la note suivante
- **COMPLETE** : applique les resultats a la config et sauvegarde

### Broadcast audio

Quand le monitoring micro est actif (onglet Calibration), toutes les 100ms :
```json
{"t":"audio","rms":0.12,"hz":440.0,"midi":69,"cents":-5}
```

Pendant l'auto-calibration, progression :
```json
{"t":"acal_prog","idx":3,"note":"C6","total":14,"st":3}
```

## Watchdog ESP32

Task WDT configure a 4000ms. `esp_task_wdt_reset()` appele a chaque iteration de `loop()`.
Si le systeme se bloque (I2C freeze, boucle infinie), l'ESP32 redemarrera automatiquement.
Le `initSafeState()` est appele avant tout dans `setup()` pour que le materiel soit dans un etat sur meme apres un crash.

## Memoire

Budget typique (ESP32-WROOM 520KB SRAM) :
- PROGMEM (web_content.h) : ~42KB en flash, 0 en RAM
- RuntimeConfig cfg : ~300 bytes
- ArduinoJson (parsing) : ~2KB temporaire
- NimBLE stack : ~20KB
- WiFi stack : ~40KB
- AsyncWebServer + WS : ~15KB
- AudioAnalyzer (si micro) : ~4KB (buffer I2S DMA) + ~8KB (buffer analyse)
- Heap libre typique : ~150-180KB (sans micro), ~138-168KB (avec micro)
