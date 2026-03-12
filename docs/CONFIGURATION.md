# Configuration

## Principe

Tous les parametres sont stockes dans la structure `RuntimeConfig cfg` (ConfigStorage.h).
Au demarrage, les valeurs par defaut de `settings.h` sont chargees, puis surchargees par `/config.json` sur LittleFS si le fichier existe.

Les modifications via l'interface web sont appliquees immediatement en memoire et sauvegardees sur LittleFS.

## Parametres disponibles

### Instrument

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Canal MIDI | `midi_ch` | 0-16 | 0 | 0 = omni (tous canaux) |
| Nom appareil | `device` | string | ServoFlute | Nom BLE et mDNS |
| Nombre de doigts | `num_fingers` | 1-31 | 6 | Nombre de servos doigts |
| Nombre de notes | `num_notes` | 1-64 | 14 | Nombre de notes jouables |
| Canal PCA airflow | `air_pca` | 0-31 | 15 | Canal PCA9685 du servo airflow |
| Type d'embouchure | `embouchure` | string | "trav" | Type d'embouchure ("trav", "bec", etc.) |

### Serial MIDI

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Active | `smidi_on` | bool | true | Active/desactive le MIDI serie |
| GPIO RX | `smidi_rx` | GPIO | 16 | Pin de reception MIDI (liste autorisee) |

### Timing

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Delai servo-solenoide | `servo_delay` | ms | 105 | Temps entre positionnement doigts et ouverture valve |
| Interval valve | `valve_interval` | ms | 50 | En dessous, la valve reste ouverte entre 2 notes |
| Duree min note | `min_note_dur` | ms | 10 | Ignore les notes plus courtes |

### Airflow

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Angle repos | `air_off` | deg | 20 | Angle servo quand aucune note |
| Angle min | `air_min` | deg | 60 | Angle minimum absolu |
| Angle max | `air_max` | deg | 100 | Angle maximum absolu |

### Servo Angle (embouchure traversiere)

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Active | `angle_on` | bool | false | Active/desactive le servo angle |
| Canal PCA | `ang_pca` | 0-31 | 14 | Canal PCA9685 du servo angle |
| Canal PCA (servo) | `angle_ch` | 0-31 | 14 | Canal PCA alternatif |
| Angle repos | `ang_off` | deg | 90 | Angle servo quand aucune note |
| Angle min | `ang_min` | deg | 70 | Angle minimum |
| Angle max | `ang_max` | deg | 110 | Angle maximum |

### Vibrato (CC1 Modulation)

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Frequence | `vib_freq` | Hz | 6.0 | Frequence du vibrato |
| Amplitude max | `vib_amp` | deg | 8.0 | Amplitude maximale a CC1=127 |

### CC defaults

| Parametre | JSON key | Type | Defaut |
|-----------|----------|------|--------|
| Volume CC7 | `cc_vol` | 0-127 | 127 |
| Expression CC11 | `cc_expr` | 0-127 | 127 |
| Modulation CC1 | `cc_mod` | 0-127 | 0 |
| Breath CC2 | `cc_breath` | 0-127 | 127 |
| Brightness CC74 | `cc_bright` | 0-127 | 64 |

### Breath Controller (CC2)

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Active | `cc2_on` | bool | true | Active/desactive CC2 |
| Seuil silence | `cc2_thr` | 0-127 | 10 | En dessous = silence |
| Courbe reponse | `cc2_curve` | float | 1.4 | Exposant (1.0=lineaire, >1=progressif) |
| Timeout | `cc2_timeout` | ms | 1000 | Fallback velocity si pas de CC2 |

### Expression airflow (attaque)

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Mode d'attaque | `air_atk_mode` | 0-2 | 0 | 0=stable, 1=overshoot, 2=undershoot |
| Offset attaque | `air_atk_off` | % | 20 | Ecart initial en % |
| Duree attaque | `air_atk_ms` | ms | 150 | Duree de transition |
| Reponse velocite | `air_vel_resp` | % | 50 | Influence de la velocite sur l'airflow |

### Solenoide / Valve

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| PWM activation | `sol_act` | 0-255 | 255 | PWM a l'ouverture (pleine puissance) |
| PWM maintien | `sol_hold` | 0-255 | 128 | PWM apres stabilisation (economie chaleur) |
| Temps activation | `sol_time` | ms | 50 | Duree avant passage en maintien |
| GPIO solenoide | `sol_pin` | GPIO | 13 | Pin de commande du solenoide |
| Inter-note | `sol_inter` | ms | 0 | Delai entre notes pour le solenoide |

### Doigts

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Angle ouverture | `angle_open` | deg | 30 | Angle d'ouverture par rapport a la position fermee |
| Demi-ouverture globale | `half_hole_pct` | 10-90 | 50 | % d'ouverture pour les positions "demi" (fallback global) |
| Angles fermes | `fingers[].a` | deg | 90 | Angle servo en position fermee pour chaque doigt |
| Canaux PCA | `fingers[].ch` | 0-31 | auto | Canal PCA9685 par doigt |
| Directions | `fingers[].d` | +1/-1 | -1 | Sens d'ouverture |
| Trou de pouce | `fingers[].th` | 0/1 | 0 | 1 = trou arriere (dessous de la flute) |
| Demi % per-doigt | `fingers[].hp` | 0-100 | 0 | Override demi-ouverture (0 = utiliser global) |

### Airflow par note

Chaque note contient ses parametres dans le tableau `notes[]` :

| JSON key | Description |
|----------|-------------|
| `notes[].midi` | Numero de note MIDI (0-127) |
| `notes[].amn` | Pourcentage airflow min (0-100) |
| `notes[].amx` | Pourcentage airflow max (0-100) |
| `notes[].ang` | Pourcentage angle servo (0-100) |
| `notes[].fp` | Pattern de doigtes (tableau : 0=ouvert, 1=ferme, 2=demi) |

### Systeme de distribution d'air (modulaire)

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Mode air | `air_mode` | 0-5 | 0 | Voir AIR_MANAGEMENT.md |
| Type de valve | `valve_type` | 0-1 | 0 | 0=solenoide, 1=servo |
| Canal PCA valve | `valve_ch` | 0-31 | 14 | Canal PCA du servo-valve |
| Angle ferme | `vlv_close` | deg | 0 | Angle fermeture servo-valve |
| Angle ouvert | `vlv_open` | deg | 90 | Angle ouverture servo-valve |
| Direction valve | `vlv_dir` | 0/1 | 0 | Sens d'ouverture |
| Type moteur | `motor_type` | 0-1 | 0 | 0=ventilateur, 1=pompe |
| Afficher air | `show_air` | bool | true | Afficher l'onglet Air dans l'interface |
| Format reservoir | `res_format` | string | "balloon" | Format du reservoir (ballon, soufflet...) |

### Ventilateur

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| GPIO fan | `fan_pin` | GPIO | 25 | Pin PWM du ventilateur |
| PWM min | `fan_min` | 0-255 | 80 | PWM minimum (demarrage) |
| PWM max | `fan_max` | 0-255 | 255 | PWM maximum |
| Idle % | `fan_idle_pct` | 0-100 | 20 | Vitesse idle (%) |
| Idle timeout | `fan_idle_timeout` | ms | 5000 | Delai avant passage en idle |

### Pompe(s)

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Nombre de pompes | `num_pumps` | 1-3 | 1 | Nombre de pompes en cascade |
| GPIO pompes | `pump_pins` | [GPIO] | [25,26,27] | Pins PWM des pompes |
| PWM min | `pump_mins` | [0-255] | [80,...] | PWM minimum par pompe |
| PWM max | `pump_maxs` | [0-255] | [255,...] | PWM maximum par pompe |
| Seuil cascade | `pump_cascade` | 0-100 | 70 | % avant d'activer la pompe suivante |
| Delai cascade | `pump_stagger` | ms | 200 | Delai entre activations |
| Hysteresis | `bb_hyst` | 0-50 | 5 | Hysteresis bang-bang |

### Capteur pression

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Type capteur | `sens_type` | 0-3 | 0 | 0=aucun, 1=analogique, 2=endstop meca, 3=Hall |
| Cible | `sens_target` | mm | 50 | Cible pression PID |
| Min | `sens_min` | mm | 10 | Lecture min capteur |
| Max | `sens_max` | mm | 200 | Lecture max capteur |
| PID Kp | `pid_kp` | float | 2.0 | Gain proportionnel |
| PID Ki | `pid_ki` | float | 0.5 | Gain integral |
| GPIO endstop | `endstop_pin` | GPIO | 34 | Pin endstop |
| Endstop actif haut | `endstop_high` | bool | true | Polarite du endstop |
| Endstop pompe on | `endstop_pump_on` | bool | false | Garder pompe active sur endstop |
| GPIO Hall | `hall_pin` | GPIO | 36 | Pin capteur Hall |
| Hall seuil bas | `hall_low` | 0-4095 | 1500 | Seuil analogique bas |
| Hall seuil haut | `hall_high` | 0-4095 | 2500 | Seuil analogique haut |

### WiFi

| Parametre | JSON key | Description |
|-----------|----------|-------------|
| SSID | `wifi_ssid` | SSID du reseau WiFi (vide = rester en AP) |
| Mot de passe | `wifi_pass` | Mot de passe WiFi |

### Interface utilisateur

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Masquer calibration | `hide_calib` | bool | false | Masquer l'onglet calibration |
| Masquer air | `hide_air` | bool | false | Masquer l'onglet air |
| Mode clavier | `kbd_mode` | 0-2 | 0 | Mode d'affichage du clavier |
| Couleur instrument | `color` | hex | #D4B044 | Couleur d'accent dans l'interface |

### Power

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Timeout inactivite | `time_unpower` | ms | 200 | Coupe les servos apres inactivite |

### Stockage MIDI

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Limite stockage | `midi_limit` | KB | 512 | Espace max pour fichiers MIDI sur LittleFS |

### Microphone INMP441 (optionnel)

Ces constantes sont definies dans `settings.h` et ne sont pas modifiables via l'interface web :

| Constante | Defaut | Description |
|-----------|--------|-------------|
| `MIC_ENABLED` | true | Active/desactive la compilation du code micro |
| `MIC_PIN_BCLK` | 14 | GPIO pour I2S bit clock |
| `MIC_PIN_LRCLK` | 15 | GPIO pour I2S word select |
| `MIC_PIN_DIN` | 32 | GPIO pour I2S data in |
| `MIC_SAMPLE_RATE` | 16000 | Frequence d'echantillonnage (Hz) |
| `MIC_BUFFER_SIZE` | 1024 | Taille du buffer d'analyse (samples) |
| `MIC_RMS_THRESHOLD` | 0.02 | Seuil de detection de son (RMS) |
| `MIC_PITCH_MIN_HZ` | 200 | Frequence min detectable (Hz) |
| `MIC_PITCH_MAX_HZ` | 4000 | Frequence max detectable (Hz) |
| `MIC_PITCH_TOLERANCE_CENTS` | 200 | Tolerance pitch pour auto-cal (cents) |
| `MIC_YIN_THRESHOLD` | 0.15 | Seuil de confiance algorithme YIN |
| `AUTOCAL_SETTLE_MS` | 300 | Delai stabilisation servos (ms) |
| `AUTOCAL_STEP_MS` | 80 | Intervalle entre pas de sweep (ms) |
| `AUTOCAL_SILENCE_COUNT` | 3 | Lectures silencieuses pour valider fin de son |
| `AUTOCAL_RF_STEP_MS` | 100 | Intervalle entre pas du range finder (ms) |
| `AUTOCAL_RF_MARGIN_DEG` | 3 | Marge de securite range finder (deg) |
| `AUTOCAL_AUDIO_INTERVAL_MS` | 100 | Intervalle broadcast audio WS (ms) |

Pour desactiver completement le support micro, mettre `MIC_ENABLED` a `false` dans `settings.h`. Cela exclut tout le code audio/auto-calibration a la compilation.

## Fichier JSON

Exemple de `/config.json` sur LittleFS :

```json
{
  "num_fingers": 6,
  "num_notes": 14,
  "air_pca": 15,
  "angle_open": 30,
  "half_hole_pct": 50,
  "embouchure": "trav",
  "midi_ch": 0,
  "smidi_on": 1,
  "smidi_rx": 16,
  "servo_delay": 105,
  "valve_interval": 50,
  "min_note_dur": 10,
  "air_off": 20,
  "air_min": 60,
  "air_max": 100,
  "ang_pca": 14,
  "ang_off": 90,
  "ang_min": 70,
  "ang_max": 110,
  "vib_freq": 6.0,
  "vib_amp": 8.0,
  "cc2_on": 1,
  "cc2_thr": 10,
  "cc2_curve": 1.4,
  "cc2_timeout": 1000,
  "sol_act": 255,
  "sol_hold": 128,
  "sol_time": 50,
  "air_atk_mode": 0,
  "air_atk_off": 20,
  "air_atk_ms": 150,
  "air_vel_resp": 50,
  "air_mode": 0,
  "valve_type": 0,
  "valve_ch": 14,
  "vlv_close": 0,
  "vlv_open": 90,
  "vlv_dir": 0,
  "sol_inter": 0,
  "motor_type": 0,
  "fan_pin": 25,
  "fan_min": 80,
  "fan_max": 255,
  "fan_idle_pct": 20,
  "fan_idle_timeout": 5000,
  "num_pumps": 1,
  "pump_pins": [25, 26, 27],
  "pump_mins": [80, 80, 80],
  "pump_maxs": [255, 255, 255],
  "pump_cascade": 70,
  "pump_stagger": 200,
  "bb_hyst": 5,
  "sens_type": 0,
  "sens_target": 50,
  "sens_min": 10,
  "sens_max": 200,
  "pid_kp": 2.0,
  "pid_ki": 0.5,
  "endstop_pin": 34,
  "endstop_high": 1,
  "endstop_pump_on": 0,
  "hall_pin": 36,
  "hall_low": 1500,
  "hall_high": 2500,
  "angle_on": 0,
  "angle_ch": 14,
  "show_air": 1,
  "res_format": "balloon",
  "midi_limit": 512,
  "hide_calib": 0,
  "hide_air": 0,
  "sol_pin": 13,
  "kbd_mode": 0,
  "color": "#D4B044",
  "fingers": [
    {"ch": 0, "a": 90, "d": -1, "th": 1, "hp": 25},
    {"ch": 1, "a": 95, "d": 1},
    {"ch": 2, "a": 90, "d": 1},
    {"ch": 3, "a": 100, "d": 1},
    {"ch": 4, "a": 95, "d": -1},
    {"ch": 5, "a": 90, "d": 1}
  ],
  "notes": [
    {"midi": 82, "amn": 10, "amx": 60, "ang": 50, "fp": [0,1,1,1,1,1]},
    {"midi": 83, "amn": 0, "amx": 50, "ang": 50, "fp": [1,1,1,1,1,1]}
  ],
  "wifi_ssid": "MonReseau",
  "wifi_pass": "motdepasse",
  "device": "ServoFlute",
  "time_unpower": 200
}
```

### Detection du micro

Le champ `mic` dans la reponse de `GET /api/config` indique si un micro INMP441 a ete detecte au demarrage :
- `"mic": true` - micro present, section auto-calibration visible dans l'interface web
- `"mic": false` - micro absent, interface web normale sans auto-calibration

La detection se fait automatiquement au boot : le firmware lit des echantillons I2S et verifie si >10% sont non-nuls. Un bus I2S sans micro connecte retourne uniquement des zeros.

### Premier demarrage

Le champ `first_boot` dans `GET /api/config` indique si c'est le premier demarrage (pas de fichier `/config.json`). L'interface web utilise ce flag pour afficher le wizard de configuration initiale.

## Adapter a un autre instrument

Pour changer le nombre de trous/notes, modifier dans `settings.h` :

1. `NUMBER_SERVOS_FINGER` - nombre de servos doigts
2. `NUMBER_NOTES` - nombre de notes jouables
3. Tableau `FINGERS[]` - canaux PCA, angles, directions
4. Tableau `NOTES[]` - notes MIDI, patterns de doigtes, airflow

Le serveur web s'adapte automatiquement (boucles sur `NUMBER_SERVOS_FINGER` / `NUMBER_NOTES`).
L'interface web est entierement dynamique : clavier, calibration et config se construisent depuis `/api/config`.
