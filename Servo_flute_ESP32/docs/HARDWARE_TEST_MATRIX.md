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
| ANGLE | Transverse angle servo present/absent | `embouchure=trav`, angle channel configured | Test angle and CC74 | Angle servo moves only when enabled and returns rest | Not executed | NOT TESTED — requires hardware | Validate PCA conflicts. |
