# API Web

## Endpoints REST

### GET /

Page web principale (SPA). HTML/CSS/JS embarque en PROGMEM (~100KB).

### GET /api/status

Retourne l'etat courant en JSON.

```json
{
  "mode": "WiFi AP: 192.168.4.1",
  "connected": false,
  "uptime": 3600,
  "cc7": 127, "cc11": 127, "cc1": 0, "cc2": 127,
  "player": {
    "state": 0,
    "loaded": true,
    "file": "song.mid",
    "events": 450,
    "duration": 120000,
    "position": 0,
    "progress": 0.0
  }
}
```

### GET /api/config

Retourne la configuration complete. Inclut les tableaux dynamiques `fingers` et `notes` qui permettent a l'interface web de s'adapter au nombre de doigts/notes de l'instrument.

Le champ `mic` indique si un microphone INMP441 a ete detecte au demarrage. L'interface web utilise ce champ pour afficher ou masquer la section auto-calibration.

```json
{
  "num_fingers": 6,
  "num_notes": 14,
  "air_pca": 15,
  "angle_open": 30,
  "half_hole_pct": 50,
  "embouchure": "trav",
  "midi_ch": 0,
  "smidi_on": true,
  "smidi_rx": 16,
  "servo_delay": 105,
  "valve_interval": 50,
  "min_note_dur": 10,
  "air_off": 20, "air_min": 60, "air_max": 100,
  "ang_pca": 14, "ang_off": 90, "ang_min": 70, "ang_max": 110,
  "vib_freq": 6.0, "vib_amp": 8.0,
  "cc_vol": 127, "cc_expr": 127, "cc_mod": 0, "cc_breath": 127, "cc_bright": 64,
  "cc2_on": true, "cc2_thr": 10, "cc2_curve": 1.40, "cc2_timeout": 1000,
  "sol_act": 255, "sol_hold": 128, "sol_time": 50,
  "device": "ServoFlute",
  "wifi_ssid": "MonReseau",
  "time_unpower": 200,
  "hide_calib": false, "hide_air": false,
  "sol_pin": 13, "kbd_mode": 0,
  "color": "#D4B044",
  "air_atk_mode": 0, "air_atk_off": 20, "air_atk_ms": 150,
  "air_vel_resp": 50,
  "air_mode": 0, "valve_type": 0,
  "valve_ch": 14, "vlv_close": 0, "vlv_open": 90, "vlv_dir": 0,
  "sol_inter": 0,
  "motor_type": 0,
  "fan_pin": 25, "fan_min": 80, "fan_max": 255,
  "fan_idle_pct": 20, "fan_idle_timeout": 5000,
  "num_pumps": 1,
  "pump_pins": [25, 26, 27],
  "pump_mins": [80, 80, 80],
  "pump_maxs": [255, 255, 255],
  "pump_cascade": 70, "pump_stagger": 200,
  "bb_hyst": 5,
  "sens_type": 0, "sens_target": 50, "sens_min": 10, "sens_max": 200,
  "pid_kp": 2.0, "pid_ki": 0.5,
  "endstop_pin": 34, "endstop_high": true, "endstop_pump_on": false,
  "hall_pin": 36, "hall_low": 1500, "hall_high": 2500,
  "angle_on": false, "angle_ch": 14,
  "show_air": true,
  "res_format": "balloon",
  "midi_limit": 512,
  "mic": true,
  "first_boot": false,
  "fingers": [
    {"ch": 0, "a": 90, "d": -1, "th": 1, "hp": 25},
    {"ch": 1, "a": 95, "d": 1}
  ],
  "notes": [
    {"midi": 82, "amn": 10, "amx": 60, "ang": 50, "fp": [0,1,1,1,1,1]},
    {"midi": 83, "amn": 0, "amx": 50, "ang": 50, "fp": [1,1,1,1,1,1]}
  ]
}
```

**Notes sur les champs `notes[]` :**
- `amn` / `amx` : pourcentage airflow min/max (0-100) pour cette note
- `ang` : pourcentage angle servo (0-100) pour cette note
- `fp` : pattern de doigtes (0=ouvert, 1=ferme, 2=demi)

### POST /api/config

Modifie la configuration. Accepte un JSON partiel (seuls les champs presents sont mis a jour).

**Request :**
```json
{
  "air_min": 65,
  "vib_freq": 5.0,
  "fingers": [{"a": 92, "d": -1}, {"a": 93, "d": 1}],
  "notes": [{"midi": 82, "amn": 15, "amx": 65, "ang": 50, "fp": [0,1,1,1,1,1]}]
}
```

**Response :**
```json
{"ok": true}
```

### POST /api/config/reset

Remet tous les parametres aux valeurs par defaut de `settings.h` et sauvegarde.

**Response :**
```json
{"ok": true}
```

### POST /api/config/factory

Reset usine : supprime le fichier config pour relancer le wizard de premiere configuration au prochain demarrage.

**Response :**
```json
{"ok": true}
```

### POST /api/midi

Upload d'un fichier MIDI (multipart/form-data). Max 100KB. Supporte SMF Type 0 et Type 1.

**Response succes :**
```json
{"ok": true, "events": 450, "duration": 120000, "file": "song.mid"}
```

**Response erreur :**
```json
{"ok": false, "msg": "Format MIDI invalide"}
```

### GET /api/midi/list

Liste les fichiers MIDI stockes sur LittleFS.

**Response :**
```json
{"files": ["song.mid", "test.mid"], "total": 2}
```

### POST /api/midi/load

Charge un fichier MIDI deja stocke pour le playback.

**Request :**
```json
{"file": "song.mid"}
```

**Response :**
```json
{"ok": true, "events": 450, "duration": 120000}
```

### POST /api/midi/delete

Supprime un fichier MIDI du stockage LittleFS.

**Request :**
```json
{"file": "song.mid"}
```

**Response :**
```json
{"ok": true}
```

### GET /api/wifi/scan

Lance un scan WiFi asynchrone. Retourne immediatement.

```json
{"ok": true, "msg": "Scan lance"}
```

### GET /api/wifi/results

Recupere les resultats du scan. Appeler en polling (toutes les 1.5s) jusqu'a `done: true`.

**Scan en cours :**
```json
{"done": false}
```

**Scan termine :**
```json
{
  "done": true,
  "networks": [
    {"ssid": "MaBox", "rssi": -45, "enc": 1},
    {"ssid": "Voisin", "rssi": -72, "enc": 1},
    {"ssid": "OpenWifi", "rssi": -80, "enc": 0}
  ]
}
```

`enc`: 1 = protege, 0 = ouvert

### POST /api/wifi/connect

Connexion a un reseau WiFi. Les credentials sont sauvegardes dans la config et persistes.

**Request :**
```json
{"ssid": "MaBox", "pass": "motdepasse"}
```

**Response :**
```json
{"ok": true, "msg": "Connexion en cours..."}
```

Note : apres connexion STA, l'ESP32 quitte le mode AP. L'adresse IP change vers celle attribuee par le routeur. Accessible ensuite via `servo-flute.local`.

### GET /api/wifi/status

Etat WiFi courant.

```json
{
  "state": 2,
  "ip": "192.168.1.42",
  "ap": false,
  "ssid": "MaBox",
  "rssi": -52
}
```

States : 0=deconnecte, 1=connexion en cours, 2=STA connecte, 3=AP actif

Le champ `rssi` n'est present qu'en mode STA connecte.

### Captive Portal

En mode AP, les requetes vers les URLs de detection de portail captif sont automatiquement redirigees vers `http://192.168.4.1/` :

- `/generate_204` (Android)
- `/hotspot-detect.html` (iOS/macOS)
- `/connecttest.txt` (Windows)
- `/ncsi.txt` (Windows)
- `/redirect`

Toute requete 404 en mode AP est egalement redirigee vers la page principale.

## WebSocket /ws

Connexion persistante pour le controle temps reel et le monitoring.

### Messages Client -> Serveur

| Type | Format | Description |
|------|--------|-------------|
| Note On | `{"t":"non","n":82,"v":100}` | Jouer une note |
| Note Off | `{"t":"nof","n":82}` | Arreter une note |
| Control Change | `{"t":"cc","c":7,"v":100}` | Envoyer un CC |
| Velocity | `{"t":"velocity","v":100}` | Changer velocity par defaut |
| Airflow live | `{"t":"air_live","v":75}` | Controle direct airflow (%) |
| Angle live | `{"t":"angle_live","v":50}` | Controle direct angle servo (%) |
| Play | `{"t":"play"}` | Demarrer playback MIDI |
| Pause | `{"t":"pause"}` | Pause playback |
| Stop | `{"t":"stop"}` | Arreter playback |
| Channel filter | `{"t":"ch_filter","ch":0}` | Filtrer canal MIDI (0-15, >15 = tous) |
| Panic | `{"t":"panic"}` | All Sound Off |
| Test doigt | `{"t":"test_finger","i":0,"a":90}` | Positionner servo doigt |
| Test airflow | `{"t":"test_air","a":60}` | Positionner servo airflow |
| Test angle | `{"t":"test_angle","a":90}` | Positionner servo angle (embouchure) |
| Test solenoide | `{"t":"test_sol","o":1}` | Ouvrir/fermer solenoide |
| Test note | `{"t":"test_note","n":84}` | Position complete pour une note (doigts + airflow) |
| Pompe cible | `{"t":"pump_target","v":80}` | Definir cible pompe (%) |
| Pompe stop | `{"t":"pump_stop"}` | Arreter la pompe |
| Pompe enable | `{"t":"pump_enable","v":1}` | Activer/desactiver pompe (0=stop) |
| Fan cible | `{"t":"fan_target","v":60}` | Definir vitesse ventilateur (%) |
| Fan stop | `{"t":"fan_stop"}` | Arreter le ventilateur |
| Monitoring micro | `{"t":"mic_mon","on":true}` | Activer/desactiver le flux audio |
| Auto-calibration | `{"t":"auto_cal","mode":"air"}` | Demarrer auto-cal airflow |
| Range finder | `{"t":"auto_cal","mode":"range"}` | Detecter plage servo (sweep 0-180) |
| Appliquer range | `{"t":"auto_cal","mode":"apply_range"}` | Appliquer resultats range finder |
| Stop auto-cal | `{"t":"auto_cal","mode":"stop"}` | Arreter auto-cal en cours |

### Messages Serveur -> Client

**Status broadcast (toutes les 500ms) :**
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

**MIDI fichier charge :**
```json
{"t": "midi_loaded", "file": "song.mid", "events": 450, "duration": 120000}
```

**Erreur MIDI :**
```json
{"t": "midi_error", "msg": "Format MIDI invalide"}
```

**Audio monitoring (si micro INMP441 detecte et monitoring actif, toutes les 100ms) :**
```json
{
  "t": "audio",
  "rms": 0.12,
  "hz": 440.0,
  "midi": 69,
  "cents": -5
}
```

- `rms` : niveau sonore RMS (0.0 - 1.0)
- `hz` : frequence detectee en Hz (0 si pas de son)
- `midi` : note MIDI la plus proche (0 si pas de pitch)
- `cents` : ecart en cents par rapport a la note MIDI (typiquement -50 a +50, peut etre plus large aux limites de detection)

**Progression auto-calibration :**
```json
{
  "t": "acal_prog",
  "idx": 3,
  "note": "C6",
  "total": 14,
  "angle": 75,
  "st": 3
}
```

- `idx` : index de la note en cours (0-based)
- `note` : nom de la note (ex: "C6", "F#5")
- `total` : nombre total de notes
- `st` : etat de la machine (0=idle, 1=prepare, 2=settle, 3=sweep, 4=note_done, 5=complete)

**Resultats auto-calibration :**
```json
{
  "t": "acal_done",
  "ok": true,
  "results": [
    {"name": "A#5", "ok": true, "min": 15, "max": 65, "minA": 72, "maxA": 95},
    {"name": "B5", "ok": true, "min": 10, "max": 58, "minA": 68, "maxA": 92},
    {"name": "C6", "ok": false, "min": 0, "max": 0, "minA": 60, "maxA": 60}
  ]
}
```

- `results[].ok` : `true` si la calibration a reussi pour cette note
- `results[].min` / `max` : pourcentages airflow detectes (0-100)
- `results[].minA` / `maxA` : angles servo correspondants (degres)

**Progression range finder :**
```json
{"t": "rf_prog", "angle": 85, "st": 8, "min": 42}
```

- `angle` : angle servo courant (0-180)
- `st` : etat machine (6=prepare, 7=settle, 8=sweep)
- `min` : angle minimum detecte (present seulement si deja trouve)

**Resultat range finder :**
```json
{"t": "rf_done", "ok": true, "min": 42, "max": 135}
```

- `ok` : `true` si du son a ete detecte
- `min` / `max` : angles servo detectes (degres)

**Range finder applique :**
```json
{"t": "rf_applied", "min": 42, "max": 135}
```

Confirme que les valeurs `air_min` / `air_max` ont ete mises a jour dans la configuration.
