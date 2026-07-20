# Servo Flute MIDI Boop

**Modular MIDI-driven robotic instrument based on ESP32.**

Turns a recorder, tin whistle, ocarina, Native American flute, or any other simple blown flute into an autonomous robotic instrument. The firmware adapts to the instrument: number of fingers, air-management type, and custom fingerings are all runtime-configurable from the web interface without recompiling.

> **Scope:** Simple blown instruments (recorder, tin whistle, NAF, ocarina, etc.). Reed or buzzing instruments (clarinet, trumpet, etc.) are out of scope.

## Features

- **Up to 31 servos** across 2 PCA9685 boards, each independently configurable
- **3 simultaneous MIDI inputs** — Bluetooth (BLE-MIDI), WiFi (rtpMIDI), and Serial (UART)
- **6 air-management modes** — solenoid, servo valve, fan, pump, and more
- **Web interface** — virtual keyboard, MIDI player, calibration, air management, and settings page
- **Optional auto-calibration** with an INMP441 microphone (I2S)
- **MIDI file playback** (.mid SMF Type 0/1) without an external source
- **Persistent configuration** in JSON on LittleFS, editable at runtime

## Hardware

| Component | Role |
|-----------|------|
| ESP32-WROOM-32E | Main microcontroller |
| PCA9685 (x1 or x2) | 16-channel PWM controller for servos |
| SG90 servos | Fingers + airflow |
| Air system | Solenoid, servo valve, fan, or pump |
| 5V power supply | Sized according to the number of servos |
| INMP441 (optional) | I2S microphone for auto-calibration |

Full pinout: [Configuration](docs/CONFIGURATION.md)

## Quick start

1. Install the Arduino dependencies (see `Servo_flute_ESP32/settings.h`).
2. Flash the firmware to the ESP32.
3. On first boot, the `ServoFlute-Setup` WiFi hotspot starts.
4. Connect to the hotspot — the configuration page opens automatically through the captive portal.
5. Configure the number of fingers, fingerings, and air mode.
6. Calibrate the servos in the Calibration tab.
7. Connect a MIDI source (BLE, WiFi, or Serial) and play.

## Physical controls

### BT/WiFi switch (GPIO4)

| Position | Mode |
|----------|------|
| LOW | Bluetooth (BLE-MIDI) |
| HIGH | WiFi (rtpMIDI + web interface) |

### BOOT button (GPIO0)

| Action | Effect |
|--------|--------|
| Short press | BLE: restart advertising / WiFi: show the IP address |
| Double press (< 500ms) | Open all fingers, regardless of the current mode |
| Long press (3s) | WiFi: force hotspot (AP) mode |

## MIDI reception

The three channels run in parallel and converge on the same `InstrumentManager`:

| Channel | Protocol | Usage |
|---------|----------|-------|
| Bluetooth | BLE-MIDI (NimBLE) | iOS, macOS, Windows, Android |
| WiFi | rtpMIDI (AppleMIDI) | DAW over the local network |
| Serial | MIDI DIN (UART 31250 baud) | Hardware MIDI modules |

Details: [WIFI_MODES.md](docs/WIFI_MODES.md)

## Web interface

Available in WiFi mode — a single-page web application with 4 tabs and a settings page:

| Section | Function |
|---------|----------|
| **Keyboard** | Interactive notes (touch, mouse, AZERTY keyboard) |
| **MIDI** | Upload and play `.mid` files |
| **Air** | Air-system control and real-time monitoring |
| **Calibration** | Servo tests, finger setup wizard, airflow sweep, and auto-calibration |
| **Settings** (gear) | Parameters, fingering table, WiFi, and persistent save |

REST API and WebSocket protocol: [API_WEB.md](docs/API_WEB.md)

## Documentation

| Document | Description |
|----------|-------------|
| [Architecture](docs/ARCHITECTURE.md) | Modules, data flow, and state machine |
| [Air management](docs/AIR_MANAGEMENT.md) | The 6 air modes, sensors, pumps, and fans |
| [Web API](docs/API_WEB.md) | REST endpoints and WebSocket protocol |
| [Auto-calibration](docs/AUTO_CALIBRATION.md) | Automatic calibration with the INMP441 microphone |
| [Calibration](docs/CALIBRATION.md) | Step-by-step manual calibration guide |
| [Configuration](docs/CONFIGURATION.md) | All configurable parameters |
| [PCA9685 expansion](docs/PCA9685_EXPANSION.md) | Multi-board PCA9685 support (up to 31 servos) |
| [WiFi modes](docs/WIFI_MODES.md) | BLE-MIDI, WiFi STA, hotspot AP |
| [Serial MIDI](docs/MIDI_SERIAL.md) | MIDI DIN wiring (optocoupler, GPIO) |
| [Servo angle](docs/SERVO_ANGLE.md) | Servo mounting and calibration for transverse flutes |

## Project structure

```
Servo-Flute-Midi-Boop/
  README.md                    <- This file
  docs/                        <- Technical documentation
  Servo_flute_ESP32/           <- Arduino firmware (ESP32)
    Servo_flute_ESP32.ino      - Main entry point (setup/loop)
    settings.h                 - Hardware defines and default values
    ...                        - Modules (see ARCHITECTURE.md)
```

## License

MIT

## 2026 safety/configuration audit notes

This firmware now validates `RuntimeConfig` centrally after LittleFS load, before save, and after web configuration updates. Invalid impossible configurations are rejected by the REST API with HTTP 400; recoverable bounds are normalized. Hardware-affecting changes are reported with `restart_required` so the web UI can request a safe reboot instead of silently applying pin/PCA changes at runtime.

Supported instruments remain simple wind instruments without reed/lip embouchure control: transverse flute, recorder, tin whistle, Native American flute, ocarina, shakuhachi, ney, kaval, and similar flute-family instruments.

See `Servo_flute_ESP32/docs/AUDIT_CODE.md` and `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md` for the current audit and bench-test plan.

## Finalization validation status

Software CI covers ESP32 firmware build, PlatformIO native behavior tests, pytest audits, JSON escaping regressions, MIDI 7-bit WebSocket bounds, request-size limits, diagnostics vocabulary, and supported air-management modes. Physical validation remains explicitly marked **NOT TESTED — requires hardware** until executed on the corresponding PCA9685, microphone, ToF, Hall, endstop, pump, fan, solenoid, and servo hardware.
