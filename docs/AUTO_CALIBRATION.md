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
