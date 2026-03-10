# API Web

## Endpoints REST

### GET /

Page web principale (SPA). HTML/CSS/JS embarque en PROGMEM (~36KB).

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
  "midi_ch": 0,
  "servo_delay": 105,
  "valve_interval": 50,
  "min_note_dur": 10,
  "air_off": 20, "air_min": 60, "air_max": 100,
  "vib_freq": 6.0, "vib_amp": 8.0,
  "cc_vol": 127, "cc_expr": 127, "cc_mod": 0, "cc_breath": 127, "cc_bright": 64,
  "cc2_on": true, "cc2_thr": 10, "cc2_curve": 1.40, "cc2_timeout": 1000,
  "sol_act": 255, "sol_hold": 128, "sol_time": 50,
  "angle_open": 30,
  "device": "ServoFlute",
  "wifi_ssid": "MonReseau",
  "time_unpower": 200,
  "num_notes": 14,
  "num_fingers": 6,
  "mic": true,
  "fingers": [
    {"ch": 0, "a": 90, "d": -1},
    {"ch": 1, "a": 95, "d": 1}
  ],
  "notes": [
    {"midi": 82, "air_min": 10, "air_max": 60, "fingers": [0,1,1,1,1,1]},
    {"midi": 83, "air_min": 0, "air_max": 50, "fingers": [1,1,1,1,1,1]}
  ]
}
```

### POST /api/config

Modifie la configuration. Accepte un JSON partiel (seuls les champs presents sont mis a jour).

**Request :**
```json
{
  "air_min": 65,
  "vib_freq": 5.0,
  "fingers": [{"a": 92, "d": -1}, {"a": 93, "d": 1}],
  "notes_air": [{"mn": 15, "mx": 65}]
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

## WebSocket /ws

Connexion persistante pour le controle temps reel et le monitoring.

### Messages Client -> Serveur

| Type | Format | Description |
|------|--------|-------------|
| Note On | `{"t":"non","n":82,"v":100}` | Jouer une note |
| Note Off | `{"t":"nof","n":82}` | Arreter une note |
| Control Change | `{"t":"cc","c":7,"v":100}` | Envoyer un CC |
| Velocity | `{"t":"velocity","v":100}` | Changer velocity par defaut |
| Play | `{"t":"play"}` | Demarrer playback MIDI |
| Pause | `{"t":"pause"}` | Pause playback |
| Stop | `{"t":"stop"}` | Arreter playback |
| Panic | `{"t":"panic"}` | All Sound Off |
| Test doigt | `{"t":"test_finger","i":0,"a":90}` | Positionner servo doigt |
| Test airflow | `{"t":"test_air","a":60}` | Positionner servo airflow |
| Test solenoide | `{"t":"test_sol","o":1}` | Ouvrir/fermer solenoide |
| Test note | `{"t":"test_note","n":84}` | Position complete pour une note |
| Monitoring micro | `{"t":"mic_mon","on":true}` | Activer/desactiver le flux audio |
| Auto-calibration | `{"t":"auto_cal","mode":"air"}` | Demarrer auto-cal airflow |
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
    {"name": "A#5", "ok": true, "min": 15, "max": 65},
    {"name": "B5", "ok": true, "min": 10, "max": 58},
    {"name": "C6", "ok": false, "min": 0, "max": 0}
  ]
}
```

- `results[].ok` : `true` si la calibration a reussi pour cette note
- `results[].min` / `max` : pourcentages airflow detectes (0-100)
