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

## Hardening pass (2026-07)

Fixed two reachable divide-by-zero crashes:

- **`MidiFilePlayer::parseMThd`** — a malformed uploaded `.mid` with a `division`
  (ticks/beat) of `0` reached `ticksToMs()` and triggered an integer
  divide-by-zero panic on the ESP32. `parseMThd` now rejects `division == 0`
  in addition to the existing SMPTE guard, so the upload fails cleanly with
  "Invalid MIDI format".
- **`fastSin` (AirflowController)** — if `vibratoFrequencyHz` was configured to
  `0` (or above ~1000 Hz, which rounds the period to `0`) while modulation was
  active, the `timeMs % period` computed a modulo-by-zero. `fastSin` now guards
  both cases and returns `0` (vibrato disabled) instead of crashing.

### Still open (defense-in-depth, not blocking)

- REST/WebSocket JSON responses are hand-built by string concatenation; embedded
  string fields (`device`, `wifi_ssid`, `instrumentColor`, and scanned SSIDs in
  `getScanResultsJson`) are not escaped. A `"` or `\` in any of them yields
  malformed JSON and breaks the corresponding UI panel. Scanned SSIDs are the
  only externally-controlled source. Prefer ArduinoJson serialization here.
- `POST /api/config` scalar fields (PCA channels, GPIO pins, angles) are stored
  with minimal server-side range validation. The web UI constrains inputs, but
  out-of-range values sent directly are accepted. Add range clamps for
  defense-in-depth.
- `NoteSequencer` keeps the relative-timestamp / `_playbackStartTime`
  pre-scheduling machinery, but `_playbackStartTime` is never reset after the
  first event, so live events always fire immediately (the intended behavior for
  live play). The scheduling code is effectively vestigial and could be
  simplified for clarity.
