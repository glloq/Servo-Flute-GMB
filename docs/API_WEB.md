# Web API

The ESP32 exposes a REST API and a WebSocket endpoint used by the embedded web UI.

## REST endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Main single-page web interface |
| GET | `/api/status` | Runtime status JSON |
| GET | `/api/config` | Full runtime configuration |
| POST | `/api/config` | Partial configuration update |
| POST | `/api/config/reset` | Reset configuration to defaults |
| POST | `/api/config/factory` | Factory reset and reopen first-use wizard |
| POST | `/api/midi` | Upload a MIDI file |
| GET | `/api/midi/list` | List stored MIDI files |
| POST | `/api/midi/load` | Load an existing MIDI file |
| POST | `/api/midi/delete` | Delete a stored MIDI file |
| GET | `/api/wifi/scan` | Start WiFi scan |
| GET | `/api/wifi/results` | Poll WiFi scan results |
| POST | `/api/wifi/connect` | Save WiFi credentials and connect |
| GET | `/api/wifi/status` | Current WiFi state |

## WebSocket endpoint

`/ws` is used for real-time control and status updates.

Common client messages:

| Type | Example | Description |
|------|---------|-------------|
| `non` | `{"t":"non","n":82,"v":100}` | Note On |
| `nof` | `{"t":"nof","n":82}` | Note Off |
| `cc` | `{"t":"cc","c":7,"v":100}` | Control Change |
| `velocity` | `{"t":"velocity","v":100}` | Default keyboard velocity |
| `panic` | `{"t":"panic"}` | All sound off |
| `test_finger` | `{"t":"test_finger","i":0,"a":90}` | Move one finger servo |
| `test_note` | `{"t":"test_note","n":84}` | Test a full note position |
| `air_live` | `{"t":"air_live","v":50}` | Live airflow test |
| `pump_stop` | `{"t":"pump_stop"}` | Stop pumps |
| `fan_stop` | `{"t":"fan_stop"}` | Stop fan |

Server messages include status broadcasts, MIDI file events, audio-monitoring data, and auto-calibration progress.

### Microphone auto-calibration messages (server → client)

Client control messages: `{"t":"auto_cal","mode":"air"}` starts per-note airflow
calibration, `"mode":"range"` starts the servo range finder, `"mode":"stop"`
stops, `"mode":"apply_range"` applies range-finder results.

**Range-finder lifecycle.** When the range finder finishes, the server broadcasts
`rf_done` **once** and keeps the result *pending* (the calibrator stays in its
completed state). `apply_range` therefore remains valid until the owner applies,
cancels (`stop`), starts a new calibration, or disconnects. `rf_done` reports
`ok:false` with `reason`/`reasonName` (and `airError` for an air-supply failure)
when no usable range was found. Applying replies with `{"t":"rf_applied","ok":true,
"min":…,"max":…}` on success, or `{"t":"rf_applied","ok":false,"error":"…"}`
(`storage_failed` / `no_valid_range`) when nothing was written — the servo range is
storage-checked and rolled back on a LittleFS failure, exactly like the per-note
results.

Live progress (`acal_prog`), one per airflow position:

```json
{
  "t": "acal_prog", "idx": 0, "note": "C6", "total": 14,
  "phase": "coarse", "air": 32,
  "rms": 0.043, "noise": 0.006,
  "hz": 1046.5, "midi": 84, "cents": -3.2,
  "confidence": 91, "validFrames": 5, "totalFrames": 5
}
```

`phase` is one of `prepare`, `noise`, `coarse`, `fine`, `nominal`, `done`,
`range`.

Completion (`acal_done`) reports overall persistence status and one result per
note:

```json
{
  "t": "acal_done", "ok": true, "applied": true, "saved": true,
  "validCount": 12, "failedCount": 2,
  "results": [
    { "name": "C6", "ok": true, "min": 20, "nominal": 39, "max": 68,
      "confidence": 91, "cents": -3.2, "stability": 0.94, "snr": 18.5,
      "reason": 0, "reasonName": "none" }
  ]
}
```

- `ok` is `true` only when the results were both **applied and saved**
  (`applied && saved`). On a LittleFS write failure the RAM configuration is
  rolled back, so `applied` and `saved` are both `false`, `ok` is `false`, and an
  `"error":"storage_failed"` field is added — a client is never told a partial
  success while nothing was written.
- A failed note has `ok:false`, keeps its previous calibration, and reports both a
  numeric `reason` (`AutoCalFailureReason`: 0 none, 1 no-sound, 2 wrong-note,
  3 low-confidence, 6 no-stable-nominal, 7 audio-stale, 8 note-timeout,
  9 global-timeout, 10 air-supply-not-ready, …) and a textual `reasonName`
  (e.g. `"audio_stale"`).
- On a global-timeout abort the server sends `{"t":"acal_error","msg":"..."}`.
- The audio monitor stream (`{"t":"audio",...}`) additionally carries `conf`
  (0–100) and `valid`.

**Ownership / concurrency.** A calibration is owned by the WS client that
started it. A second `auto_cal` start returns `{"t":"acal_error","msg":
"calibration_busy"}`; a non-owner stop/apply returns `{"t":"error","msg":
"not_calibration_owner"}`. Actuator commands (`test_*`, `non/nof/cc`, `play`,
pump/fan targets **and `pump_stop`/`fan_stop`**, `mic_mon`, `mic_reset`) sent
during a calibration are refused with `{"t":"error","msg":"calibration_active"}`.
The player `stop` command, when a calibration is active, cancels the calibration
cleanly (owner-gated) instead of fighting it with all-sound-off. Every
config-mutating REST route — `POST /api/config`, `POST /api/config/reset` and
`POST /api/config/factory` — returns HTTP `409 {"ok":false,
"error":"calibration_active"}` while a calibration (including a pending
range-finder result) is active.

**Microphone.** `GET /api/config` exposes `mic_status` (`detected` / `all_zero`
/ `stuck` / `saturated` / `read_error` / `not_init`). `{"t":"mic_reset"}` re-probes
the microphone without rebooting and replies `{"t":"mic_reset","ok":...,
"status":"..."}`.

Each `notes[]` entry in `GET/POST /api/config` includes `anm` (nominal airflow
percent) next to `amn`/`amx`; it is derived from min/max when absent
(backward-compatible migration) and validated as `0 ≤ amn ≤ anm ≤ amx ≤ 100`.

## 2026 runtime safety and validation update

Configuration is validated by the firmware, not only by HTML controls. PCA channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only output pins, invalid min/max relationships, invalid MIDI channels, invalid fingering values, and unsafe pump/sensor bounds are detected before saving.

Parameters that change `pinMode()`, I2C/PCA routing, controller `begin()` behavior, sensor setup, or serial MIDI setup require a restart. Dynamic musical values such as CC defaults, note airflow/angle percentages, fingering patterns, and temporary fan/pump test targets can be applied without hardware reinitialization.

Manual hardware tests must always be time-limited and followed by a safe state. In software-only validation, hardware procedures are documented but marked `NOT TESTED — requires hardware` in `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.


## JSON serialization and diagnostics contract

REST and WebSocket payloads that include user-provided strings are serialized with ArduinoJson so escaping is correct for examples such as `deviceName = Flute "A"`, `SSID = atelier\wifi`, and `fichier = étude "test".mid`.

`GET /api/diagnostics` is passive: it reports `ok`, `warning`, `error`, `not_tested`, or `not_applicable` without moving actuators. `POST /api/diagnostics/run` is reserved for an explicit active, timeout-bounded hardware sequence that announces the tested components, supports stop/panic, returns outputs to a safe state, and keeps physical checks marked `NOT TESTED — requires hardware` until executed on real hardware.

## Post-audit API safety contract

`GET /api/diagnostics` is passive and must not move hardware. Active diagnostics must be requested explicitly, bounded by firmware timeouts, cancellable, and associated with the owning client. Variable strings in JSON responses must be emitted through a JSON serializer to preserve escaping for SSIDs, filenames, device names and error text.

Configuration reset and factory reset responses must report `applied:false` and `restart_required:true` after stopping active notes and manual tests; they must not claim the defaults are already active until reboot.
