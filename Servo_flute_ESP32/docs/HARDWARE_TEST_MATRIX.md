# Hardware Test Matrix

Status convention: `NOT TESTED — requires hardware` means the procedure is defined but was not executed in this software-only environment.

| ID | Configuration | Preconditions | Steps | Expected result | Actual result | Status | Comments |
| --- | --- | --- | --- | --- | --- | --- | --- |
| AIR-0 | Air mode 0: solenoid + airflow servo | ESP32, PCA9685 0x40, solenoid driver with flyback diode | Boot, play note, release note, panic | Fingers move before valve opens; valve closes and airflow rests; panic safe | Not executed | NOT TESTED — requires hardware | Verify configured solenoid GPIO and polarity. |
| AIR-1 | Air mode 1: valve servo + airflow servo | PCA channel for valve servo configured | Test close/open/cycle, play note | Valve servo uses close/open angles only and returns closed | Not executed | NOT TESTED — requires hardware | Legacy direction ignored. |
| AIR-2 | Air mode 2: airflow servo only | PCA9685, no valve | Play/release note | Airflow servo opens and returns rest; no valve output active | Not executed | NOT TESTED — requires hardware | Validate no solenoid heating. |
| AIR-3 | Air mode 3: fan + airflow servo | Fan MOSFET on configured PWM GPIO | Boot, BLE-MIDI note, no web page | Fan idles/defaults, follows note demand, times out safely | Not executed | NOT TESTED — requires hardware | Test 25/50/75/100% ramp. |
| AIR-4 | Air mode 4: direct pumps + valve | 1-3 pumps configured | Play notes via DIN MIDI and MIDI file | Pumps follow note demand and return idle/off after note-off | Not executed | NOT TESTED — requires hardware | Test PWM and on/off motors. |
| AIR-5 | Air mode 5: reservoir pumps + valve | Reservoir sensor connected | Enable autostart target and no web client | Pumps maintain reservoir target independently of notes | Not executed | NOT TESTED — requires hardware | Sensor loss must cut pumps. |
| PCA-1 | One PCA9685 | Only channels 0-15 used | Run diagnostics | 0x40 required, 0x41 not required | Not executed | NOT TESTED — requires hardware | Confirm OE pin behavior. |
| PCA-2 | Two PCA9685 | Any channel >=16 configured | Run diagnostics, play high-channel note | 0x41 required and used | Not executed | NOT TESTED — requires hardware | Verify I2C address jumpers. |
| PUMP-1 | One pump | Pump 0 wired | Direct and reservoir tests | One pump starts/stops within configured min/max | Not executed | NOT TESTED — requires hardware | Check startup threshold. |
| PUMP-2 | Two pumps cascade/stagger | Pumps 0-1 wired | Demand below/above cascade threshold | Pump 1 starts after stagger only above threshold | Not executed | NOT TESTED — requires hardware | Test threshold 0 and 99. |
| PUMP-3 | Three pumps parallel/cascade | Pumps 0-2 wired | Full demand and panic | All pumps obey mode and panic off | Not executed | NOT TESTED — requires hardware | Watch inrush current. |
| SENSOR-HALL | KY-024 Hall | Hall on analog GPIO | Set empty/full, move reservoir | Live bar changes; equal thresholds rejected | Not executed | NOT TESTED — requires hardware | Validate inversion if used mechanically. |
| SENSOR-VL53 | VL53L0X ToF | I2C sensor installed | Detect, set empty/target/full | Distance valid and pumps stop out of range | Not executed | NOT TESTED — requires hardware | Address 0x29. |
| SENSOR-VL618 | VL6180X ToF | I2C sensor installed | Detect, set empty/target/full | Distance valid and pumps stop out of range | Not executed | NOT TESTED — requires hardware | Address 0x29. |
| SENSOR-END-M | Mechanical endstop | NC switch recommended | Toggle switch during reservoir test | Logic matches configured active level; pumps safe on fault | Not executed | NOT TESTED — requires hardware | Check broken-wire behavior where possible. |
| SENSOR-END-O | Optical endstop | Optical endstop wired | Block/unblock beam | State changes and pump logic safe | Not executed | NOT TESTED — requires hardware | Verify pullups. |
| MIDI-BLE | BLE-MIDI | BLE central available | Connect and play scale | All notes play without web page | Not executed | NOT TESTED — requires hardware | Confirm channel filter. |
| MIDI-RTP | rtpMIDI | Wi-Fi network | Connect AppleMIDI session and play | All notes play without web page | Not executed | NOT TESTED — requires hardware | Test Wi-Fi loss. |
| MIDI-DIN | Serial MIDI DIN | UART RX wired | Send note/CC stream | Notes and CCs processed, rate limits safe | Not executed | NOT TESTED — requires hardware | Restart required after RX pin change. |
| MIDI-FILE | Local SMF file | LittleFS and uploaded MIDI | Upload/load/play type 0 and type 1 | Playback follows schedule; panic purges queue | Not executed | NOT TESTED — requires hardware | Also test truncated files. |
| SYS-RESET | Restart/factory reset | Saved config present | Save restart-required config, restart, factory reset | Safe boot, persisted config loaded, factory reset returns defaults | Not executed | NOT TESTED — requires hardware | Watch servos stay disabled before config. |
| SYS-WS | WebSocket loss | Web UI connected during test | Start pump/fan/servo test, close tab | All actuators return safe | Not executed | NOT TESTED — requires hardware | Disconnect handler invokes all-sound-off. |
| SYS-WDT | Watchdog | Firmware running | Induce controlled stall in debug build | System restarts with safe OE disabled first | Not executed | NOT TESTED — requires hardware | Do not perform on unattended hardware. |
| MIC | Microphone present/absent | INMP441 optional | Boot with/without mic and run diagnostics | Diagnostics distinguish detected/absent; calibration refuses absent mic | Not executed | NOT TESTED — requires hardware | No external network assets. |
| MIC-WIRING | INMP441 I2S wiring | BCLK=14, WS=15, SD=32, L/R→GND, 3V3 | Boot, open mic monitor | Mic detected; VU + pitch update on sound | Not executed | NOT TESTED — requires hardware | 3V3 only; left slot. |
| MIC-PLACEMENT | Mic placement / wind screen | Mic 5–15 cm off-axis with foam | Play sustained note near jet vs in jet | Off-axis + foam gives stable pitch; in-jet saturates | Not executed | NOT TESTED — requires hardware | Avoid direct air blast. |
| MIC-NOISE | Adaptive noise floor | Pumps/fan running | Run per-note calibration | Per-note noise measured; threshold scales with noise | Not executed | NOT TESTED — requires hardware | Noise = valve closed, air at rest. |
| MIC-CAL-AIR | Per-note airflow calibration | Instrument sounding | Run auto-calibrate all notes | min/nominal/max, confidence, cents, stability, SNR per note | Not executed | NOT TESTED — requires hardware | Neighbour/octave rejected; overblow = upper limit. |
| MIC-CAL-FAIL | Failed note keeps old values | One note cannot sound | Run calibration | Failed note reported ok:false; old config unchanged | Not executed | NOT TESTED — requires hardware | No overwrite on failure. |
| MIC-CAL-AIRSUPPLY | Air source ready before measuring | Air mode 3/4/5 (fan or pump/reservoir) | Run per-note calibration | Fan/pump/reservoir spins up first; notes sound and calibrate | Not executed | NOT TESTED — requires hardware | Source is at representative demand while the noise floor is measured. |
| MIC-CAL-AIRFAIL | Air source never ready | Air mode 3/4/5 with pump/fan disconnected | Run calibration | All notes fail with ACAL_FAIL_AIR_SUPPLY; nothing written; source stopped | Not executed | NOT TESTED — requires hardware | Bounded by AUTOCAL_AIR_READY_TIMEOUT_MS. |
| MIC-CAL-TIMEOUT | Global calibration timeout | Calibration running | Wait beyond AUTOCAL_GLOBAL_TIMEOUT_MS | Calibration aborts, valve closed, acal_error published | Not executed | NOT TESTED — requires hardware | Firmware-side timeout, not browser. |
| MIC-CAL-DISCONNECT | Client disconnect during calibration | Web UI running calibration | Close the browser tab | Calibration stops, hardware returns to safe state | Not executed | NOT TESTED — requires hardware | Owner disconnect stops actuator test. |
| MIC-RANGE | Servo range finder | Middle note sounding | Run range finder, apply | Usable servo min/max detected via exact-note pitch | Not executed | NOT TESTED — requires hardware | Bounded safe angle window (AUTOCAL_RF_MIN/MAX_SAFE_ANGLE, default 30–150°); apply is storage-checked. |
| ANGLE | Transverse angle servo present/absent | `embouchure=trav`, angle channel configured | Test angle and CC74 | Angle servo moves only when enabled and returns rest | Not executed | NOT TESTED — requires hardware | Validate PCA conflicts. |

## Finalization validation status

Software CI covers ESP32 firmware build, PlatformIO native behavior tests, pytest audits, JSON escaping regressions, MIDI 7-bit WebSocket bounds, request-size limits, diagnostics vocabulary, and supported air-management modes. Physical validation remains explicitly marked **NOT TESTED — requires hardware** until executed; current post-audit software changes do not claim hardware PASS on the corresponding PCA9685, microphone, ToF, Hall, endstop, pump, fan, solenoid, and servo hardware.

| POST-PCA0-MISSING | Boot with PCA0 absent | Disconnect PCA0 | Boot firmware | OE remains disabled, diagnostics report PCA0 missing | Not executed | NOT TESTED — requires hardware | Software stub regression added. |
| POST-PCA1-MISSING | Boot with PCA1 required absent | Configure channel >=16, disconnect PCA1 | Boot firmware | OE remains disabled, diagnostics report PCA1 missing | Not executed | NOT TESTED — requires hardware | Software stub regression added. |
| POST-RES-AUTOSTART | Reservoir autostart without browser | Reservoir mode + autostart | Boot without web client | Regulation starts only with sensor present | Not executed | NOT TESTED — requires hardware | Software stub regression added. |
| POST-MANUAL-TIMEOUT | Solenoid/pump/fan timeout | Start manual test | Wait beyond firmware timeout | Actuator stops | Not executed | NOT TESTED — requires hardware | Required before merge. |
| POST-TOF-BLOCKED | ToF absent/blocked | Disconnect or hold ToF bus | Run firmware | Loop remains responsive | Not executed | NOT TESTED — requires hardware | Required before merge. |
| POST-DIAG-CANCEL | Active diagnostic cancelled | Start selected diagnostic | Cancel/disconnect | Hardware stops | Not executed | NOT TESTED — requires hardware | Required before merge. |
| POST-WS-2CLIENTS | Two WebSocket clients | Connect two browsers | Disconnect non-owner then owner | Only owner disconnect stops manual test | Not executed | NOT TESTED — requires hardware | Required before merge. |
