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

## Software verified

- Native C++ behavioral harness builds production sources including `InstrumentManager.cpp`.
- pytest suite passes in the constrained environment.

## Hardware verification still required

No hardware was connected in this environment. PCA0/PCA1, OE, servos, solenoid, fan, pumps, Hall, endstop, microphone and ToF sensors remain **NOT TESTED — requires hardware**.
