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
| `ConfigStorage` | Runtime configuration, fail-safe LittleFS mount and persistence |
| `ConfigCommit` | Transactional configuration commit (validate → save → activate) |
| `CommandQueue` | Cross-task actuator command ring + undroppable panic flag |
| `TofSensor` | VL53L0X / VL6180X driver: identification, full init, non-blocking ranging |
| `WebAuth` | Web session tokens (creation, sliding expiry, constant-time check) |
| `DeviceSecrets` | NVS-backed hotspot key and web admin password |
| `VibratoMath` | Signed sine LUT for the CC1 vibrato oscillator |
| `gmb/` | General-Midi-Boop recognition: capability snapshot, JSON descriptor, SysEx codec, transport bridge, revision |

## Data flow

1. MIDI input handlers receive events.
2. `InstrumentManager` filters and queues note/control events.
3. `NoteSequencer` schedules finger movement and airflow timing.
4. `FingerController` drives PCA9685 finger servos.
5. `AirflowController`, `PressureController`, and `FanController` drive the selected air system.
6. `WebConfigurator` publishes status and accepts configuration changes.

## Task ownership

The firmware runs on more than one FreeRTOS task: `loop()`, the AsyncTCP task
that serves HTTP and WebSocket callbacks, and the NimBLE host task that reports
BLE connection events. Only **one** of them owns the hardware.

```text
AsyncTCP (HTTP / WebSocket)  ─┐
NimBLE host (connect events) ─┼─▶ CommandQueue ─▶ loop() ─▶ actuators, I2C, GPIO
MIDI callbacks (loop task)   ─┘                     │
                                                    └─▶ EventQueue ─▶ NoteSequencer
web operation (config, LittleFS,
calibration, MIDI player)     ──▶ single WebOp slot ──▶ loop() ──▶ result ──▶ HTTP response
```

- **`EventQueue`** carries timed note events. Reading an event and removing it is
  a single locked operation (`tryPopDueEvent`), so a concurrent `clear()`, a
  forced Note Off on a full queue, or a panic can no longer make the sequencer
  execute, reorder or lose an event. No method hands out a pointer into the
  queue; `clear()` bumps an epoch that aborts a burst already in progress.
- **`CommandQueue`** carries actuator orders. A panic is a dedicated flag rather
  than a queue slot, so it can never be lost to a full queue, and setting it
  discards every command issued before it.
- **`InstrumentManager::applyCommand()`** is the single application point, and it
  refuses every physically-acting command while the hardware is not ready.
- **Requests that are not queue slots** — the deferred servo power-on and the
  CC121 Reset All Controllers — are taken and cleared in a *single* critical
  section, like the panic flag. They used to be plain `volatile bool` read and
  then cleared as two steps; a request posted between the two was silently lost.
  `volatile` keeps the compiler from caching a variable and gives neither
  atomicity nor a barrier, so it is never a synchronisation primitive here.
- **The work applied per `update()` is bounded.** Draining the whole ring in one
  pass could chain dozens of I2C transactions and starve the rest of `loop()`
  under a WebSocket burst. Commands over the bound are *deferred*, never
  dropped, and the ring is FIFO so nothing starves. The panic is outside the
  bound. Pending Note Offs still come after the ring, but they yield at most two
  consecutive passes: past that the release goes through, because a permanently
  saturated ring would otherwise hold a note with the valve and air open.
- **A queue whose allocation failed is inert, not fatal**: pushes and pops refuse,
  `count()` is 0, nothing is dereferenced, and `queues_ok` in the diagnostics
  reports it. The panic flag and the Note Off bitmap keep working, because
  neither lives in the allocated array.
- **Web operations** that touch the active configuration, LittleFS, the MIDI
  player or the calibrator are handed to `loop()` in one of two ways:
  - an **HTTP** handler uses the one-slot blocking hand-off: it fills the slot,
    waits (bounded by `WEBOP_TIMEOUT_MS`) for `loop()` to execute it, then
    answers with the real result, so `POST /api/config` keeps exact semantics;
  - a **WebSocket** command uses a non-blocking queue and its result, if any, is
    broadcast by `loop()`. A WS callback may hold the AsyncWebSocket lock that
    `loop()` takes to broadcast, so it must never wait for `loop()`.

  `loop()` never waits on AsyncTCP, so neither path can deadlock.

The rule this enforces: **no AsyncTCP or NimBLE callback ever performs an I2C/PCA
transaction, drives a GPIO, or mutates `cfg`.**

## Configuration commit

`POST /api/config` is transactional (`ConfigCommit.cpp`):

```text
candidate = copy(active)
apply JSON onto candidate        (no visible effect)
normalise + validate candidate   (no visible effect)
decide restartRequired           (pure predicate)
save candidate to LittleFS       (flash first)
active = candidate               (one assignment, on the loop task)
apply to controllers
```

A validation failure or a storage failure leaves the active configuration *and*
the controllers untouched. A change that needs a hardware re-init is saved but
not activated: the device keeps running on the configuration that matches the
initialised hardware until the controlled reboot.

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

LittleFS is mounted **without automatic formatting**. A mount failure used to
reformat the partition, which silently destroyed `/config.json` and the stored
MIDI files and restarted the instrument on defaults that may not match the actual
wiring. The firmware now enters a recovery state instead: PCA output-enable stays
high, no actuator command is accepted, the diagnostics report the failure, and a
format happens only on an explicit, confirmed `POST /api/fs/format`.

The web admin password and the hotspot WPA2 key live in NVS
(`Preferences`, namespace `flutesec`), not in `/config.json`: a factory reset of
the configuration must not silently open the device up.

The General-Midi-Boop capability revision is deliberately kept out of
`/config.json` and stored in NVS (`Preferences`, namespace `gmb`): a
configuration save never rewrites the counter, and a counter bump never rewrites
the configuration.

## 2026 runtime safety and validation update

Configuration is validated by the firmware, not only by HTML controls. PCA channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only output pins, invalid min/max relationships, invalid MIDI channels, invalid fingering values, and unsafe pump/sensor bounds are detected before saving.

Parameters that change `pinMode()`, I2C/PCA routing, controller `begin()` behavior, sensor setup, or serial MIDI setup require a restart. Dynamic musical values such as CC defaults, note airflow/angle percentages, fingering patterns, and temporary fan/pump test targets can be applied without hardware reinitialization.

Manual hardware tests must always be time-limited and followed by a safe state. In software-only validation, hardware procedures are documented but marked `NOT TESTED — requires hardware` in `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.
