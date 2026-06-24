# Code Audit

## Overall verdict

The firmware is functional and modular. The architecture cleanly separates MIDI input, sequencing, finger servos, airflow, storage, and the web interface. Remaining concerns are mostly robustness improvements and edge cases.

## BLE-MIDI

- NimBLE backend is initialized correctly.
- Note On with velocity 0 is handled as Note Off.
- Static callbacks are protected against null pointers.
- Advertising restart behavior depends on the BLE-MIDI library; explicit restart may be worth validating on hardware.

## WiFi / rtpMIDI

- AP and STA modes are implemented with fallback behavior.
- rtpMIDI uses AppleMIDI on port 5004.
- mDNS advertises HTTP and AppleMIDI services.
- WiFi credentials are persisted in configuration.
- JSON escaping for scanned SSIDs should be reviewed if arbitrary SSIDs are expected.

## MIDI file player

- SMF Type 0 and Type 1 parsing is implemented.
- Tempo changes and tick-to-ms conversion use 64-bit arithmetic.
- Playback is non-blocking.
- Event allocation is fixed-size to avoid fragmentation.
- Unknown or malformed chunks should continue to be guarded against infinite loops.

## Finger and note sequencing

- Finger lookup and servo angle bounds checks are present.
- Note sequencing positions fingers before opening air.
- Overlapping notes are handled by stopping the current note before starting the next.

## Airflow and pressure

- Six air modes are supported.
- CC7, CC11, CC2, CC73, and CC74 are integrated.
- Pump/reservoir control supports cascade, stagger, bang-bang, and PID behavior.
- CC73 intentionally changes live expression behavior and is not persisted unless saved through the UI.

## Web configurator

- REST and WebSocket APIs cover configuration, MIDI upload, playback, WiFi, and live testing.
- MIDI upload validates size and storage limits.
- Path traversal is mitigated by sanitizing stored MIDI file names.

## Storage

- LittleFS configuration is initialized from defaults and persisted to JSON.
- Bounds checks are applied to variable-size arrays.
- WiFi password is stored in plain text, which is acceptable for this embedded project but should be documented.

## Recommendations

1. Build JSON responses with ArduinoJson where user-provided strings are included.
2. Keep EOF guards in MIDI parsing paths.
3. Consider factoring shared angle-to-PWM math.
4. Hardware-test BLE advertising restart behavior.
5. Document volatile MIDI CC behavior for expression controls.
