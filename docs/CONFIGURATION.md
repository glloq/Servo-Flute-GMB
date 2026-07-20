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
