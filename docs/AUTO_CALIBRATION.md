# Auto-calibration avec micro INMP441

Module optionnel de calibration automatique utilisant un microphone MEMS I2S pour detecter le son produit par l'instrument et ajuster les plages d'airflow.

Pour la calibration manuelle (sliders, assistant doigts, tests), voir [CALIBRATION.md](CALIBRATION.md).

## Materiel necessaire

| Pin INMP441 | GPIO ESP32 | Description |
|-------------|------------|-------------|
| SCK (BCLK) | GPIO 14 | Horloge bit I2S |
| WS (LRCLK) | GPIO 15 | Selection canal I2S |
| SD (DIN) | GPIO 32 | Donnees audio I2S |
| VDD | 3.3V | Alimentation |
| GND | GND | Masse |
| L/R | GND | Canal gauche (connecter a GND) |

## Detection automatique

Au demarrage, le firmware lit 256 echantillons I2S. Si >10% sont non-nuls, le micro est considere present (`"mic": true` dans `/api/config`). Sinon, le driver I2S est desinstalle pour liberer les ressources.

## Prerequis avant auto-calibration

L'auto-calibration ne peut fonctionner que si l'instrument est deja partiellement configure :

1. **Doigts calibres** — l'assistant calibration doigts doit etre termine (angles de fermeture corrects pour chaque servo). Sinon les doigts ne bouchent pas les trous correctement
2. **Table des doigtes definie** — chaque note doit avoir son pattern de doigts configure dans Config > Doigtes. L'auto-cal utilise ces patterns pour positionner les doigts avant de tester chaque note
3. **Systeme d'air operationnel** — le solenoide/valve doit fonctionner (teste dans Calibration > Test solenoide) et l'air doit effectivement arriver a la flute
4. **Micro proche du bec** — le micro INMP441 doit etre positionne a quelques centimetres du bec de la flute pour capter le son correctement

### Workflow recommande

```
1. Calibration manuelle des doigts (CALIBRATION.md, etapes 1-5)
       ↓
2. Configuration des doigtes par note (Config > Doigtes)
       ↓
3. Range Finder → detecte la plage utile du servo airflow (angles globaux)
       ↓
4. Auto-calibration par note → affine les % d'airflow pour chaque note
```

## Angles vs pourcentages

Deux systemes de coordonnees coexistent :

| Concept | Unite | Description |
|---------|-------|-------------|
| `air_off` | angle servo (deg) | Position repos, aucun son |
| `air_min` | angle servo (deg) | Debut de la plage utile (son le plus faible) |
| `air_max` | angle servo (deg) | Fin de la plage utile (son le plus fort) |
| Per-note `airflowMinPercent` | % (0-100) | Position dans la plage [air_min, air_max] |
| Per-note `airflowMaxPercent` | % (0-100) | Position dans la plage [air_min, air_max] |

Conversion angle → pourcentage :
```
pct = (angle - air_min) * 100 / (air_max - air_min)
```

Le **Range Finder** detecte `air_min` et `air_max` en angles absolus.
L'**auto-calibration par note** travaille en pourcentages dans cette plage.

## Etape 1 : Range Finder (detection plage servo)

Le Range Finder balaye le servo airflow de 0 a 180 degres pour trouver a quels angles la flute produit du son. C'est l'etape a faire en premier car elle definit la plage dans laquelle l'auto-calibration par note va ensuite travailler.

### Deroulement

1. **Preparation** (RF_PREPARE)
   - Choisit la note du milieu de la tessiture (`cfg.numNotes / 2`)
   - Positionne les doigts pour cette note
   - Met le servo airflow a 0 deg
   - Ouvre le solenoide/valve

2. **Stabilisation** (RF_SETTLE)
   - Attend 300ms pour que les servos atteignent leur position

3. **Sweep** (RF_SWEEP)
   - Incremente l'angle de **1 deg toutes les 100ms** (`AUTOCAL_RF_STEP_MS`)
   - A chaque pas, analyse le son :
     - **Debut du son detecte** : RMS > 0.02 **ET** pitch a ±3 demi-tons de la note attendue
       → `rfMinAngle = angle - 3` (marge de securite `AUTOCAL_RF_MARGIN_DEG`)
     - **Fin du son detectee** : 3 lectures consecutives sans son valide (`AUTOCAL_SILENCE_COUNT`)
       → `rfMaxAngle = angle - 3 + 3` (retranche les 3 lectures silencieuses + marge)
     - Si 180 deg atteint sans fin de son → `rfMaxAngle = 180`
   - Duree totale du sweep : ~18 secondes (180 pas × 100ms)

4. **Resultat** (RF_COMPLETE)
   - Ferme le solenoide, remet le servo au repos
   - Affiche les angles min et max detectes
   - L'utilisateur choisit **Appliquer** ou **Ignorer**
   - "Appliquer" ecrit les angles dans `cfg.servoAirflowMin` et `cfg.servoAirflowMax` et sauvegarde

### Pourquoi la note du milieu ?

Le Range Finder utilise une seule note pour le sweep. La note du milieu de la tessiture est choisie car elle represente un compromis typique en terme de debit d'air (ni trop aigu, ni trop grave). Les notes extremes pourront avoir des plages differentes, mais les angles globaux `air_min`/`air_max` doivent couvrir l'ensemble.

## Etape 2 : Auto-calibration par note

Une fois la plage globale definie (par le Range Finder ou manuellement), l'auto-calibration affine les pourcentages d'airflow pour chaque note individuellement.

### Deroulement

Pour chaque note, de `cfg.notes[0]` a `cfg.notes[numNotes - 1]` :

1. **Preparation** (PREPARE)
   - Positionne les doigts selon le pattern de la note courante
   - Met le servo airflow a l'angle `air_off` (position repos)
   - Ouvre le solenoide/valve

2. **Stabilisation** (SETTLE)
   - Attend 300ms (`AUTOCAL_SETTLE_MS`)

3. **Sweep** (SWEEP)
   - Incremente l'angle de **1 deg toutes les 80ms** (`AUTOCAL_STEP_MS`)
   - Plage de sweep : de `air_off` jusqu'a `air_max + 5` deg (`AUTOCAL_SWEEP_OVERSHOOT`)
   - A chaque pas :

   **Phase 1 — recherche du debut du son :**
   - Condition : RMS > 0.02 **ET** pitch a ±3 demi-tons de la note MIDI attendue
   - Quand detecte → `air_min% = angleToPct(angle)`

   **Phase 2 — recherche de la fin du son :**
   - Condition : 3 lectures consecutives sans son valide (silence ou mauvais pitch)
   - Quand detecte → `air_max% = angleToPct(angle - 3)` (retranche les 3 pas silencieux)
   - Garantie : `air_max >= air_min + 5%` (`AUTOCAL_MIN_RANGE_PCT`)

   **Fin de plage atteinte sans fin de son :**
   - Si le sweep depasse `air_max + 5` sans avoir detecte de fin → `air_max% = 100%`

4. **Resultat note** (NOTE_DONE)
   - Ferme le solenoide
   - Stocke le resultat : `valid = true` si le debut du son a ete trouve, sinon `false`
   - Attend 200ms (`AUTOCAL_NOTE_INTERVAL_MS`) avant la note suivante

5. **Fin** (COMPLETE)
   - Applique les resultats : pour chaque note valide, ecrit `airflowMinPercent` et `airflowMaxPercent`
   - Sauvegarde sur LittleFS
   - Les notes marquees "FAIL" ne sont pas modifiees (la config precedente est conservee)

### Duree typique

Pour chaque note, le sweep dure entre 2 et 10 secondes selon la plage. Avec les pauses de stabilisation et entre notes, compter **~5-10 secondes par note**. Pour 14 notes : environ **1 a 2 minutes**.

## Detection audio

### Echantillonnage

- Bus I2S en mode standard Philips (ESP-IDF 5.x `i2s_std`)
- Frequence : 16 kHz, 32-bit, mono canal gauche
- Buffer DMA : 1024 echantillons
- Normalisation : samples 32-bit → float [-1.0, +1.0]

### Analyse (~25 Hz, toutes les 40ms)

**Niveau sonore (RMS)** :
- Racine de la moyenne des carres des echantillons
- Seuil de detection : `MIC_RMS_THRESHOLD = 0.02` (silence en dessous)

**Detection de hauteur (YIN simplifie)** :
- Algorithme YIN avec interpolation parabolique pour precision sub-echantillon
- Plage detectable : 200 Hz a 4000 Hz (~G3 a B7)
- Seuil de confiance : `MIC_YIN_THRESHOLD = 0.15`
- Conversion Hz → note MIDI : `69 + 12 × log2(hz / 440)`
- Tolerance auto-cal : ±3 demi-tons (`AUTOCAL_PITCH_TOLERANCE_SEMI`)

### Critere de detection du son

Le son est considere comme **present et correct** quand les **deux** conditions sont remplies simultanement :
1. RMS > 0.02 (il y a du son)
2. Le pitch detecte est a ±3 demi-tons de la note MIDI attendue (c'est la bonne note)

Si seule la condition 1 est remplie (du bruit mais pas la bonne note), ce n'est pas compte comme un son valide.

## Diagnostics

| Symptome | Cause probable | Solution |
|----------|---------------|----------|
| Note marquee **FAIL** | Pas de son detecte dans la plage | Verifier le doigte de cette note, la position du micro, la pression d'air |
| Pitch detecte faux (mauvaise note) | Doigte incorrect, fuite d'air | Corriger le pattern de doigts dans la table des doigtes |
| Range finder `min = 0` | Bruit ambiant capte par le micro | Reduire le bruit, eloigner les sources de parasites |
| Toutes les notes FAIL | Systeme d'air inoperant ou micro muet | Tester le solenoide manuellement, verifier le cablage micro |
| air_min% tres proche de air_max% | Plage utile tres etroite pour cette note | Normal pour certaines notes; verifier le positionnement de la flute |
| Range finder ne detecte rien | Le servo airflow ne couvre pas la plage utile | Verifier le branchement du servo, tester les angles manuellement |

## Parametres

Constantes definies dans `settings.h` :

| Constante | Defaut | Description |
|-----------|--------|-------------|
| `MIC_SAMPLE_RATE` | 16000 | Frequence d'echantillonnage I2S (Hz) |
| `MIC_BUFFER_SIZE` | 1024 | Taille du buffer d'analyse (samples) |
| `MIC_RMS_THRESHOLD` | 0.02 | Seuil RMS pour "son detecte" |
| `MIC_PITCH_MIN_HZ` | 200 | Pitch minimum detectable (Hz) |
| `MIC_PITCH_MAX_HZ` | 4000 | Pitch maximum detectable (Hz) |
| `MIC_PITCH_TOLERANCE_CENTS` | 200 | Tolerance pitch monitoring UI (cents) |
| `MIC_YIN_THRESHOLD` | 0.15 | Seuil de confiance YIN |
| `AUTOCAL_SETTLE_MS` | 300 | Stabilisation servos (ms) |
| `AUTOCAL_STEP_MS` | 80 | Intervalle par pas — auto-cal per-note (ms) |
| `AUTOCAL_SILENCE_COUNT` | 3 | Lectures silencieuses pour valider fin de son |
| `AUTOCAL_SWEEP_OVERSHOOT` | 5 | Degres au-dela de air_max pendant le sweep |
| `AUTOCAL_PITCH_TOLERANCE_SEMI` | 3 | Tolerance pitch auto-cal (demi-tons) |
| `AUTOCAL_MIN_RANGE_PCT` | 5 | Ecart minimum garanti entre air_min% et air_max% |
| `AUTOCAL_NOTE_INTERVAL_MS` | 200 | Pause entre notes (ms) |
| `AUTOCAL_RF_STEP_MS` | 100 | Intervalle par pas — range finder (ms) |
| `AUTOCAL_RF_MARGIN_DEG` | 3 | Marge de securite range finder (deg) |
| `AUTOCAL_AUDIO_INTERVAL_MS` | 100 | Intervalle broadcast audio WS (ms) |

## Commandes WebSocket

| Commande | JSON | Description |
|----------|------|-------------|
| Monitoring micro | `{"t":"mic_mon","on":true}` | Active/desactive le flux audio temps reel |
| Range finder | `{"t":"auto_cal","mode":"range"}` | Demarre le sweep 0-180 deg |
| Appliquer range | `{"t":"auto_cal","mode":"apply_range"}` | Applique les angles detectes |
| Auto-calibration | `{"t":"auto_cal","mode":"air"}` | Demarre la calibration per-note |
| Stop | `{"t":"auto_cal","mode":"stop"}` | Arrete toute calibration en cours |

Pour le detail des messages serveur (progression, resultats), voir [API_WEB.md](API_WEB.md).
