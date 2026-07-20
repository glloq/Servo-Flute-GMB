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

## 2026 runtime safety and validation update

Configuration is validated by the firmware, not only by HTML controls. PCA channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only output pins, invalid min/max relationships, invalid MIDI channels, invalid fingering values, and unsafe pump/sensor bounds are detected before saving.

Parameters that change `pinMode()`, I2C/PCA routing, controller `begin()` behavior, sensor setup, or serial MIDI setup require a restart. Dynamic musical values such as CC defaults, note airflow/angle percentages, fingering patterns, and temporary fan/pump test targets can be applied without hardware reinitialization.

Manual hardware tests must always be time-limited and followed by a safe state. In software-only validation, hardware procedures are documented but marked `NOT TESTED — requires hardware` in `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.


## JSON serialization and diagnostics contract

REST and WebSocket payloads that include user-provided strings are serialized with ArduinoJson so escaping is correct for examples such as `deviceName = Flute "A"`, `SSID = atelier\wifi`, and `fichier = étude "test".mid`.

`GET /api/diagnostics` is passive: it reports `ok`, `warning`, `error`, `not_tested`, or `not_applicable` without moving actuators. `POST /api/diagnostics/run` is reserved for an explicit active, timeout-bounded hardware sequence that announces the tested components, supports stop/panic, returns outputs to a safe state, and keeps physical checks marked `NOT TESTED — requires hardware` until executed on real hardware.
