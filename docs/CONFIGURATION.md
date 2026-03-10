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

### Solenoide PWM

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| PWM activation | `sol_act` | 0-255 | 255 | PWM a l'ouverture (pleine puissance) |
| PWM maintien | `sol_hold` | 0-255 | 128 | PWM apres stabilisation (economie chaleur) |
| Temps activation | `sol_time` | ms | 50 | Duree avant passage en maintien |

### Doigts

| Parametre | JSON key | Description |
|-----------|----------|-------------|
| Angle ouverture | `angle_open` | Angle d'ouverture par rapport a la position fermee (deg) |
| Angles fermes | `fingers[].a` | Angle servo en position fermee pour chaque doigt |
| Directions | `fingers[].d` | Sens d'ouverture (+1 ou -1) |

### Airflow par note

Chaque note a un pourcentage min et max de la plage airflow :

| JSON key | Description |
|----------|-------------|
| `notes_air[].mn` | Pourcentage min (0-100) |
| `notes_air[].mx` | Pourcentage max (0-100) |

### WiFi

| Parametre | JSON key | Description |
|-----------|----------|-------------|
| SSID | `wifi_ssid` | SSID du reseau WiFi (vide = rester en AP) |
| Mot de passe | `wifi_pass` | Mot de passe WiFi |

### Power

| Parametre | JSON key | Type | Defaut | Description |
|-----------|----------|------|--------|-------------|
| Timeout inactivite | `time_unpower` | ms | 200 | Coupe les servos apres inactivite |

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
| `AUTOCAL_AUDIO_INTERVAL_MS` | 100 | Intervalle broadcast audio WS (ms) |

Pour desactiver completement le support micro, mettre `MIC_ENABLED` a `false` dans `settings.h`. Cela exclut tout le code audio/auto-calibration a la compilation.

## Fichier JSON

Exemple de `/config.json` sur LittleFS :

```json
{
  "midi_ch": 0,
  "servo_delay": 105,
  "valve_interval": 50,
  "min_note_dur": 10,
  "air_off": 20,
  "air_min": 60,
  "air_max": 100,
  "vib_freq": 6.0,
  "vib_amp": 8.0,
  "cc2_on": true,
  "cc2_thr": 10,
  "cc2_curve": 1.4,
  "cc2_timeout": 1000,
  "sol_act": 255,
  "sol_hold": 128,
  "sol_time": 50,
  "angle_open": 30,
  "fingers": [
    {"a": 90, "d": -1},
    {"a": 95, "d": 1},
    {"a": 90, "d": 1},
    {"a": 100, "d": 1},
    {"a": 95, "d": -1},
    {"a": 90, "d": 1}
  ],
  "notes_air": [
    {"mn": 10, "mx": 60},
    {"mn": 0, "mx": 50}
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

## Adapter a un autre instrument

Pour changer le nombre de trous/notes, modifier dans `settings.h` :

1. `NUMBER_SERVOS_FINGER` - nombre de servos doigts
2. `NUMBER_NOTES` - nombre de notes jouables
3. Tableau `FINGERS[]` - canaux PCA, angles, directions
4. Tableau `NOTES[]` - notes MIDI, patterns de doigtes, airflow

Le serveur web s'adapte automatiquement (boucles sur `NUMBER_SERVOS_FINGER` / `NUMBER_NOTES`).
L'interface web est entierement dynamique : clavier, calibration et config se construisent depuis `/api/config`.
