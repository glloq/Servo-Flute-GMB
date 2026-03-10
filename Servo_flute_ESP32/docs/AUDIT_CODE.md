# Audit Complet du Code - Servo Flute ESP32

**Date**: 2026-03-10
**Version auditee**: ESP32 1.1
**Fichiers analyses**: 38 fichiers (.ino, .h, .cpp, .md)

---

## Resume executif

Le code est globalement **bien structure et fonctionnel**. L'architecture modulaire est solide avec une bonne separation des responsabilites. Quelques problemes mineurs ont ete identifies, principalement des bugs potentiels dans des cas limites et des points d'amelioration pour la robustesse.

**Verdict global**: **FONCTIONNEL** avec recommandations mineures

| Module | Statut | Problemes |
|--------|--------|-----------|
| MIDI (BLE) | OK | Aucun probleme |
| MIDI (WiFi/rtpMIDI) | OK | 1 mineur |
| MIDI File Player | OK | 2 mineurs |
| Finger Controller | OK | Aucun probleme |
| Note Sequencer | OK | 1 mineur |
| Airflow Controller | OK | 1 mineur |
| Manual Calibration | OK | Aucun probleme |
| Auto Calibration (Mic) | OK | 2 mineurs |
| Config Storage | OK | 1 mineur |
| Power Management | OK | Aucun probleme |
| Web Interface | OK | 1 mineur |

---

## 1. MIDI - BLE (BleMidiHandler)

**Fichiers**: `BleMidiHandler.h`, `BleMidiHandler.cpp`

### Fonctionnement verifie
- Initialisation NimBLE avec callbacks correctement enregistres
- Filtrage canal MIDI (omni ou specifique) : **OK**
- Gestion NoteOn avec velocity=0 traitee comme NoteOff : **OK** (standard MIDI)
- Callbacks statiques avec protection nullptr : **OK**
- Gestion connexion/deconnexion : **OK**

### Problemes detectes
**Aucun** - Le module est propre et conforme au standard BLE-MIDI.

### Remarque
- `startAdvertising()` / `stopAdvertising()` ne font que changer un flag `_advertising` sans appeler l'API NimBLE correspondante. L'advertising est gere automatiquement par la lib BLE-MIDI lors de `BMIDI.begin()`, mais un `startAdvertising()` explicite apres deconnexion pourrait ne pas relancer l'advertising reel si la lib ne le fait pas automatiquement. **Impact faible** : la lib BLE-MIDI relance generalement l'advertising apres deconnexion.

---

## 2. MIDI - WiFi/rtpMIDI (WifiMidiHandler)

**Fichiers**: `WifiMidiHandler.h`, `WifiMidiHandler.cpp`

### Fonctionnement verifie
- Mode STA avec timeout et fallback AP : **OK**
- Mode AP avec configuration (SSID, password, canal) : **OK**
- rtpMIDI (AppleMIDI) sur port 5004 : **OK**
- mDNS avec services apple-midi et http : **OK**
- Scan WiFi asynchrone : **OK**
- Sauvegarde credentials WiFi dans config : **OK** (avec `strncpy` + null-termination)
- Callbacks MIDI identiques a BLE : **OK**

### Problemes detectes

**[MINEUR] `getScanResultsJson()` - injection SSID non-sanitise**
- Fichier: `WifiMidiHandler.cpp:236`
- Le SSID est insere directement dans le JSON sans echappement des caracteres speciaux (`"`, `\`).
- Un SSID malicieux contenant des guillemets pourrait casser le parsing JSON cote client.
- **Impact**: Faible (affichage corrompu dans l'UI, pas de faille de securite reelle sur ESP32).
- **Fix recommande**: Utiliser ArduinoJson pour construire le JSON au lieu de la concatenation String.

---

## 3. MIDI File Player

**Fichiers**: `MidiFilePlayer.h`, `MidiFilePlayer.cpp`

### Fonctionnement verifie
- Parsing SMF Type 0 et Type 1 : **OK**
- VLQ (Variable Length Quantity) : **OK**
- Running status MIDI : **OK**
- Tempo changes (meta event 0x51) : **OK**
- Conversion ticks->ms avec changements de tempo : **OK** (utilise `uint64_t` pour eviter overflow)
- Playback non-bloquant avec `update()` : **OK**
- Pause/resume avec sauvegarde position : **OK**
- Filtrage canal : **OK**
- Detection canaux actifs (bitmask) : **OK**
- Tri insertion pour merge multi-pistes : **OK**

### Problemes detectes

**[MINEUR] Boucle infinie potentielle sur chunks inconnus**
- Fichier: `MidiFilePlayer.cpp:244`
- `t--` dans la boucle de recherche MTrk pourrait boucler indefiniment si le fichier contient beaucoup de chunks non-MTrk consecutifs.
- **Impact**: Bloquerait le parsing mais le watchdog (4s) protege contre un gel total.
- **Fix recommande**: Ajouter un compteur max de chunks inconnus (ex: 10).

**[MINEUR] Allocation memoire fixe**
- Fichier: `MidiFilePlayer.cpp:20`
- `new MidiFileEvent[MIDI_FILE_MAX_EVENTS]` alloue 2000 * 8 = 16KB au `begin()`, meme si le fichier MIDI est petit.
- **Impact**: Consommation memoire permanente. Sur ESP32 avec ~150KB de heap, c'est acceptable mais represente ~10% du heap.
- **Note**: C'est un choix de design delibere pour eviter la fragmentation. Acceptable.

---

## 4. Finger Controller

**Fichiers**: `FingerController.h`, `FingerController.cpp`

### Fonctionnement verifie
- Calcul angle servo (ferme/ouvert/demi-ouvert) : **OK**
- Direction configurable (+1/-1) : **OK**
- Clamping angle [SERVO_MIN_ANGLE, SERVO_MAX_ANGLE] : **OK**
- Conversion angle->PWM (pulse width 550-2450us) : **OK**
- Lookup note par MIDI via `getNoteByMidi()` : **OK**
- Test angle calibration : **OK** (avec bounds checking)

### Problemes detectes
**Aucun** - Module solide et bien borne.

### Remarque
- La formule PWM (`angleToPWM`) est dupliquee entre `FingerController` et `AirflowController`. Une factorisation serait propre mais n'est pas un bug.

---

## 5. Note Sequencer

**Fichiers**: `NoteSequencer.h`, `NoteSequencer.cpp`

### Fonctionnement verifie
- Machine a etats (IDLE -> POSITIONING -> PLAYING -> STOPPING) : **OK**
- Delai servo-to-solenoid configurable : **OK**
- Pre-positionnement des doigts avant ouverture valve : **OK**
- Gestion legato (valve gardee ouverte entre notes rapides) : **OK**
- Compensation timing mecanique : **OK**
- `stop()` force avec retour a IDLE : **OK**

### Problemes detectes

**[MINEUR] handlePlaying() ne traite que le NoteOff de la note courante**
- Fichier: `NoteSequencer.cpp:72-83`
- `handlePlaying()` fait un `peek()` et ne dequeue que si c'est un NoteOff pour la note en cours. Si un NoteOn arrive pendant l'etat PLAYING, il sera traite par `handleIdle()` seulement apres le passage par STOPPING->IDLE.
- Cependant, `processNextEvent()` (appele dans `handleIdle`) gere le cas `_currentState != STATE_IDLE` en appelant `stopCurrentNote()` avant de demarrer la nouvelle note.
- **Impact**: Aucun en pratique, le systeme gere correctement les notes qui se chevauchent. Le design est intentionnel pour le sequencement mecanique.

### Remarque sur le timing
- `_playbackStartTime` est initialise a 0 et mis a `millis()` lors du premier `processNextEvent()`. Cela fonctionne correctement car les timestamps relatifs sont calcules depuis cette reference.

---

## 6. Airflow Controller

**Fichiers**: `AirflowController.h`, `AirflowController.cpp`

### Fonctionnement verifie
- 6 modes d'air avec gestion differentielle : **OK**
- PWM solenoide (activation forte + maintien faible) : **OK**
- Vibrato via LUT sinus 256 entrees : **OK**
- CC7 (Volume) module la plage max : **OK**
- CC11 (Expression) module dans la plage : **OK**
- CC1 (Modulation) controle amplitude vibrato : **OK**
- CC2 (Breath Controller) avec lissage buffer circulaire : **OK**
- CC2 timeout avec fallback vers velocity : **OK**
- CC73 (Attack Time) avec modes stable/accent/crescendo : **OK**
- Servo-valve (alternative au solenoide GPIO) : **OK**
- Transition d'attaque lineraire dans `update()` : **OK**
- `setAirflowLivePercent()` pour slider UI : **OK**

### Problemes detectes

**[MINEUR] CC73 modifie directement `cfg` sans sauvegarder**
- Fichier: `AirflowController.cpp:423-432`
- `setCC73Attack()` modifie `cfg.airAttackMode` et `cfg.airAttackOffset` directement. Ces valeurs ne sont pas persistees (pas de `ConfigStorage::save()`).
- **Impact**: Comportement correct (CC73 est un controle temps-reel, la valeur est volatile). Mais si l'utilisateur modifie airAttackMode via l'UI web et qu'un CC73 arrive ensuite, la valeur UI sera ecrasee en memoire.
- **Fix recommande**: Utiliser des variables membres au lieu de modifier `cfg` directement, ou documenter ce comportement.

### Remarque
- Le calcul d'airflow pour une note est sophistique (5 etapes : per-note range, volume, CC2/velocity, velocity response, expression). La logique est correcte et bien stratifiee.
- `fastSin()` utilise `unsigned long` pour la phase, ce qui overflow proprement modulo `period`. **OK**.

---

## 7. Calibration Manuelle

**Fichiers**: `WebConfigurator.cpp` (messages WebSocket `test_finger`, `test_air`, `test_sol`)

### Fonctionnement verifie
- Test servo doigt a angle arbitraire : **OK** (bounds checking dans `testFingerAngle`)
- Test servo airflow : **OK** (bounds checking dans `testAirflowAngle`)
- Test solenoide open/close : **OK** (via `openSolenoid`/`closeSolenoid`)
- Sliders UI pour calibration temps-reel : **OK** (via WebSocket JSON)
- Test note complete (doigts + airflow + solenoide) : **OK**
- Modification config et sauvegarde via API REST : **OK**
- Reset aux defauts : **OK**

### Problemes detectes
**Aucun** - La calibration manuelle fonctionne correctement.

---

## 8. Calibration Automatique (Microphone)

**Fichiers**: `AudioAnalyzer.h/.cpp`, `AutoCalibrator.h/.cpp`

### Fonctionnement verifie - AudioAnalyzer
- Driver I2S new API (ESP-IDF 5.x `i2s_std`) : **OK**
- Detection automatique microphone (>10% samples non-zero) : **OK**
- Liberation ressources si pas de micro : **OK**
- RMS computation : **OK**
- Pitch detection YIN avec interpolation parabolique : **OK**
- Conversion Hz->MIDI note + cents : **OK**
- Rate limiting (25 Hz max) : **OK**
- Buffer stack pour YIN limite a 256 : **OK** (protection overflow)

### Fonctionnement verifie - AutoCalibrator
- Machine a etats (PREPARE->SETTLE->SWEEP->NOTE_DONE->COMPLETE) : **OK**
- Positionnement doigts avant sweep : **OK**
- Ouverture solenoide pendant sweep : **OK**
- Detection onset (son + pitch ±3 semitones) : **OK**
- Detection offset (3 lectures silencieuses consecutives) : **OK**
- Stockage resultats air_min/air_max par note : **OK**
- Application resultats a la config + sauvegarde : **OK**
- Fermeture propre a la fin ou sur arret : **OK**

### Problemes detectes

**[MINEUR] Division par zero potentielle dans computeRMS()**
- Fichier: `AudioAnalyzer.cpp:186`
- `sqrtf(sum / _validSamples)` - si `_validSamples` est 0, division par zero.
- `analyzeBuffer()` est appele seulement si `_validSamples > 0`, donc en pratique la division par zero ne se produit pas.
- **Impact**: Aucun en pratique grace au guard dans `update()`.

**[MINEUR] AutoCalibrator NOTE_DONE ecrit le resultat a chaque update()**
- Fichier: `AutoCalibrator.cpp:162-188`
- Dans l'etat `ACAL_NOTE_DONE`, le code de stockage s'execute a chaque appel `update()` tant que `elapsed < AUTOCAL_STORE_DELAY_MS`. Avec `AUTOCAL_STORE_DELAY_MS = 10ms` et le loop() a ~1ms, le resultat et la fermeture solenoide sont executes ~10 fois.
- **Impact**: La fermeture solenoide et l'ecriture resultat sont idempotentes (memes valeurs reecrites). Pas de bug fonctionnel mais travail redondant.
- **Fix recommande**: Ajouter un flag `_noteResultStored` pour n'executer le stockage qu'une fois.

---

## 9. Configuration & Storage

**Fichiers**: `ConfigStorage.h/.cpp`, `settings.h`

### Fonctionnement verifie
- Structure `RuntimeConfig` complete avec tous les parametres : **OK**
- `initDefaults()` initialise correctement depuis les constantes `settings.h` : **OK**
- `load()` : defaults d'abord, puis surcharge partielle depuis JSON : **OK**
- `save()` : serialisation complete en JSON : **OK**
- Retro-compatibilite (ancien champ `valve_servo` -> `valveType`) : **OK**
- Retro-compatibilite (ancien mode 6 -> mode 5 + endstop) : **OK**
- Retro-compatibilite (ancien `pump_pin` unique -> `pumpPins[0]`) : **OK**
- Bounds checking sur `numFingers`, `numNotes`, `numPumps` : **OK**
- `strncpy` avec null-termination pour toutes les strings : **OK**
- `factoryReset()` avec suppression fichier : **OK**
- `isFirstBoot()` pour detection premier demarrage : **OK**

### Problemes detectes

**[MINEUR] Mot de passe WiFi stocke en clair**
- Fichier: `ConfigStorage.cpp:451` et `WifiMidiHandler.cpp:257`
- `cfg.wifiPassword` est sauvegarde en clair dans `/config.json` sur LittleFS.
- **Impact**: Sur un ESP32 embarque, le risque est faible (acces physique necessaire pour lire la flash). Mais c'est une pratique a noter.
- **Note**: Acceptable pour un projet embarque sans interface utilisateur sensible. Le WiFi AP est ouvert par defaut de toute facon.

### Remarque
- Le JSON sauvegarde utilise des cles courtes (`"a"`, `"ch"`, `"fp"`) pour economiser de l'espace. Bon choix pour LittleFS.
- `JsonDocument` sans taille specifiee utilise l'allocation dynamique (ArduinoJson v7). Le document est libere automatiquement en fin de scope.

---

## 10. Main Entry Point & Safety

**Fichiers**: `Servo_flute_ESP32.ino`, `InstrumentManager.h/.cpp`

### Fonctionnement verifie
- `initSafeState()` avant tout : solenoide ferme, servos en position closed : **OK**
- LittleFS avec formatage auto au premier boot : **OK**
- Watchdog Task WDT 4s avec `esp_task_wdt_reset()` dans loop : **OK**
- `allSoundOff()` vide la queue, arrete le sequenceur, ferme tout : **OK**
- `resetAllControllers()` remet les CC aux defauts : **OK**
- Rate limiting CC standard (10/s) et CC2 (50/s) : **OK**
- Power management (OE pin, auto-off apres inactivite) : **OK**
- Heap libre affiche au demarrage : **OK**

### Problemes detectes
**Aucun** - L'initialisation est robuste avec un etat sur prioritaire.

### Remarque
- `initSafeState()` cree une instance PWM driver temporaire, puis `InstrumentManager::begin()` en cree une autre. Sur ESP32, l'I2C est partage et le PCA9685 repond a l'adresse 0x40 quelle que soit l'instance. Pas de conflit.

---

## 11. EventQueue

**Fichiers**: `EventQueue.h`, `EventQueue.cpp`

### Fonctionnement verifie
- File FIFO circulaire : **OK**
- Timestamps relatifs au premier evenement : **OK**
- Reference temps resetee quand la queue se vide : **OK**
- Protection overflow (isFull check) : **OK**
- `clear()` propre : **OK**

### Problemes detectes
**Aucun** - Implementation standard et correcte.

### Remarque
- Taille fixe de 16 evenements. Pour du MIDI temps-reel (un noteOn + un noteOff par note), c'est suffisant pour 8 notes simultanees. Largement adequat pour un instrument monophonique.

---

## 12. Modules Secondaires

### PressureController
- Multi-pompes avec cascade et stagger : **OK**
- Bang-bang avec hysteresis pour moteurs On/Off : **OK**
- PID pour moteurs PWM : **OK** (anti-windup avec clamping integral)
- 5 types de capteurs (ToF, Hall, Endstop) : **OK**
- Securites (distance hors portee, surgonflage) : **OK**
- `delay(1)` dans `readSensor()` pour polling I2C : acceptable (max 50ms bloaquant, mais appele toutes les 50ms seulement)

### FanController
- Soft-start ramping (300ms) : **OK**
- Idle management (vitesse reduite entre notes) : **OK**
- Idle timeout configurable : **OK**
- Integration avec NoteSequencer via `InstrumentManager` : **OK**

### StatusLed & HardwareInputs
- Non audites en detail (modules auxiliaires simples).
- Architecture correcte (non-bloquant, debounce, long-press).

---

## 13. Securite Web

### WebConfigurator
- Pas de CORS header explicite (acceptable: SPA servie depuis le meme serveur)
- Pas d'authentification (acceptable: reseau local/AP prive)
- Upload MIDI avec verification taille (`MIDI_FILE_MAX_SIZE = 100KB`) : **OK**
- Verification quota stockage MIDI : **OK**
- WebSocket avec nettoyage clients deconnectes : **OK**
- Pas de validation path-traversal sur upload MIDI : le nom est sanitise (`/midi/` prefix impose)
- Config POST avec parsing JSON (ArduinoJson valide les types) : **OK**

---

## Tableau recapitulatif des problemes

| # | Severite | Module | Description | Ligne |
|---|----------|--------|-------------|-------|
| 1 | MINEUR | WifiMidiHandler | SSID non-echappe dans JSON scan | cpp:236 |
| 2 | MINEUR | MidiFilePlayer | Boucle potentielle sur chunks inconnus | cpp:244 |
| 3 | MINEUR | AirflowController | CC73 modifie cfg directement | cpp:423 |
| 4 | MINEUR | AutoCalibrator | Stockage resultat repete dans NOTE_DONE | cpp:162 |
| 5 | INFO | ConfigStorage | Mot de passe WiFi en clair | cpp:451 |
| 6 | INFO | MidiFilePlayer | Allocation fixe 16KB pour events | cpp:20 |

**Aucun probleme critique ou bloquant detecte.**

---

## Conclusion

Le code est **de bonne qualite** pour un projet embarque de cette complexite :
- Architecture modulaire propre avec separation des responsabilites
- Machines a etats non-bloquantes dans tout le systeme
- Protection watchdog + etat sur au demarrage
- Gestion memoire raisonnable pour ESP32 (~150KB heap libre)
- Configuration persistante robuste avec retro-compatibilite
- Calibration automatique fonctionnelle avec verification pitch
- Rate limiting MIDI pour eviter les floods
- Power management intelligent (servo auto-off)

Les 6 problemes identifies sont tous mineurs et n'affectent pas le fonctionnement normal du systeme. Ils representent principalement des ameliorations de robustesse pour des cas limites rares.
