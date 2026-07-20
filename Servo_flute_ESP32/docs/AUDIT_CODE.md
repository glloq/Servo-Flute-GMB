# Firmware and Web Configuration Audit

Date: 2026-07-20
Scope: `RuntimeConfig`, `ConfigStorage`, `InstrumentManager`, `FingerController`, `AirflowController`, `PressureController`, `FanController`, `NoteSequencer`, `AutoCalibrator`, `WebConfigurator`, embedded `web_content.h`, BLE-MIDI, rtpMIDI/Wi-Fi MIDI, serial MIDI, MIDI file playback, PCA9685 routing, GPIO use, and servo power management.

## Summary of findings and fixes

| Area | Severity | Finding | Correction | Required tests |
| --- | --- | --- | --- | --- |
| Boot safety | Critical | `initSafeState()` initialized PCA9685 outputs and moved servos using compiled defaults before `/config.json` was loaded. | Boot safe state now disables PCA OE immediately, sets only known fixed critical GPIOs safe, starts LittleFS, loads and validates configuration before hardware managers can position actuators. | Power-on with custom `/config.json`; verify no default servo movement before config load. |
| Runtime validation | Critical | Limits and conflicts were partly enforced in HTML or ad hoc request parsing only. | Added centralized `validateAndNormalizeConfig()` with explicit limits, PCA conflict checks, GPIO reserved/input-only checks, min/max relationship checks, MIDI/note/fingering checks, and restart-required detection. | Native validation tests plus invalid REST payloads returning HTTP 400. |
| Configuration migration | High | `anglePcaChannel` and `angleServoPcaChannel` duplicated the same concept; legacy JSON keys existed. | `angleServoPcaChannel` is the canonical field. `ang_pca` and `angle_ch` are migrated on load/POST; saves emit `angle_ch` only. | Load old configs containing `ang_pca`; save and reload. |
| Valve servo direction | Medium | `valveServoDir` was saved but close/open angles already define travel and direction. | Saves no longer emit `vlv_dir`; POST ignores it for backward compatibility. | Load old config with `vlv_dir`; test close/open angle commands. |
| Sequencing timestamps | High | Live events and scheduled events shared relative timestamp logic that could keep stale references. | Event queue now has explicit live/scheduled enqueue methods and stores absolute execution times; queue reset clears timing reference. | Live note-on/off; MIDI file playback; queue-empty then new sequence. |
| Minimum note duration | High | `minNoteDurationMs` was persisted but not enforced. | `NoteSequencer` defers normal note-off until the minimum duration expires while keeping panic/all-sound-off immediate. | Short note-off test and panic bypass test. |
| Pressure calculations | Critical | Hall and ToF span divisions could divide by zero; pump cascade threshold 100 could divide by zero; direct pump demand was converted twice. | Validation rejects equal sensor thresholds/ranges, clamps cascade to 0..99, runtime guards zero denominators, and direct mode feeds logical demand to a single physical PWM conversion path. | Pump direct/reservoir tests for thresholds 0 and 99, equal sensor bounds, absent sensor. |
| Reservoir sensor absence | Critical | Reservoir mode without sensor silently behaved like direct pump mode. | Reservoir mode now stops pumps, clears bang-bang/PID state, and does not run direct mode when the sensor is absent. | Disconnect sensor during mode 5 and verify pumps off/fault diagnostics. |
| Autonomous fan/pump behavior | High | Fan and pump operation depended on WebSocket manual targets for some modes. | Added persistent autonomous defaults for fan note/idle behavior, direct pump note/idle behavior, and reservoir autostart target. MIDI note-on/off now drives fan/pump targets without a web page. | BLE-MIDI, DIN MIDI, rtpMIDI, and MIDI file playback without web client. |
| REST/WebSocket parsing | High | WebSocket command parsing used `String::indexOf()` and accepted malformed/out-of-range data. | WebSocket messages are parsed with ArduinoJson, capped at 512 bytes, range-checked, and panic on disconnect returns hardware to safe state. Config POST is size-limited and returns structured JSON including `restart_required`. | Invalid JSON, oversize WS frame, bad finger index, out-of-range note/CC. |
| Diagnostics | Medium | No structured hardware/config diagnostics API existed. | Added `GET /api/diagnostics` and `POST /api/diagnostics/run` with config, LittleFS, PCA requirement, microphone, restart, and heap checks. | Call endpoints before/after invalid config; hardware PCA checks require device. |

## Fields requiring restart

The validator marks `restart_required` when persisted changes affect pin modes, I2C/PCA routing, controller initialization, or MIDI serial setup: number of fingers, finger PCA channels, airflow/valve/angle PCA channels, air mode, valve type, solenoid GPIO, fan GPIO, pump count or pump GPIOs, motor type, sensor type, Hall/endstop GPIOs, and serial MIDI enable/RX pin.

## Dynamic fields

CC defaults, note airflow percentages, note angle percentages, finger patterns, velocities, expression/vibrato values, fan/pump temporary targets, and PID target percentages are dynamic when they do not require a new `pinMode()`, PCA begin, sensor begin, or MIDI serial begin.

## Remaining hardware tests

No physical ESP32, PCA9685 board, sensor, pump, fan, valve, microphone, or MIDI hardware was available in this environment. All rows in `HARDWARE_TEST_MATRIX.md` are therefore marked `NOT TESTED — requires hardware` until executed on a real bench.
