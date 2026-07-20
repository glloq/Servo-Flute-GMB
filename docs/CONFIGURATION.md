# Configuration

Configuration is stored in `RuntimeConfig` and persisted as JSON on LittleFS. Defaults are defined in `Servo_flute_ESP32/settings.h`.

## Instrument

- number of fingers;
- number of notes;
- PCA channel per finger;
- closed angle and direction per finger;
- half-hole percentage;
- mouthpiece type;
- note table and fingering patterns.

## Airflow

- air mode;
- flow servo PCA channel;
- per-note airflow min/max;
- optional angle servo for transverse flutes;
- solenoid or servo-valve settings;
- pump, fan, reservoir, and sensor settings.

## MIDI

- MIDI channel (`0` means omni);
- default CC values;
- serial MIDI enable flag and RX pin;
- MIDI file storage limit.

## WiFi

- station SSID and password;
- AP setup mode;
- mDNS hostname.

## Web UI options

- device name;
- instrument color;
- hide/show Air and Calibration tabs;
- keyboard display mode.

## Reset behavior

A normal reset restores defaults and saves them. A factory reset removes the configuration file so the first-use wizard opens again.

## 2026 runtime safety and validation update

Configuration is validated by the firmware, not only by HTML controls. PCA channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only output pins, invalid min/max relationships, invalid MIDI channels, invalid fingering values, and unsafe pump/sensor bounds are detected before saving.

Parameters that change `pinMode()`, I2C/PCA routing, controller `begin()` behavior, sensor setup, or serial MIDI setup require a restart. Dynamic musical values such as CC defaults, note airflow/angle percentages, fingering patterns, and temporary fan/pump test targets can be applied without hardware reinitialization.

Manual hardware tests must always be time-limited and followed by a safe state. In software-only validation, hardware procedures are documented but marked `NOT TESTED — requires hardware` in `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.

## Real-time monophonic policy

The sequencer is intentionally monophonic. Due events are drained from the head of the MIDI queue on every update, regardless of the current note state, so an old event can never block access to later events. The priority order is:

1. panic / all-sound-off: immediate queue clear, valve close, airflow rest, fingers safe;
2. due `NOTE_ON`: replaces the current note immediately, even if `minNoteDurationMs` has not elapsed;
3. `minNoteDurationMs`: delays only a matching normal `NOTE_OFF` for the active note;
4. normal `NOTE_OFF`: stops only the active note and stale note-offs for replaced notes are ignored.

On replacement, the current valve/airflow is transitioned through the same inter-note policy as a normal stop. If the next note is within `valve_interval`, the valve may stay open while the new fingering is positioned; otherwise the valve is closed and airflow returns to rest before the new note starts.

`valve_interval` is the canonical inter-note setting. Legacy JSON using `sol_inter` is migrated on load/API update, but saves and REST responses expose only `valve_interval`.
