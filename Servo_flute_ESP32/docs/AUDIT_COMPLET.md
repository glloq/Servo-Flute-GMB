# AUDIT COMPLET DU PROJET — Servo Flute MIDI Boop

**Date :** 2026-03-12
**Version auditee :** commit `8944922` (branche `main`)
**Auditeur :** Claude (audit automatise exhaustif)

---

## TABLE DES MATIERES

1. [Resume executif](#1-resume-executif)
2. [Metriques du projet](#2-metriques-du-projet)
3. [Architecture et structure](#3-architecture-et-structure)
4. [Qualite de code et bonnes pratiques](#4-qualite-de-code-et-bonnes-pratiques)
5. [Securite et robustesse](#5-securite-et-robustesse)
6. [Gestion memoire et performance](#6-gestion-memoire-et-performance)
7. [Hardware et conception electronique](#7-hardware-et-conception-electronique)
8. [Logique MIDI et musicale](#8-logique-midi-et-musicale)
9. [Interface web et API](#9-interface-web-et-api)
10. [Documentation](#10-documentation)
11. [Synthese des anomalies](#11-synthese-des-anomalies)
12. [Recommandations prioritaires](#12-recommandations-prioritaires)

---

## 1. RESUME EXECUTIF

Le projet **Servo Flute MIDI Boop** est un systeme embarque ESP32 qui automatise le jeu d'instruments a vent (flutes, ocarinas, NAF, etc.) via des servomoteurs pilotes par MIDI. Le code est **bien structure, modulaire et fonctionnel**. L'architecture est solide pour un projet embedded de cette envergure.

### Points forts
- Architecture modulaire exemplaire (separation claire des responsabilites)
- Systeme de configuration persistant bien concu (JSON/LittleFS)
- Support multi-modes d'air tres complet (6 modes)
- Interface web riche et fonctionnelle
- Gestion du watchdog et etat sur au demarrage (safe state)
- Retro-compatibilite JSON bien geree
- Auto-calibration avec analyse audio (YIN) — fonctionnalite avancee
- Support multi-PCA9685 prepare (jusqu'a 31 servos)

### Points faibles identifies
- Securite web quasi inexistante (pas d'authentification, AP ouvert)
- Construction JSON manuelle dans WebConfigurator (risque XSS/injection)
- Code duplique dans plusieurs modules (angleToPWM, isChannelAccepted)
- CC73 modifie directement `cfg` sans sauvegarder (effet de bord)
- Pas de verification de null apres `new` pour les allocations dynamiques
- `web_content.h` est un fichier monolithique de 3928 lignes

### Verdict global : **BON** avec reserves mineures

---

## 2. METRIQUES DU PROJET

| Metrique | Valeur |
|---|---|
| Fichiers source (.h/.cpp/.ino) | 39 |
| Lignes de code totales | 12 061 |
| dont web_content.h (HTML/CSS/JS) | 3 928 |
| dont code C++ effectif | ~8 133 |
| Nombre de classes | 14 |
| Nombre de structures | 6 |
| Fichiers documentation | 11 |
| Dependances externes | ~6 (ArduinoJson, NimBLE, AppleMIDI, Adafruit_PWMServoDriver, ESPAsyncWebServer, LittleFS) |

### Repartition par module (lignes .cpp + .h)

| Module | Lignes | Role |
|---|---|---|
| WebConfigurator | 1 494 | Serveur web + API REST + WebSocket |
| web_content.h | 3 928 | Interface HTML/CSS/JS embarquee |
| ConfigStorage | 761 | Configuration persistante JSON |
| AirflowController | 651 | Controle servo air + vibrato + CC2 |
| settings.h | 461 | Constantes compile-time |
| PressureController | 568 | Pompes + capteurs + PID |
| MidiFilePlayer | 602 | Parseur et lecteur MIDI SMF |
| AutoCalibrator | 491 | Calibration automatique micro |
| InstrumentManager | 401 | Orchestrateur instrument |
| AudioAnalyzer | 333 | I2S + YIN pitch detection |
| WifiMidiHandler | 399 | WiFi + rtpMIDI |
| WirelessManager | 274 | Orchestrateur sans-fil |
| NoteSequencer | 260 | Machine a etats note |
| BleMidiHandler | 187 | BLE-MIDI NimBLE |
| FanController | 177 | Ventilateur PWM |
| FingerController | 163 | Servos doigts |
| SerialMidiHandler | 225 | MIDI DIN serie |
| EventQueue | 137 | File FIFO circulaire |
| StatusLed | 154 | Patterns LED |
| HardwareInputs | 191 | Bouton + switch |
| Servo_flute_ESP32.ino | 204 | Point d'entree |

---

## 3. ARCHITECTURE ET STRUCTURE

### 3.1 Diagramme de dependances

```
Servo_flute_ESP32.ino (main)
  |
  +-- InstrumentManager
  |     +-- FingerController (PCA9685)
  |     +-- AirflowController (PCA9685 + GPIO solenoide)
  |     +-- PressureController (pompes, capteurs I2C)
  |     +-- FanController (PWM GPIO)
  |     +-- NoteSequencer
  |     |     +-- EventQueue (FIFO)
  |     |     +-- FingerController (ref)
  |     |     +-- AirflowController (ref)
  |     +-- AudioAnalyzer (I2S INMP441)
  |     +-- AutoCalibrator
  |
  +-- WirelessManager
  |     +-- BleMidiHandler (NimBLE)
  |     +-- WifiMidiHandler (AppleMIDI + WiFi + mDNS + DNS captive)
  |     +-- SerialMidiHandler (UART2)
  |     +-- WebConfigurator (ESPAsyncWebServer)
  |     |     +-- web_content.h (PROGMEM HTML)
  |     +-- MidiFilePlayer (SMF parser + playback)
  |
  +-- ConfigStorage (LittleFS JSON)
  +-- StatusLed (GPIO2)
  +-- HardwareInputs (GPIO0 bouton + GPIO4 switch)
  +-- settings.h (constantes compile-time)
```

### 3.2 Evaluation

| Critere | Note | Commentaire |
|---|---|---|
| Separation des responsabilites | **A** | Chaque module a un role unique et clair |
| Couplage | **B+** | Faible couplage, mais `cfg` globale partagee partout |
| Cohesion | **A** | Chaque classe est fortement cohesive |
| Extensibilite | **A** | Ajout d'un nouveau mode d'air = ajout d'un controller |
| Testabilite | **C** | Pas de tests unitaires, forte dependance hardware |
| Nommage | **A-** | Clair et coherent, mix FR/EN acceptable |

### 3.3 Problemes architecturaux

**[ARCH-1] Variable globale `cfg`** — MINEUR
La configuration `RuntimeConfig cfg` est une variable globale accedee directement par tous les modules. C'est un pattern accepte en embarque, mais certains modules modifient `cfg` sans controle :
- `AirflowController::setCC73Attack()` (ligne 429-441) modifie `cfg.airAttackMode` et `cfg.airAttackOffset` directement — ces changements sont volatiles (perdus au reboot) mais creent une incoherence si l'utilisateur sauvegarde ensuite la config.
- `WifiMidiHandler::connectToNetwork()` modifie et sauvegarde `cfg.wifiSsid` et `cfg.wifiPassword`.

**[ARCH-2] Allocation dynamique sans verification null** — MINEUR
Dans `Servo_flute_ESP32.ino`:
```cpp
instrument = new InstrumentManager();  // ligne 137
wireless = new WirelessManager(statusLed, inputs);  // ligne 142
```
Dans `WirelessManager::begin()`:
```cpp
_midiPlayer = new MidiFilePlayer();  // ligne 43
_webConfig = new WebConfigurator();  // ligne 47
```
Dans `EventQueue::EventQueue()`:
```cpp
_events = new MidiEvent[capacity];  // ligne 6
```
Dans `MidiFilePlayer::begin()`:
```cpp
_events = new MidiFileEvent[MIDI_FILE_MAX_EVENTS];  // ligne 20
```
Aucune verification de retour `nullptr`. Sur ESP32, `new` peut retourner `nullptr` si le heap est epuise (contrairement aux systemes desktop ou il leve `std::bad_alloc`).

**[ARCH-3] Pas de destructeur pour EventQueue** — MINEUR
`EventQueue` alloue `_events` avec `new[]` dans le constructeur mais n'a pas de destructeur `~EventQueue()` pour liberer la memoire. En pratique, ces objets ne sont jamais detruits (vie = duree du programme), donc pas de fuite reelle.

---

## 4. QUALITE DE CODE ET BONNES PRATIQUES

### 4.1 Code duplique

**[DUP-1] `angleToPWM()` est duplique** — MINEUR
La meme fonction `angleToPWM()` est implementee dans :
- `FingerController.cpp` (ligne 117-128)
- `AirflowController.cpp` (ligne 399-410)

Les deux implementations sont identiques. Devrait etre une fonction utilitaire partagee.

**[DUP-2] `isChannelAccepted()` est duplique** — MINEUR
La meme logique de filtrage canal MIDI est dupliquee dans :
- `BleMidiHandler.cpp` (ligne 124-127)
- `WifiMidiHandler.cpp` (ligne 301-304)
- `SerialMidiHandler.cpp` (ligne 152-155)

**[DUP-3] Logique PID dupliquee dans PressureController** — MINEUR
Le bloc PID (lignes 303-318 et 371-388) est copie-colle entre le mode Hall et le mode ToF. Devrait etre une methode privee `computePID()`.

### 4.2 Qualite du parsing MIDI (MidiFilePlayer)

**[MIDI-1] Boucle de skip de chunks inconnues peut boucler indefiniment** — MOYEN
Dans `parseFile()` (ligne 244) :
```cpp
t--;  // Reessayer pour trouver MTrk
continue;
```
Si le fichier contient beaucoup de chunks non-MTrk, `t` ne progresse jamais, ce qui cree une boucle infinie potentielle. Il manque un compteur de tentatives ou un check de fin de fichier dans cette boucle.

**[MIDI-2] readVLQ sans protection contre les boucles infinies** — MOYEN
`readVLQ()` (ligne 436-448) lit des octets jusqu'a trouver un byte < 0x80. Un fichier MIDI malveillant/corrompu pourrait avoir tous les bytes > 0x80, causant une lecture infinie (jusqu'a la fin du fichier, mais `file.read()` retourne -1 qui est 0xFF donc le bit 0x80 est set).

### 4.3 Constantes magiques

**[MAGIC-1] Valeurs hardcodees dans le code** — NEGLIGEABLE
Quelques valeurs hardcodees sans define/constante :
- `AudioAnalyzer::update()` : `40` ms hardcode (devrait etre `#define MIC_ANALYSIS_INTERVAL_MS 40`)
- `AudioAnalyzer::detectMicrophone()` : `256` samples, `500` ms timeout, `10%` threshold
- `FanController.cpp` : `FAN_RAMP_TIME_MS 300` est defini localement, pas dans settings.h
- `PressureController.cpp` : adresses I2C `0x29` hardcodees (meme adresse pour VL53L0X et VL6180X)

### 4.4 Qualite de la machine a etats (NoteSequencer)

**[SEQ-1] Transition STATE_STOPPING est instantanee** — OBSERVATION
`handleStopping()` fait juste `transitionTo(STATE_IDLE)` — c'est un no-op effectif. L'etat STOPPING n'a aucune utilite concrete actuellement, mais offre un point d'extension pour un eventuel delai de fermeture des doigts.

**[SEQ-2] _playbackStartTime non reinitialise apres vidage de la queue** — MINEUR
`NoteSequencer::stop()` ne reinitialise pas `_playbackStartTime`. Si la queue est videe par un "all sound off" et de nouveaux evenements arrivent, le timing relatif peut etre incorrectement calcule, bien que le check `_playbackStartTime == 0` en ligne 98 compense partiellement.

### 4.5 Style et conventions

| Critere | Evaluation |
|---|---|
| Indentation | Coherente (2 espaces partout) |
| Nommage variables | camelCase pour membres (_prefix), UPPER_CASE pour constantes |
| Commentaires | Presents et utiles, en francais, coherents |
| Guards #ifndef | Presents partout |
| Include order | Pas strictement alphabetique mais logique |
| Const correctness | Bonne (getters const, params const ou ref) |
| Gestion d'erreurs | Present mais pas systematique |

---

## 5. SECURITE ET ROBUSTESSE

### 5.1 Securite reseau

**[SEC-1] Point d'acces WiFi sans mot de passe** — MOYEN
`AP_PASSWORD ""` dans `settings.h` (ligne 346). Le hotspot `ServoFlute-Setup` est ouvert. Toute personne a portee WiFi peut se connecter et controler entierement le dispositif : actionner les servos, uploader des fichiers, faire un factory reset, lire le mot de passe WiFi du reseau domestique.

**Risque :** En atelier/makerspace partage, quelqu'un pourrait actionner les servos/solenoides de maniere inattendue (risque mecanique/brulure solenoid si ouvert trop longtemps).

**[SEC-2] Aucune authentification sur l'API REST et WebSocket** — MOYEN
Tous les endpoints sont accessibles sans aucune verification. En mode STA (connexion reseau domestique), tout appareil sur le meme LAN peut :
- `POST /api/config` : modifier toute la configuration
- `POST /api/config/factory` : effacer toute la configuration
- `POST /api/midi/delete` : supprimer des fichiers MIDI
- WebSocket : commander directement les servos et solenoides

**[SEC-3] Mot de passe WiFi stocke en clair** — MINEUR
`cfg.wifiPassword` est sauvegarde tel quel dans `/config.json` sur LittleFS. Quelqu'un avec acces physique ou acces reseau (via l'API `/api/config` en GET) peut lire le mot de passe WiFi.

**[SEC-4] XSS potentiel via SSID WiFi non echappe** — MINEUR
`WifiMidiHandler::getScanResultsJson()` (ligne 251) :
```cpp
json += "{\"ssid\":\"" + WiFi.SSID(i) + "\"";
```
Un SSID contenant `"`, `\` ou des balises HTML serait injecte tel quel dans le JSON, puis potentiellement rendu via `innerHTML` dans le navigateur.

**[SEC-5] Pas de HTTPS** — NEGLIGEABLE (contrainte ESP32)
Tout le trafic (y compris l'envoi du mot de passe WiFi via `/api/wifi/connect`) circule en HTTP clair. C'est une limitation connue de la RAM ESP32 pour TLS.

### 5.2 Robustesse

**[ROB-1] Delay bloquant dans readSensor() (PressureController)** — MOYEN
`PressureController::readSensor()` (lignes 201, 213) utilise `delay(1)` dans une boucle de polling (jusqu'a 50 iterations = 50ms bloquantes dans le pire cas). Cela bloque tout le loop() et peut declencher le watchdog si combine avec d'autres delais.

**[ROB-2] Division par zero possible dans PressureController** — MINEUR
`PressureController::update()` ligne 275 :
```cpp
(cfg.hallThresholdHigh - cfg.hallThresholdLow)
```
Si `hallThresholdHigh == hallThresholdLow`, division par zero. Idem pour `sensorMaxMm - sensorMinMm` (ligne 333).

**[ROB-3] Overflow potentiel dans calcul airflow** — NEGLIGEABLE
`AirflowController::setAirflowForNote()` ligne 118 :
```cpp
(cfg.servoAirflowMax - cfg.servoAirflowMin) * airMin / 100
```
Les types sont `uint16_t` et `uint8_t`. Le produit `uint16_t * uint8_t` peut atteindre 180 * 100 = 18000, ce qui tient dans un `uint16_t` (max 65535). Pas de probleme en pratique.

**[ROB-4] Millis() overflow apres 49.7 jours** — NEGLIGEABLE
Les calculs de delai utilisent `millis() - startTime` ce qui fonctionne correctement avec l'overflow de `unsigned long` grace a l'arithmetique non-signee. Pas de bug ici.

### 5.3 Securite hardware

**[HW-SEC-1] Pas de protection surchauffe solenoide** — MINEUR
Le mode PWM du solenoide (`SOLENOID_USE_PWM`) reduit le courant de maintien (holding current = 128/255), mais il n'y a pas de timeout maximum pour la duree d'ouverture du solenoide. Un bug logiciel ou une commande WebSocket pourrait laisser le solenoide ouvert indefiniment, causant une surchauffe.

**[HW-SEC-2] Pas de limitation de duree pour testSolenoid** — MINEUR
`AirflowController::testSolenoid(true)` ouvre le solenoide sans aucune limite de temps. Le WebSocket permet d'envoyer `test_sol:{"o":1}` ce qui ouvre le solenoide tant qu'un `{"o":0}` n'est pas envoye.

---

## 6. GESTION MEMOIRE ET PERFORMANCE

### 6.1 Empreinte memoire RAM

| Allocation | Taille estimee | Type |
|---|---|---|
| `RuntimeConfig cfg` | ~4.9 KB | Statique (BSS) |
| `NoteConfig[128]` (dans cfg) | ~128 * 36 = 4.6 KB | Statique |
| `FingerConfig[31]` (dans cfg) | ~31 * 5 = 155 B | Statique |
| `SIN_LUT[256]` | 256 B | Flash (PROGMEM) |
| `EventQueue._events[16]` | 16 * 12 = 192 B | Heap |
| `MidiFileEvent[2000]` | 2000 * 8 = 16 KB | Heap |
| `AudioAnalyzer._rawBuffer[1024]` | 4 KB | Statique |
| `AudioAnalyzer._analysisBuffer[1024]` | 4 KB | Statique |
| `AutoCalibrator._results[128]` | ~384 B | Statique |
| `web_content.h` (PROGMEM) | ~130+ KB | Flash |
| Stack YIN (yinBuf[258]) | ~1 KB | Stack |
| Total RAM statique estimee | **~30 KB** | |
| Total heap dynamique | **~17 KB** | |

ESP32-WROOM a ~320 KB de RAM. L'empreinte totale (~47 KB code + donnees) laisse ~270 KB pour le WiFi stack (~80 KB), BLE stack (~60 KB), RTOS (~20 KB), et le reste pour le heap dynamique. C'est serré mais viable.

### 6.2 Empreinte Flash

| Composant | Taille estimee |
|---|---|
| web_content.h PROGMEM | ~130 KB |
| Code compile | ~800 KB-1.2 MB (avec NimBLE + WiFi + AppleMIDI) |
| LittleFS partition | ~1.5 MB |

### 6.3 Performance du loop()

```
loop() {
  esp_task_wdt_reset()          // ~1 us
  inputs.update()               // ~10 us (digitalRead x2 + debounce)
  wireless->update()            // Variable :
    - BLE: BMIDI.read()         // ~10 us (pas de msg) a ~100 us
    - WiFi: MIDI.read()         // ~50 us
    - _midiPlayer->update()     // ~10 us (idle) a ~500 us (burst)
    - _webConfig->update()      // ~50 us (idle) a ~1 ms (WS broadcast)
    - _dnsServer.processNext()  // ~20 us (AP mode)
  instrument->update()          // ~20 us + I2C si PCA9685
    - _sequencer.update()       // ~5 us (idle) a ~500 us (I2C writes)
    - _airflowCtrl.update()     // ~10 us (vibrato calcul + I2C)
    - _pressureCtrl.update()    // ~0 (mode 0-3) a ~50 ms (sensor read!)
    - _fanCtrl.update()         // ~5 us
    - managePower()             // ~5 us
  statusLed.update()            // ~5 us
}
```

**Observation critique :** `PressureController::readSensor()` avec ses `delay(1)` dans la boucle de polling bloque potentiellement 50 ms. Combine avec le watchdog a 4000 ms, c'est sûr, mais la latence de 50 ms est perceptible musicalement (surtout pour le vibrato).

### 6.4 Allocation dynamique

**[MEM-1] MidiFilePlayer alloue 16 KB de heap au boot** — OBSERVATION
`MidiFileEvent[2000]` = 16 KB est alloue des `begin()`, meme si aucun fichier MIDI n'est jamais lu. En mode BLE, cet objet n'est pas cree (bon), mais en mode WiFi il l'est toujours.

**[MEM-2] EventQueue ne libere jamais sa memoire** — NEGLIGEABLE
`new MidiEvent[16]` dans le constructeur, pas de `delete[]` dans le destructeur (absent). L'objet vit toute la duree du programme, donc pas de fuite.

**[MEM-3] String Arduino utilise en plusieurs endroits** — OBSERVATION
`String` Arduino (allocation heap dynamique) est utilise dans :
- `WifiMidiHandler::getScanResultsJson()` — concatenation de strings en boucle = fragmentation heap
- `MidiFilePlayer::_fileName` — `String` membre
- `WirelessManager::getStatusText()` — retour de String

Les `String` Arduino sont connus pour fragmenter le heap sur ESP32. Pour un usage aussi ponctuel, c'est acceptable.

---

## 7. HARDWARE ET CONCEPTION ELECTRONIQUE

### 7.1 Assignation des GPIO ESP32

| GPIO | Fonction | Type | Commentaire |
|---|---|---|---|
| 0 | Bouton BOOT/appairage | Input (pullup) | OK, bouton BOOT standard |
| 2 | LED statut | Output | OK, LED integree |
| 4 | Switch BT/WiFi | Input | OK |
| 5 | PCA9685 OE | Output | OK, controle alimentation servos |
| 13 | Solenoide | Output (PWM) | OK |
| 14 | I2S BCLK (mic) | Output | OK |
| 15 | I2S WS (mic) | Output | **Attention :** GPIO15 a un pullup interne au boot (JTAG). Peut generer un bruit au demarrage du mic |
| 16 | MIDI RX (UART2) | Input | OK (UART2 RX par defaut) |
| 21 | I2C SDA | Bidirectionnel | OK (defaut ESP32) |
| 22 | I2C SCL | Output | OK (defaut ESP32) |
| 25 | Pompe 1 (DAC) | Output PWM | OK, DAC-capable |
| 26 | Pompe 2 / Ventilateur | Output PWM | **Conflit potentiel:** partage entre pompe[1] et fanPin par defaut |
| 27 | Pompe 3 | Output PWM | OK |
| 32 | I2S DIN (mic) | Input | OK (input only) |
| 34 | Endstop | Input | OK (input only, pas de pullup — externe requis) |
| 36 | Hall effect | Input ADC | OK (input only, ADC1) |

### 7.2 Problemes GPIO identifies

**[GPIO-1] Conflit GPIO26 entre pompe 2 et ventilateur** — MOYEN
`DEFAULT_FAN_PIN = 26` et `cfg.pumpPins[1] = 26` (ConfigStorage.cpp ligne 120). Si un utilisateur configure le mode ventilateur ET 2 pompes, le GPIO26 sera utilise pour les deux fonctions, causant un conflit.

**Correction :** La logique d'air mode devrait empecher la coexistence de ventilateur et pompes, ou un check devrait etre ajoute.

**[GPIO-2] GPIO15 avec pullup au boot** — NEGLIGEABLE
GPIO15 (I2S WS pour le micro) a un pullup interne au boot (strapping pin). Cela peut generer un bruit sur le bus I2S au demarrage, mais n'affecte pas le fonctionnement normal apres initialisation I2S.

**[GPIO-3] GPIO34 sans pullup interne** — OBSERVATION
GPIO34 est input-only et n'a pas de pullup/pulldown interne. Le code configure `INPUT_PULLUP` (PressureController.cpp ligne 102), mais cela n'a aucun effet sur GPIO34. Un pullup externe est necessaire pour le fin de course.

### 7.3 Bus I2C partage

Le bus I2C (SDA=21, SCL=22) est partage entre :
- PCA9685 (adresse 0x40 par defaut, 0x41 pour le 2eme)
- Capteurs ToF VL53L0X ou VL6180X (adresse 0x29)

**[I2C-1] VL53L0X et VL6180X ont la meme adresse I2C** — OBSERVATION
Les deux capteurs ont l'adresse 0x29 (lignes 6-7 de PressureController.cpp). Il n'est pas possible d'utiliser les deux en meme temps sans changer l'adresse d'un des deux via son XSHUT pin. Le code ne supporte qu'un seul capteur a la fois, ce qui est correct.

### 7.4 Gestion d'alimentation servos

Le systeme utilise le pin OE (Output Enable) du PCA9685 pour couper les servos en cas d'inactivite (`managePower()`). Le delai d'inactivite est configurable (`cfg.timeUnpower`, defaut 200ms). C'est une bonne pratique pour :
- Reduire le bruit des servos au repos
- Economiser l'energie
- Prolonger la vie des servos

**[PWR-1] Delai de coupure tres court** — OBSERVATION
200ms est tres court comme timeout par defaut. Les servos seront coupes entre chaque note si le tempo est lent. Le code les reactive sur la prochaine note, mais le "bzzz" de reactivation peut etre audible.

---

## 8. LOGIQUE MIDI ET MUSICALE

### 8.1 Gestion des notes

La chaine de traitement MIDI est propre :
```
MIDI In (BLE/WiFi/Serial/File) → Canal filter → InstrumentManager.noteOn()
  → EventQueue.enqueue() → NoteSequencer.processNextEvent()
  → FingerController.setFingerPatternForNote() → STATE_POSITIONING
  → [delai servo] → AirflowController.setAirflowForNote() + openSolenoid()
  → STATE_PLAYING
```

### 8.2 Chaine de traitement du souffle

```
Velocity MIDI ─────────────────────────────┐
                                           │
CC2 (Breath) ──► Lissage ──► Courbe ──► airflowSource
                                           │
CC7 (Volume) ──► effectiveMaxAngle ────────┤
                                           │
cfg.airVelocityResponse ──► midpoint ──► baseAngle
                                           │
CC11 (Expression) ──► expressionFactor ──► finalAngle
                                           │
CC1 (Modulation) ──► vibrato (SIN LUT) ──► angle + oscillation
                                           │
CC73 (Attack) ──► accent/crescendo ────────┤
                                           │
CC74 (Brightness) ──► angle servo trav ────┘
```

Ce pipeline est **bien concu** et offre une expressivite musicale remarquable.

### 8.3 Problemes musicaux

**[MUS-1] EventQueue trop petite pour le polyphonique** — OBSERVATION
`EVENT_QUEUE_SIZE = 16` est suffisant pour un instrument monophonique, mais si un fichier MIDI contient des accords rapides, la queue peut deborder. Le code gere le debordement (enqueue retourne false), mais les notes sont simplement perdues sans notification MIDI (pas de "note off" pour compenser).

**[MUS-2] Pas de gestion du legato** — OBSERVATION
Quand une nouvelle note arrive pendant qu'une note est en cours (STATE_PLAYING), le sequenceur fait `stopCurrentNote()` puis `startNoteSequence()`. Il n'y a pas de mode legato ou les doigts changeraient sans couper la valve. Pour un instrument a vent, le legato est musical — c'est une evolution possible.

**[MUS-3] Servo angle (trav) ne profite pas du vibrato** — OBSERVATION
Le vibrato (CC1) ne s'applique qu'au servo flow (axe du debit), pas au servo angle (axe de l'inclinaison). Pour une flute traversiere, un vibrato d'angle simulerait le mouvement naturel de l'embouchure du joueur. C'est une optimisation musicale possible.

### 8.4 Parseur MIDI (MidiFilePlayer)

Le parseur MIDI SMF est **complet et bien implemente** :
- Support SMF Type 0 et Type 1 (merge multi-pistes via sort)
- Running status correctement gere
- Variable Length Quantity (VLQ)
- Changements de tempo mid-track
- SysEx correctement skip
- SMPTE time division detecte et refuse proprement

**[MIDI-3] Tri par insertion sort (O(n^2))** — NEGLIGEABLE
`sortEvents()` utilise un insertion sort qui est O(n^2). Pour n=2000 max, c'est ~4M comparaisons au pire cas. Sur ESP32 a 240MHz, ca prend ~50ms. Acceptable pour un chargement de fichier.

---

## 9. INTERFACE WEB ET API

### 9.1 Architecture web

- Serveur : ESPAsyncWebServer
- Contenu : HTML/CSS/JS monolithique dans `web_content.h` (PROGMEM)
- Temps reel : WebSocket natif
- API : REST JSON (construction manuelle)

### 9.2 Problemes identifies

**[WEB-1] Construction JSON manuelle sans echappement** — MOYEN
Tout au long de `WebConfigurator.cpp`, le JSON est construit par concatenation de chaines :
```cpp
json += "\"ssid\":\"" + WiFi.SSID(i) + "\"";
json += "\"device\":\"" + String(cfg.deviceName) + "\"";
```
Si `deviceName` ou un SSID contient `"` ou `\`, le JSON produit est invalide. Si le JSON casse est ensuite affiche via `innerHTML`, c'est un vecteur XSS.

**[WEB-2] Buffer partage `_configBody`** — MINEUR
La variable membre `_configBody` est reutilisee par les handlers de POST `/api/config`, `/api/wifi/connect`, `/api/midi/delete` et `/api/midi/load`. ESPAsyncWebServer est mono-thread mais les handlers peuvent s'entremeler si deux clients font des requetes quasi-simultanees.

**[WEB-3] Parsing WebSocket JSON fait a la main** — MINEUR
`processWsMessage()` utilise `indexOf()` et `substring()` pour parser le JSON des messages WebSocket. C'est fragile et peut casser si l'ordre des cles change ou si des valeurs contiennent les memes patterns.

**[WEB-4] Pas de rate limiting sur les commandes WebSocket** — MINEUR
Un client malveillant pourrait envoyer des milliers de commandes `test_finger` ou `test_air` par seconde, saturant le bus I2C et potentiellement causant un reset watchdog.

**[WEB-5] `user-scalable=no` dans le viewport** — NEGLIGEABLE
La balise meta viewport inclut `user-scalable=no`, ce qui empeche le zoom sur mobile. C'est un probleme d'accessibilite pour les utilisateurs malvoyants.

**[WEB-6] Pas de confirmation pour factory reset** — MINEUR
`POST /api/config/factory` effectue un reset usine sans confirmation. Un appel accidentel ou malveillant efface toute la configuration.

### 9.3 Endpoints API documentes vs implementes

| Endpoint | Documente | Implemente | Note |
|---|---|---|---|
| GET /api/config | Oui | Oui | OK |
| POST /api/config | Oui | Oui | OK |
| POST /api/config/factory | Non | Oui | **Manque dans docs** |
| GET /api/wifi/scan | Oui | Oui | Devrait etre POST |
| GET /api/wifi/status | Non | Oui | **Manque dans docs** |
| POST /api/wifi/connect | Oui | Oui | OK |
| POST /api/midi (upload) | Oui | Oui | OK |
| GET /api/midi/list | Non | Oui | **Manque dans docs** |
| POST /api/midi/delete | Non | Oui | **Manque dans docs** |
| POST /api/midi/load | Non | Oui | **Manque dans docs** |
| WS /ws | Oui | Oui | Partiellement documente |

---

## 10. DOCUMENTATION

### 10.1 Fichiers de documentation

| Fichier | Contenu | Qualite |
|---|---|---|
| README.md | Vue d'ensemble, features, hardware | **Bon** |
| docs/ARCHITECTURE.md | Diagramme de modules | **Bon** |
| docs/API_WEB.md | Endpoints REST + WebSocket | **Incomplet** (endpoints manquants) |
| docs/CALIBRATION.md | Guide calibration manuelle | **Bon** |
| docs/CONFIGURATION.md | Parametres config | **Bon** |
| docs/WIFI_MODES.md | Modes WiFi STA/AP | **Bon** |
| docs/MIDI_SERIAL.md | MIDI DIN serie | **Bon** |
| docs/AIR_MANAGEMENT.md | Systeme pneumatique | **Tres bon** |
| docs/AUTO_CALIBRATION.md | Auto-calibration micro | **Bon** |
| docs/PCA9685_EXPANSION.md | Expansion multi-PCA | **Attention:** c'est un document de conception, pas de l'implementation |
| docs/SERVO_ANGLE.md | Servo angle traversiere | **Bon** |
| AUDIT_CODE.md | Audit precedent (2026-03-11) | **Bon** |

### 10.2 Problemes de documentation

**[DOC-1] API_WEB.md incomplet** — MOYEN
5 endpoints ne sont pas documentes (voir section 9.3). Plusieurs formats de messages WebSocket sont egalement absents ou incorrects.

**[DOC-2] PCA9685_EXPANSION.md est un doc de conception** — MINEUR
Ce fichier est une etude technique ("Etude technique") avec du code propose, pas de la documentation de fonctionnalites implementees. Le README le lie sans distinction, ce qui peut porter a confusion.

**[DOC-3] Estimation taille PROGMEM incorrecte** — NEGLIGEABLE
API_WEB.md annonce ~36KB, le header de web_content.h dit ~42KB, le fichier fait en realite ~130KB+.

### 10.3 Commentaires dans le code

Les commentaires sont **bons** :
- Header de chaque fichier avec description du role du module
- Commentaires en francais, coherents
- Sections bien delimitees dans settings.h
- Commentaires inline pour les formules mathematiques (vibrato, PID, YIN)
- Le code est globalement auto-documente grace au bon nommage

---

## 11. SYNTHESE DES ANOMALIES

### Par severite

| Severite | Nombre | IDs |
|---|---|---|
| **CRITIQUE** | 0 | — |
| **MOYEN** | 7 | SEC-1, SEC-2, GPIO-1, MIDI-1, MIDI-2, ROB-1, WEB-1 |
| **MINEUR** | 15 | ARCH-1, ARCH-2, ARCH-3, DUP-1, DUP-2, DUP-3, SEC-3, SEC-4, ROB-2, HW-SEC-1, HW-SEC-2, WEB-2, WEB-3, WEB-4, WEB-6 |
| **NEGLIGEABLE** | 8 | ROB-3, ROB-4, MAGIC-1, MEM-2, GPIO-2, MIDI-3, WEB-5, DOC-3 |
| **OBSERVATION** | 9 | SEQ-1, SEQ-2, MEM-1, MEM-3, I2C-1, GPIO-3, PWR-1, MUS-1, MUS-2, MUS-3 |

### Par categorie

| Categorie | Critique | Moyen | Mineur | Negligeable | Observation |
|---|---|---|---|---|---|
| Securite | 0 | 2 | 4 | 1 | 0 |
| Architecture | 0 | 0 | 3 | 0 | 0 |
| Code qualite | 0 | 2 | 3 | 1 | 2 |
| Robustesse | 0 | 1 | 1 | 2 | 0 |
| Hardware | 0 | 1 | 2 | 1 | 2 |
| Performance | 0 | 0 | 0 | 0 | 3 |
| Web/API | 0 | 1 | 3 | 1 | 0 |
| Documentation | 0 | 0 | 1 | 1 | 0 |
| Musical | 0 | 0 | 0 | 0 | 3 |

---

## 12. RECOMMANDATIONS PRIORITAIRES

### Priorite 1 — A corriger (impact fonctionnel/securite)

1. **Proteger la boucle de chunks inconnus dans MidiFilePlayer** (MIDI-1)
   - Ajouter un compteur max de tentatives (ex: 50) pour eviter la boucle infinie
   - Ajouter un check `file.position() < fileSize` dans la boucle

2. **Proteger readVLQ contre les boucles infinies** (MIDI-2)
   - Limiter le nombre de bytes lus (VLQ MIDI ne devrait jamais depasser 4 bytes)
   - Ajouter un guard : `if (bytesRead > 4) break;`

3. **Proteger PressureController::readSensor() contre le blocage** (ROB-1)
   - Remplacer `delay(1)` par un polling non-bloquant dans `update()`
   - Ou utiliser le mode continuous du capteur plutot que single-shot

4. **Ajouter un mot de passe par defaut au hotspot** (SEC-1)
   - Meme un mot de passe simple (`ServoFlute`) dans `AP_PASSWORD` est mieux que rien

5. **Echapper les chaines JSON dans WebConfigurator** (WEB-1)
   - Utiliser ArduinoJson pour serialiser les reponses au lieu de la concatenation manuelle
   - Ou a minima, echapper `"`, `\`, et les caracteres de controle dans les SSID, filenames, deviceName

### Priorite 2 — A ameliorer (qualite/maintenabilite)

6. **Extraire `angleToPWM()` en fonction utilitaire** (DUP-1)
   - Creer un `ServoUtils.h` avec les fonctions partagees

7. **Proteger les divisions par zero dans PressureController** (ROB-2)
   - Ajouter `if (divisor == 0) return 0;` avant les divisions

8. **Documenter les endpoints API manquants** (DOC-1)
   - Ajouter `/api/config/factory`, `/api/wifi/status`, `/api/midi/list`, `/api/midi/delete`, `/api/midi/load`

9. **Verifier les allocations dynamiques** (ARCH-2)
   - Ajouter `if (ptr == nullptr) { reboot/halt }` apres chaque `new`

10. **Resoudre le conflit GPIO26** (GPIO-1)
    - Utiliser un GPIO different par defaut pour le ventilateur ou la pompe 2
    - Ajouter une validation dans ConfigStorage::load() pour detecter les conflits GPIO

### Priorite 3 — Evolutions souhaitables

11. **Ajouter un mode legato** (MUS-2) — changement de doigtes sans couper l'air
12. **Ajouter un timeout de securite pour le solenoide** (HW-SEC-1, HW-SEC-2)
13. **Vibrato sur le servo angle** (MUS-3) pour la traversiere
14. **Marqueur PCA9685_EXPANSION.md comme "futur/conception"** (DOC-2) dans le README

---

*Fin de l'audit — Servo Flute MIDI Boop v1.1 — 2026-03-12*
