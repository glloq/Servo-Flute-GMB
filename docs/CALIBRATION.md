# Calibration Guide

Calibration maps the physical instrument to the firmware configuration: finger count, servo channels, closed angles, fingerings, and airflow ranges.

## Step 1 — Fingers

Configure the number of fingers, PCA9685 channels, closed angles, opening direction, global opening angle, and half-hole percentage.

Use the test buttons to verify that each servo closes and opens the correct hole. Invert direction if a servo moves the wrong way.

## Step 2 — Fingerings

Select a preset or edit MIDI notes manually. Each note stores a MIDI pitch and a fingering pattern:

- closed;
- open;
- half-open.

## Step 3 — Breath

Set minimum and maximum airflow for each note. Use lower values for quiet notes and higher values for strong notes.

If an INMP441 microphone is installed, auto-calibration can sweep airflow and detect playable ranges.

## Step 4 — Expression

Configure attack behavior, vibrato, CC2 breath-controller response, and MIDI expression defaults.

## Saving

Each calibration step must be saved from the web interface. The configuration is persisted on LittleFS as JSON.

## 2026 runtime safety and validation update

Configuration is validated by the firmware, not only by HTML controls. PCA channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only output pins, invalid min/max relationships, invalid MIDI channels, invalid fingering values, and unsafe pump/sensor bounds are detected before saving.

Parameters that change `pinMode()`, I2C/PCA routing, controller `begin()` behavior, sensor setup, or serial MIDI setup require a restart. Dynamic musical values such as CC defaults, note airflow/angle percentages, fingering patterns, and temporary fan/pump test targets can be applied without hardware reinitialization.

Manual hardware tests must always be time-limited and followed by a safe state. In software-only validation, hardware procedures are documented but marked `NOT TESTED — requires hardware` in `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.
