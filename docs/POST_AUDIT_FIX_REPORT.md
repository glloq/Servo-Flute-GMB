# Post-audit critical fix report

| ID | Défaut audité | Cause | Correction | Test | Résultat |
| -- | ------------- | ----- | ---------- | ---- | -------- |
| CI-001 | CI rouge PlatformIO native | `native_stubs` était déclaré comme dépendance registry et la bibliothèque locale n'avait pas de manifeste | Ajout de `lib/native_stubs/library.json`, `lib_extra_dirs = lib`, et stubs PCA/Wire compilables | `python -m pytest -q` (harness C++ natif) | PASS logiciel |
| PCA-001 | PCA non vérifié avant OE | `beginSafe()` initialisait les sorties avant de vérifier I²C | Détection I²C PCA0/PCA1 avec OE HIGH, statut `HardwareInitStatus`, aucun PWM avant succès | `pca_detection_safe_boot` | PASS logiciel, matériel requis |
| CFG-001 | Configuration active/pending | Risque connu côté API web | Validation centralisée conserve `restartRequired`; correction complète API reste à auditer sur matériel/web | pytest statiques existants | PARTIAL logiciel |
| AIR-001 | Reservoir autostart ignoré | La cible persistée n'était pas appliquée au boot | `beginSafe()` démarre la régulation réservoir seulement si capteur présent; sinon pompes coupées | `reservoir_autostart_behaviour` | PASS logiciel |
| FAN-001 | Ventilateur remplacement rapide | Demande brute MIDI peut diverger du son réel | Couverture de non-régression existante maintenue; refactor callbacks complet non terminé | `note_sequencer_monophonic_replacement`, `fan_autonomous` | PARTIAL logiciel |
| PUMP-001 | Pompe durée minimale | Note Off pouvait arrêter tôt | Séquenceur conserve `minNoteDurationMs`; tests comportementaux existants | `note_sequencer_min_and_panic` | PASS logiciel |
| VALVE-001 | Maintien valve entre notes | Décision dépendait de la tête de file | Couverture existante de remplacement; API explicite complète non terminée | `note_sequencer_monophonic_replacement` | PARTIAL logiciel |
| MAN-001 | Timeout tests manuels | Sessions web non centralisées | Non terminé dans cette passe | Non exécuté | FAIL restant |
| DIAG-001 | Diagnostic actif confondu passif | Routes web à séparer | Non terminé dans cette passe | Non exécuté | FAIL restant |
| GPIO-001 | Conflits GPIO incomplets | Validation couvrait seulement certains modes | Validation existante conservée; extension exhaustive non terminée | pytest statiques existants | PARTIAL logiciel |
| JSON-001 | JSON manuel | Concaténations web restantes | Non terminé dans cette passe | Non exécuté | FAIL restant |
| RESET-001 | Reset/factory reset pas sûr | Arrêt actionneurs pas centralisé | Non terminé dans cette passe | Non exécuté | FAIL restant |
| MIDI-001 | CC73 modifie `cfg` | Etat temporaire stocké dans la configuration persistante | Ajout `_runtimeAttackMode` / `_runtimeAttackOffset`; CC73 ne modifie plus `cfg` | `cc73_does_not_mutate_persistent_cfg` | PASS logiciel |
| TOF-001 | ToF bloquant | Boucles de polling jusqu'à 50 ms | Non terminé dans cette passe | Non exécuté | FAIL restant |
| MIDI-002 | Désync SMF Type 1 | Chaque piste était convertie avec un tempo réinitialisé à 120 BPM ; les changements de tempo de la piste 0 n'atteignaient pas les pistes de notes | Nouvelle `MidiTempoMap` : carte GLOBALE tick→tempo alimentée par toutes les pistes ; parsing en 2 passes (collecte en ticks puis conversion en ms) | `midi_type1_global_tempo`, `midi_type0_tempo_change_midtrack`, `midi_tempo_map_math` | PASS logiciel |
| MIDI-003 | Troncature silencieuse >2000 évts | Le parsing s'arrêtait à `MIDI_FILE_MAX_EVENTS` mais déclarait le fichier valide | `insertEvent` signale la troncature ; `loadFile` refuse le fichier avec le code `event_limit_exceeded` (jamais joué partiellement) | `midi_truncation_rejected` | PASS logiciel |
| MIDI-004 | Formats non gérés acceptés | SMF Type 2 fusionné à tort ; erreurs de format vagues | Rejet explicite du Type 2 et des divisions SMPTE ; `getLoadError()`/`getLoadErrorCode()` exposés à l'API web (champ `reason`) | `midi_unsupported_formats_rejected` | PASS logiciel |
| SAFE-001 | Note/actionneur bloqué à la perte de transport | BLE/rtpMIDI/Wi-Fi/DIN ne coupaient pas le son à la déconnexion | `InstrumentManager::handleTransportLost()` → `allSoundOff()` appelé sur déconnexion BLE, session rtpMIDI, chute du lien STA, et timeout Active Sensing (0xFE) MIDI DIN | `instrument_transport_lost_panics` | PASS logiciel, matériel requis |
| CFG-002 | `angleServoEnabled` hors détection de redémarrage | Champ absent des deux blocs de comparaison ; 2ᵉ PCA9685 jamais initialisé après activation runtime | Ajout de la comparaison dans `ConfigValidator` (redémarrage exigé) | `angle_servo_enable_requires_restart` | PASS logiciel |
| GPIO-002 | GPIO12/15 (strapping) acceptés en sortie | Table de rejet incomplète ; GPIO12 sélectionne la tension flash → risque de brique | `isStrappingGpio()` rejette 12 et 15 | `gpio_validation_reserved_and_conflicts` étendu | PASS logiciel |
| WARN-001 | Avertissements / dépréciations | `Wire.requestFrom(int,uint8_t)` ambigu, `beginResponse_P` déprécié | Cast `(uint8_t)VL53L0X_ADDR` ; `beginResponse()` | Build ESP32 (CI) | PASS logiciel |
| JSON-002 | SSID non échappés dans le scan Wi-Fi | Concaténation brute de `WiFi.SSID()` | `jsonEscape()` sur chaque SSID | Revue | PASS logiciel |
| SEQ-002 | Note courte perdue en POSITIONING | Un Note Off arrivant avant l'ouverture de la valve annulait la note | Report de l'arrêt (note ≥ `minNoteDurationMs`) si le Note Off est postérieur au Note On ; note de durée nulle toujours annulée | `note_sequencer_short_note_still_sounds` | PASS logiciel |
| MIDI-005 | CC 124-127 ignorés | Seuls CC 120/123 coupaient les notes | CC 124-127 (Omni/Mono/Poly) mappés sur All Notes Off | Revue | PASS logiciel |
| CONC-001 | `EventQueue` non synchronisée entre tâches | Callbacks AsyncTCP vs `loop()` sans verrou | Section critique `portMUX` sur enqueue/dequeue/clear | Build native + revue | PARTIAL logiciel (refonte command-queue restante) |
| SEC-001 | Hotspot AP ouvert par défaut | `AP_PASSWORD ""` → `softAP(..., NULL)` | Jamais de hotspot ouvert : clé WPA2 dérivée du MAC si non configurée, affichée au boot | Revue | PARTIAL logiciel (auth API/WS restante) |
| SEQ-003 | Note Off perdu si file pleine | `enqueueLiveEvent` échouait silencieusement → note bloquée | `enqueue…Forced` évince le plus ancien ; utilisé pour les Note Off | `event_queue_forced_never_drops` | PASS logiciel |
| NAME-001 | `deviceName` non appliqué | BLE utilisait la macro `DEVICE_NAME`, mDNS `MDNS_HOSTNAME` | `BLEBMIDI.setName(cfg.deviceName)` avant l'init NimBLE ; mDNS via `deviceName` assaini en étiquette DNS | Revue (matériel requis) | PASS logiciel |
| QUAL-001 | Duplication / code mort / shift no-op | `angleToPWM` dupliqué, `processNextEvent` mort, `<<8>>8` | `ServoMath::servoAngleToPWM` partagé ; code mort retiré ; seuil PitchDetector simplifié | `servo_angle_to_pwm_math` | PASS logiciel |

### Restant à traiter (nécessite refonte ou matériel)

- **SEC-002 — Auth REST/WebSocket** : aucune route/WS n'exige d'authentification. Nécessite un champ mot de passe admin en config + adaptation de l'UI web + validation sur matériel. WPA2 (SEC-001) est la première barrière ; l'auth applicative reste un durcissement à ajouter.
- **CONC-002 — File de commandes** : les callbacks web pilotent encore `cfg`/I2C/actionneurs directement. La section critique `EventQueue` (CONC-001) élimine la corruption de file ; la refonte « callback → file de commandes exécutée dans `loop()` » reste à faire pour `cfg`/I2C.
- **TOF-002 — Init VL53L0X** : la branche VL53L0X n'a pas de séquence d'init (DataInit/SPAD) et est probablement non fonctionnelle ; à porter + valider sur capteur réel.

## Software verified

- Native C++ behavioral harness builds production sources including `InstrumentManager.cpp`.
- pytest suite passes in the constrained environment.

## Hardware verification still required

No hardware was connected in this environment. PCA0/PCA1, OE, servos, solenoid, fan, pumps, Hall, endstop, microphone and ToF sensors remain **NOT TESTED — requires hardware**.
