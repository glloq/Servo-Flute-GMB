# Project Status and Validation

This document centralizes the validation state and known limitations of Servo Flute GMB. It prevents temporary audit notes and repeated validation warnings from overloading the main README and the feature documentation.

## Status convention

- **Implemented:** present in the firmware or web interface.
- **Software tested:** covered by automated host tests, build checks, or static audits.
- **NOT TESTED — requires hardware:** the procedure is defined, but it has not been executed on the corresponding physical components.
- **Known limitation:** intentionally documented behavior that still requires future work.

## Current status

| Area | Implementation | Validation |
|---|---|---|
| ESP32 firmware build | Implemented | Automated build available |
| PlatformIO native behavior tests | Implemented | Software tested |
| Runtime configuration and LittleFS persistence | Implemented | Software tested |
| Central configuration validation | Implemented | Software tested |
| Safe boot and PCA output-enable ordering | Implemented | Software tested; physical power-on test required |
| Finger servo control | Implemented | NOT TESTED — requires hardware |
| Six air-management modes | Implemented | Software behavior tested; physical validation required |
| BLE-MIDI | Implemented | NOT TESTED — requires hardware |
| rtpMIDI / AppleMIDI | Implemented | NOT TESTED — requires hardware |
| Serial MIDI DIN | Implemented | NOT TESTED — requires hardware |
| MIDI file playback | Implemented | Software tested; acoustic validation required |
| Embedded web interface | Implemented | Software tested |
| INMP441 audio analysis | Implemented | Software tested; microphone validation required |
| Per-note automatic airflow calibration | Implemented | Software tested; flute validation required |
| Pumps, reservoir, fan, and sensors | Implemented | NOT TESTED — requires hardware |
| REST/WebSocket authentication | Not implemented | Known limitation |

## Safety and reliability work completed

The 2026 firmware audit introduced or reinforced:

- actuator outputs disabled before configuration is loaded and validated;
- inert behavior when required PCA9685 hardware cannot be initialized safely;
- centralized validation of GPIO capability, reserved pins, PCA channel conflicts, MIDI limits, fingering values, and sensor ranges;
- atomic configuration persistence and rollback on failed writes;
- controlled restart for hardware-routing changes;
- firmware-side time limits for manual actuator tests;
- actuator-session ownership for calibration and manual tests;
- panic and disconnect paths returning hardware to a safe state;
- non-blocking ToF sensor reads and stale-sensor pump shutdown;
- pump and fan demand tied to accepted sequencer note transitions;
- bounded calibration timeouts and preservation of previous values when a note fails calibration.

These software protections do not replace electrical protection, a physical emergency stop, appropriate fusing, correct power sizing, or physical verification.

## Known network limitation

The current web API and WebSocket have no authentication. A client that can reach the ESP32 may be able to drive actuators, run tests, modify the configuration, restart the controller, and manage MIDI files.

Required operational mitigations:

- set a non-empty access-point password;
- use a trusted private network;
- do not expose the ESP32 directly to the Internet;
- disconnect actuator power when unattended.

Full details: [API access model](API_WEB.md#access-model-and-known-security-limitation).

## Physical validation

The detailed procedures and expected results are maintained in:

- [`Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`](../Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md)
- [`Servo_flute_ESP32/docs/AUDIT_CODE.md`](../Servo_flute_ESP32/docs/AUDIT_CODE.md)

A hardware row must remain **NOT TESTED — requires hardware** until the test is physically executed and its actual result is recorded.

## Recommended validation order

1. Verify power rails, common ground, fuse, emergency disconnect, and PCA9685 OE behavior without servos connected.
2. Validate one finger servo with current limiting or a conservative supply.
3. Validate all fingers at low speed and check mechanical collisions.
4. Validate the selected valve and airflow servo.
5. Test panic, browser disconnect, and manual-test timeout.
6. Test BLE, Wi-Fi, serial MIDI, and local MIDI playback separately.
7. Add the INMP441 and validate microphone detection and placement.
8. Run per-note airflow calibration on a real instrument.
9. Validate fan or direct-pump modes.
10. Validate reservoir sensors and pump shutdown on sensor loss.

## Documentation maintenance rule

Feature documents should explain behavior and configuration. Audit history, temporary validation notes, and global hardware status should be maintained here or in the hardware test matrix rather than duplicated across every document.
