# Auto-Calibration

Auto-calibration uses an optional INMP441 I2S microphone to detect whether the instrument produces sound while the firmware sweeps airflow values.

## Requirements

- INMP441 microphone wired to the configured I2S pins.
- Microphone support enabled in `settings.h`.
- The instrument mechanically calibrated enough for notes to sound.

## Pitch and level detection

The audio analyzer estimates RMS level and pitch. The auto-calibrator uses those measurements to decide whether a note is sounding and to record useful airflow ranges.

## Airflow sweep

For each configured note, the firmware:

1. positions the fingers;
2. opens the air path;
3. sweeps airflow from low to high;
4. records the first and last playable airflow values;
5. stores the resulting min/max percentages.

## Range finder

A separate range-finder mode can sweep the servo from 0-180° and detect the usable servo travel range.

## Limitations

Auto-calibration is a helper, not a replacement for mechanical setup. Background noise, microphone placement, leaks, and poor servo alignment can affect results.

## 2026 runtime safety and validation update

Configuration is validated by the firmware, not only by HTML controls. PCA channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only output pins, invalid min/max relationships, invalid MIDI channels, invalid fingering values, and unsafe pump/sensor bounds are detected before saving.

Parameters that change `pinMode()`, I2C/PCA routing, controller `begin()` behavior, sensor setup, or serial MIDI setup require a restart. Dynamic musical values such as CC defaults, note airflow/angle percentages, fingering patterns, and temporary fan/pump test targets can be applied without hardware reinitialization.

Manual hardware tests must always be time-limited and followed by a safe state. In software-only validation, hardware procedures are documented but marked `NOT TESTED — requires hardware` in `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.

## Finalization validation status

Software CI covers ESP32 firmware build, PlatformIO native behavior tests, pytest audits, JSON escaping regressions, MIDI 7-bit WebSocket bounds, request-size limits, diagnostics vocabulary, and supported air-management modes. Physical validation remains explicitly marked **NOT TESTED — requires hardware** until executed on the corresponding PCA9685, microphone, ToF, Hall, endstop, pump, fan, solenoid, and servo hardware.

### Scope by hardware

Microphone auto-calibration is software-supported for airflow range finding and per-note airflow min/max. Other hardware calibration is guided and manual unless a future hardware test report proves the active sequence safe on real hardware.
