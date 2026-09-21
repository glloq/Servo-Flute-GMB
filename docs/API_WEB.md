# Web API

The ESP32 exposes a REST API and a WebSocket endpoint used by the embedded web UI.

## REST endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/` | public | Main single-page web interface |
| GET | `/api/auth/status` | public | Whether the caller holds a valid session |
| POST | `/api/auth/login` | public | Exchange the admin password for a session token |
| POST | `/api/auth/password` | token | Change the admin password |
| POST | `/api/auth/hotspot` | token | Regenerate the hotspot WPA2 key |
| GET | `/api/status` | public | Runtime status JSON |
| GET | `/api/diagnostics` | public | Passive hardware / configuration diagnostics |
| POST | `/api/diagnostics/run` | public | Same passive report (no actuator movement) |
| GET | `/api/config` | public | Full runtime configuration (carries no secret) |
| POST | `/api/config` | token | Partial configuration update (transactional) |
| POST | `/api/config/reset` | token | Reset configuration to defaults |
| POST | `/api/config/factory` | token | Factory reset and reopen first-use wizard |
| POST | `/api/fs/format` | token | Recovery: erase and re-create LittleFS (explicit confirmation) |
| POST | `/api/restart` | token | Safe the hardware and restart |
| POST | `/api/midi` | token | Upload a MIDI file |
| GET | `/api/midi/list` | token | List stored MIDI files |
| POST | `/api/midi/load` | token | Load an existing MIDI file |
| POST | `/api/midi/delete` | token | Delete a stored MIDI file |
| GET | `/api/wifi/scan` | token | Start WiFi scan |
| GET | `/api/wifi/results` | token | Poll WiFi scan results |
| POST | `/api/wifi/connect` | token | Save WiFi credentials and connect |
| GET | `/api/wifi/status` | public | Current WiFi state |
| GET | `/gmb/descriptor.json` | public | General-Midi-Boop v2 capability descriptor |

### `GET /gmb/descriptor.json`

Returns the General-Midi-Boop v2 capability descriptor for the active
configuration, as `application/json`. It is byte-for-byte the document served
over the SysEx block `0x10` transfer — both read the same cached string, so the
two can never diverge. The route exists only in Wi-Fi mode, and the SysEx
handshake advertises it through flag bit 0 exactly when it does.

See [General-Midi-Boop protocol](GMB_PROTOCOL.md).

### `GET /api/status` — recognition keys

`/api/status` additionally reports the firmware version and the recognition
state, for troubleshooting automatic discovery:

```json
{
  "firmware": "1.1.0",
  "gmb": {
    "instance_id": "0x7AEFE22E",
    "revision": 3,
    "descriptor_size": 744,
    "configured": true,
    "flags": 3
  }
}
```

`flags` is the handshake flag byte: bit 0 = HTTP descriptor reachable, bit 1 =
change notifications supported. `configured` is `false` when the active
configuration is not sufficient to play, which is the case General-Midi-Boop
reads as "hand this instrument back to manual entry".

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

`GET /api/diagnostics` and `POST /api/diagnostics/run` are passive and never move
hardware. An active test is requested explicitly through the WebSocket test
commands, is bounded by `TEST_SESSION_MAX_MS`, is cancellable, and belongs to the
client that started it. Variable strings in JSON responses go through a JSON
serializer so SSIDs, filenames, device names and error text are escaped.

Configuration reset and factory reset report `applied:false` and
`restart_required:true` after safing the hardware; they do not claim the defaults
are already active until the controlled reboot has happened.

### Transactional configuration write

`POST /api/config` never mutates the running configuration while it parses.
It copies the active configuration into a candidate, applies the JSON to that
candidate, normalises and fully validates it, decides whether the change needs a
hardware re-init, persists the candidate, and only then replaces the active
configuration in a single assignment performed by the main loop. Consequences
visible from the API:

| Outcome | Response | Device state |
|---|---|---|
| Invalid candidate | `400 {"ok":false,"error":"<reason>"}` | unchanged (configuration and controllers) |
| Save failed | `500 {"ok":false,"saved":false,"error":"storage_failed"}` | unchanged; keeps running on the previously persisted configuration |
| Applied | `200 {"ok":true,"saved":true,"applied":true,"restart_required":false}` | new configuration active |
| Needs a hardware re-init | `200 {"ok":true,"saved":true,"applied":false,"restart_required":true,"restarting":true}` | new configuration saved, **old one still active**, actuators safed, controlled reboot scheduled |
| Loop busy / not answering | `503 {"ok":false,"error":"busy"}` | unchanged |

A configuration change bumps the General-Midi-Boop revision only in the "applied"
row — validated, saved *and* active. A restart-required change is announced after
the reboot, when it really takes effect.

### MIDI upload

`POST /api/midi` holds an exclusive server-side upload slot. A second concurrent
client gets `409 {"error":"upload_busy"}` and never touches the transfer in
flight; an abandoned transfer releases the slot after `UPLOAD_LOCK_TIMEOUT_MS`.
Each transfer writes to its own temporary file outside `/midi`, so it is neither
listed nor counted against the quota, and the destination file is replaced only
**after** the upload has been fully validated (name and extension, size, storage
quota, real MIDI parse). A rejected upload therefore never destroys the file it
was meant to replace. Error codes: `upload_busy`, `unauthorized`, `invalid_name`,
`too_large`, `write_failed`, `storage_full`, `invalid_midi`, `storage_error`.

## Access model

### Authentication

Every route that changes something — configuration, reset, restart, filesystem
recovery, MIDI files, Wi-Fi — and every WebSocket command requires a session
token. Purely informative routes (`/`, `/api/status`, `/api/config` read,
`/api/diagnostics`, `/api/wifi/status`, `/gmb/descriptor.json`,
`/api/auth/status`) stay open: they expose no secret (the Wi-Fi password is
never serialized) and are what a controller or a monitoring page needs.

```
POST /api/auth/login   {"password":"<admin password>"}
      -> 200 {"ok":true,"token":"<32 hex chars>","ttl_ms":3600000}
      -> 401 {"ok":false,"error":"invalid_credentials"}
```

The token is then sent on every protected call, as the `X-Auth-Token` header or
as a `?token=` query parameter. Sessions live in RAM only (at most four, sliding
one-hour expiry, constant-time comparison) and are dropped on reboot.

The WebSocket is authenticated in-band: the server answers a new connection with
`{"t":"auth_required"}` and refuses every command with
`{"t":"error","msg":"unauthorized"}` until the client sends
`{"t":"auth","token":"<token>"}`. The embedded UI does this automatically and
shows a sign-in overlay when a 401 comes back.

### Initial credentials

Both secrets are generated **randomly at first boot** from the ESP32 hardware RNG
and stored in NVS:

- the hotspot WPA2 key (14 characters) — see [Wi-Fi modes](WIFI_MODES.md);
- the web admin password (14 characters).

Both are printed on the serial console at boot. The admin password can be changed
from the UI (`POST /api/auth/password`, minimum 8 characters); a user-chosen
password is stored only as a salted, iterated SHA-256 digest and is therefore no
longer displayable. Changing it revokes every open session.

**Recovery:** holding the BOOT button while the board powers up (5 s) regenerates
both secrets and prints them on the serial console. This is a physical-presence
path, so a headless instrument whose password was lost is never bricked.

### What authentication does and does not cover

Authentication bounds *who* may command the instrument. It does not replace
electrical protection: keep actuator power switchable, and do not expose the
device directly to the Internet. There is no TLS — the token travels in clear on
the local link, so treat the network as the trust boundary it is.

### `hardware_not_ready`

A command that would physically move something (finger servo, airflow servo,
angle servo, solenoid/valve, pump, fan, test note, automatic calibration, range
finder) is refused whenever the hardware did not initialise: missing PCA9685 at
0x40 (or 0x41 when the configuration needs it), unsafe boot configuration, or an
unmountable filesystem.

- REST: `503 {"ok":false,"error":"hardware_not_ready"}`
- WebSocket: `{"t":"error","msg":"hardware_not_ready"}`

The refusal is enforced twice: once in the web layer, so the client gets an
explicit answer, and once in `InstrumentManager::applyCommand()`, which is the
single point where any actuator order is applied. After a PCA failure there is
therefore no path — web, MIDI, calibration or physical button — that can energise
an actuator.

Diagnostics, configuration read and write, reset, filesystem recovery and network
recovery remain available in that state, on purpose: they are what you need to
get out of it.
