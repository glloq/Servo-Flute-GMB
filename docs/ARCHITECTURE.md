# Architecture

The firmware is organized into small modules around `InstrumentManager`, which coordinates MIDI events, fingers, airflow, sequencing, storage, and the web UI.

## Main modules

| Module | Responsibility |
|--------|----------------|
| `InstrumentManager` | Central orchestration, note on/off, control changes, safe state |
| `FingerController` | Finger servo positions and fingering patterns |
| `AirflowController` | Solenoid, servo-flow, valve, expression, and angle servo logic |
| `PressureController` | Pumps, reservoir sensors, PID/bang-bang control |
| `FanController` | Fan startup ramp and idle behavior |
| `NoteSequencer` | Timing between finger positioning, airflow, and note release |
| `MidiFilePlayer` | SMF Type 0/1 MIDI parsing and playback |
| `BleMidiHandler` | BLE-MIDI input |
| `WifiMidiHandler` | WiFi, rtpMIDI, AP/STA modes, mDNS |
| `SerialMidiHandler` | UART MIDI input |
| `WebConfigurator` | REST API, WebSocket API, and embedded SPA |
| `ConfigStorage` | Runtime configuration and LittleFS persistence |

## Data flow

1. MIDI input handlers receive events.
2. `InstrumentManager` filters and queues note/control events.
3. `NoteSequencer` schedules finger movement and airflow timing.
4. `FingerController` drives PCA9685 finger servos.
5. `AirflowController`, `PressureController`, and `FanController` drive the selected air system.
6. `WebConfigurator` publishes status and accepts configuration changes.

## Persistence

Defaults live in `settings.h`. Runtime values are stored in `RuntimeConfig` and saved to `/config.json` on LittleFS.

## 2026 runtime safety and validation update

Configuration is validated by the firmware, not only by HTML controls. PCA channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only output pins, invalid min/max relationships, invalid MIDI channels, invalid fingering values, and unsafe pump/sensor bounds are detected before saving.

Parameters that change `pinMode()`, I2C/PCA routing, controller `begin()` behavior, sensor setup, or serial MIDI setup require a restart. Dynamic musical values such as CC defaults, note airflow/angle percentages, fingering patterns, and temporary fan/pump test targets can be applied without hardware reinitialization.

Manual hardware tests must always be time-limited and followed by a safe state. In software-only validation, hardware procedures are documented but marked `NOT TESTED — requires hardware` in `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.
