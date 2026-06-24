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
