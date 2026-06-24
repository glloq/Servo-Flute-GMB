# Complete Firmware Audit

## Scope

This audit covers the ESP32 firmware, MIDI inputs, embedded web interface, servo control, airflow management, storage, and calibration helpers.

## Summary

The project is in a functional state. The codebase is modular, hardware responsibilities are separated, and the web interface exposes the main configuration and diagnostic tools needed to operate the instrument.

## Strengths

- Clear split between MIDI handlers, sequencing, finger control, airflow, storage, and web configuration.
- Runtime configuration supports many instrument layouts.
- Multiple MIDI inputs can converge into the same instrument manager.
- LittleFS persistence makes the project configurable without recompilation.
- The web UI covers setup, calibration, MIDI playback, air-system control, and diagnostics.
- Air modes cover solenoid, servo valve, fan, pump, and reservoir designs.

## Main risks

| Area | Risk | Recommendation |
|------|------|----------------|
| JSON generation | User-provided strings can require escaping | Prefer ArduinoJson for generated responses |
| MIDI parsing | Malformed files can create edge cases | Keep EOF and chunk guards in place |
| Heap usage | MIDI event buffer is fixed-size | Monitor heap on large configurations |
| WiFi credentials | Stored in clear text | Document the tradeoff |
| BLE advertising | Restart behavior depends on library internals | Validate on target devices |
| Mechanical setup | Bad calibration causes poor notes | Improve setup instructions and diagnostics |

## Module review

### Wireless

BLE-MIDI, WiFi AP/STA, rtpMIDI, and mDNS are implemented. AP fallback makes first setup reliable.

### MIDI playback

SMF parsing and non-blocking playback are implemented. Fixed event allocation avoids fragmentation.

### Finger control

Finger servos are configurable per channel, closed angle, direction, and half-hole state.

### Airflow

The airflow stack supports expressive control and multiple hardware topologies. Pressure control includes PID and bang-bang behavior.

### Web interface

The REST and WebSocket APIs support configuration, live control, file upload, status, and calibration. The UI is the main user-facing control surface.

### Storage

Runtime configuration is loaded from defaults, overridden from LittleFS, and saved through the web UI.

## Recommended next steps

1. Run a full firmware build in Arduino CLI or PlatformIO.
2. Hardware-test each air mode.
3. Validate BLE, WiFi AP, WiFi STA, and rtpMIDI on real clients.
4. Test MIDI upload with malformed and oversized files.
5. Test factory reset and first-use wizard flows.
6. Continue improving documentation for mechanical assembly.
