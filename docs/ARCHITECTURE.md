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
| `gmb/` | General-Midi-Boop recognition: capability snapshot, JSON descriptor, SysEx codec, transport bridge, revision |

## Data flow

1. MIDI input handlers receive events.
2. `InstrumentManager` filters and queues note/control events.
3. `NoteSequencer` schedules finger movement and airflow timing.
4. `FingerController` drives PCA9685 finger servos.
5. `AirflowController`, `PressureController`, and `FanController` drive the selected air system.
6. `WebConfigurator` publishes status and accepts configuration changes.

## General-Midi-Boop recognition

`gmb/` is a self-contained group of modules that turns the active configuration
into what General-Midi-Boop needs to recognize the instrument and import its
capabilities. It reads `RuntimeConfig`; it never holds a configuration of its
own.

```text
RuntimeConfig (active, validated)
        ↓  gmb::buildSnapshot()
CapabilitySnapshot (immutable)
        ↓  GmbDescriptor::toJson()
cached JSON descriptor
        ↓                      ↓
GmbSysExService           GET /gmb/descriptor.json
        ↓
GmbMidiBridge  ←→  BleMidiHandler / WifiMidiHandler  (IGmbMidiPort)
```

The protocol lives in one place. A MIDI transport implements `IGmbMidiPort` and
is registered as a port; a transport with no return path (the receive-only DIN
input) is simply not registered and keeps working for Note and CC traffic.
SysEx callbacks only stage a request; the reply is built and sent from the main
loop, so discovery never runs inside the real-time note path.

`GmbRuntime` is the only ESP32-specific piece (eFuse MAC, NVS) and the single
entry point a configuration change calls once it is validated, committed and
active. Details in [General-Midi-Boop protocol](GMB_PROTOCOL.md).

## Persistence

Defaults live in `settings.h`. Runtime values are stored in `RuntimeConfig` and saved to `/config.json` on LittleFS.

The General-Midi-Boop capability revision is deliberately kept out of
`/config.json` and stored in NVS (`Preferences`, namespace `gmb`): a
configuration save never rewrites the counter, and a counter bump never rewrites
the configuration.

## 2026 runtime safety and validation update

Configuration is validated by the firmware, not only by HTML controls. PCA channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only output pins, invalid min/max relationships, invalid MIDI channels, invalid fingering values, and unsafe pump/sensor bounds are detected before saving.

Parameters that change `pinMode()`, I2C/PCA routing, controller `begin()` behavior, sensor setup, or serial MIDI setup require a restart. Dynamic musical values such as CC defaults, note airflow/angle percentages, fingering patterns, and temporary fan/pump test targets can be applied without hardware reinitialization.

Manual hardware tests must always be time-limited and followed by a safe state. In software-only validation, hardware procedures are documented but marked `NOT TESTED — requires hardware` in `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.
