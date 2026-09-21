# Firmware and Web Configuration Audit

Date: 2026-07-20
Scope: `RuntimeConfig`, `ConfigStorage`, `InstrumentManager`, `FingerController`, `AirflowController`, `PressureController`, `FanController`, `NoteSequencer`, `AutoCalibrator`, `WebConfigurator`, embedded `web_content.h`, BLE-MIDI, rtpMIDI/Wi-Fi MIDI, serial MIDI, MIDI file playback, PCA9685 routing, GPIO use, and servo power management.

## Summary of findings and fixes

| Area | Severity | Finding | Correction | Required tests |
| --- | --- | --- | --- | --- |
| Boot safety | Critical | `initSafeState()` initialized PCA9685 outputs and moved servos using compiled defaults before `/config.json` was loaded. | Boot safe state now disables PCA OE immediately, sets only known fixed critical GPIOs safe, starts LittleFS, loads and validates configuration before hardware managers can position actuators. | Power-on with custom `/config.json`; verify no default servo movement before config load. |
| Runtime validation | Critical | Limits and conflicts were partly enforced in HTML or ad hoc request parsing only. | Added centralized `validateAndNormalizeConfig()` with explicit limits, PCA conflict checks, GPIO reserved/input-only checks, min/max relationship checks, MIDI/note/fingering checks, and restart-required detection. | Native validation tests plus invalid REST payloads returning HTTP 400. |
| Configuration migration | High | `anglePcaChannel` and `angleServoPcaChannel` duplicated the same concept; legacy JSON keys existed. | `angleServoPcaChannel` is the canonical field. `ang_pca` and `angle_ch` are migrated on load/POST; saves emit `angle_ch` only. | Load old configs containing `ang_pca`; save and reload. |
| Valve servo direction | Medium | `valveServoDir` was saved but close/open angles already define travel and direction. | The field is removed from `RuntimeConfig`; saves never emit `vlv_dir` and POST ignores it for backward compatibility. | Load old config with `vlv_dir`; test close/open angle commands. |
| Sequencing timestamps | High | Live events and scheduled events shared relative timestamp logic that could keep stale references. | Event queue now has explicit live/scheduled enqueue methods and stores absolute execution times; queue reset clears timing reference. | Live note-on/off; MIDI file playback; queue-empty then new sequence. |
| Minimum note duration | High | `minNoteDurationMs` was persisted but not enforced. | `NoteSequencer` defers normal note-off until the minimum duration expires while keeping panic/all-sound-off immediate. | Short note-off test and panic bypass test. |
| Pressure calculations | Critical | Hall and ToF span divisions could divide by zero; pump cascade threshold 100 could divide by zero; direct pump demand was converted twice. | Validation rejects equal sensor thresholds/ranges, clamps cascade to 0..99, runtime guards zero denominators, and direct mode feeds logical demand to a single physical PWM conversion path. | Pump direct/reservoir tests for thresholds 0 and 99, equal sensor bounds, absent sensor. |
| Reservoir sensor absence | Critical | Reservoir mode without sensor silently behaved like direct pump mode. | Reservoir mode now stops pumps, clears bang-bang/PID state, and does not run direct mode when the sensor is absent. | Disconnect sensor during mode 5 and verify pumps off/fault diagnostics. |
| Autonomous fan/pump behavior | High | Fan and pump operation depended on WebSocket manual targets for some modes. | Added persistent autonomous defaults for fan note/idle behavior, direct pump note/idle behavior, and reservoir autostart target. MIDI note-on/off now drives fan/pump targets without a web page. | BLE-MIDI, DIN MIDI, rtpMIDI, and MIDI file playback without web client. |
| REST/WebSocket parsing | High | WebSocket command parsing used `String::indexOf()` and accepted malformed/out-of-range data. | WebSocket messages are parsed with ArduinoJson, capped at 512 bytes, range-checked, and panic on disconnect returns hardware to safe state. Config POST is size-limited and returns structured JSON including `restart_required`. | Invalid JSON, oversize WS frame, bad finger index, out-of-range note/CC. |
| Diagnostics | Medium | No structured hardware/config diagnostics API existed. | Added `GET /api/diagnostics` and `POST /api/diagnostics/run` with config, LittleFS, PCA requirement, microphone, restart, and heap checks. | Call endpoints before/after invalid config; hardware PCA checks require device. |

## Fields requiring restart

The validator marks `restart_required` when persisted changes affect pin modes, I2C/PCA routing, controller initialization, or MIDI serial setup: number of fingers, finger PCA channels, airflow/valve/angle PCA channels, air mode, valve type, solenoid GPIO, fan GPIO, pump count or pump GPIOs, motor type, sensor type, Hall/endstop GPIOs, and serial MIDI enable/RX pin.

## Dynamic fields

CC defaults, note airflow percentages, note angle percentages, finger patterns, velocities, expression/vibrato values, fan/pump temporary targets, and PID target percentages are dynamic when they do not require a new `pinMode()`, PCA begin, sensor begin, or MIDI serial begin.

## Remaining hardware tests

No physical ESP32, PCA9685 board, sensor, pump, fan, valve, microphone, or MIDI hardware was available in this environment. All rows in `HARDWARE_TEST_MATRIX.md` are therefore marked `NOT TESTED — requires hardware` until executed on a real bench.


---

# 2026 concurrency, actuator-safety and security audit

Date: 2026-09-21
Scope added to the sections above: FreeRTOS task interactions (`loop()` vs the
AsyncTCP server task vs the NimBLE host task), actuator command paths, runtime
configuration changes, filesystem failure handling, ToF sensor initialisation,
network mode transitions, and web access control.

## Findings and fixes

| # | Area | Severity | Finding | Correction | Tests |
| --- | --- | --- | --- | --- | --- |
| 1 | `EventQueue` / `NoteSequencer` | P0 | `enqueue`/`dequeue`/`clear` were locked, but the consumer still did `peek() -> read the pointer -> dequeue()`. Between the two, another task could `clear()` the queue, insert a forced Note Off (which evicts the head when the queue is full), move `_tail`, or fire a panic: the sequencer could execute a stale event, lose one, or reorder two. | `peek()` is gone. `tryPopDueEvent()` computes the due time, copies the event and removes **that same event** under **one** critical section; `peekCopy()` returns a value, never a pointer into the ring. `clear()` bumps an epoch so a burst already in progress is abandoned. Every accessor that reads shared state takes the lock. | `eventqueue_*`, `sequencer_no_event_executed_after_clear`, `test_eventqueue_exposes_no_internal_pointer` |
| 2 | Actuator command paths | P0 | AsyncTCP and NimBLE callbacks drove the PCA9685 over I2C, wrote actuator GPIOs and mutated `cfg` while `loop()` was doing the same. | New `CommandQueue`: callbacks post, `InstrumentManager::update()` applies. A panic is a dedicated undroppable flag that also discards pending commands. Web operations that touch `cfg`, LittleFS, the MIDI player or the calibrator use a bounded one-slot hand-off to `loop()`. | `commandqueue_panic_never_dropped_and_cancels_pending`, `test_actuator_commands_are_centralised_on_the_loop_task` |
| 3 | `POST /api/config` | P0 | The handler mutated the global `cfg` field by field **before** validating it, so `loop()` could observe a half-applied or invalid configuration. | `ConfigCommit`: candidate copy → apply JSON → normalise → validate → decide restart → save → single atomic assignment on the loop task. A validation or storage failure changes nothing; a restart-required change is saved but not activated. | `config_commit_*`, `test_config_commit_is_transactional` |
| 4 | `hardware_not_ready` | P0 | Web commands used `getAirflowCtrl()`, `getPressureCtrl()`, `getFanCtrl()` and the calibrator directly, bypassing the guards in `noteOn()` / `setPWM()`. After a PCA failure, pumps and the fan could still be driven. | Central refusal in `InstrumentManager::applyCommand()` (the single application point) plus an explicit `hardware_not_ready` answer in the web layer. Diagnostics, configuration read/write, reset and network recovery stay available. | `hardware_not_ready_*`, `test_hardware_not_ready_is_enforced_centrally` |
| 5 | LittleFS | P0/P1 | `LittleFS.begin(true)` reformatted the partition on the first mount failure, silently destroying `/config.json` and the MIDI files and restarting on defaults that may not match the wiring. | Mount without automatic format, one retry, then a recovery state: OE stays high, actuators refused, explicit diagnostics. Formatting happens only through a confirmed `POST /api/fs/format`. | `test_littlefs_is_never_formatted_automatically` |
| 6 | Vibrato | P1 | `SIN_LUT` is `int8_t` but was read with `pgm_read_byte()`, which returns an unsigned byte: the negative half of the sine came back as 129..255. The vibrato was unipolar and twice too wide upwards. | `VibratoMath` reads the table as `int8_t`; the host stub keeps the real unsigned `pgm_read_byte` semantics so the bug cannot hide again. Non-finite frequencies are guarded. | `vibrato_lut_is_signed`, `vibrato_fast_sin_quadrants_and_guards` |
| 7 | MIDI rate limiting | P1 | CC 124-127 were counted by the rate limiter and could be dropped, leaving a note blowing. A CC2 burst could drop the value that asked for silence. | CC 120-127 bypass the limiter entirely and are applied immediately. CC2 below `cc2SilenceThreshold` always passes; above it the **last** value is coalesced and applied on the next window instead of being thrown away. | `cc_channel_mode_messages_are_never_rate_limited`, `cc2_silence_request_is_never_dropped`, `cc2_last_value_is_coalesced_not_lost` |
| 8 | MIDI upload | P1 | `_uploadFile` / `_uploadSize` / `_uploadFileName` / `_uploadError` were server-wide, so two clients interleaved their bytes and destination names. The destination file was also deleted **before** the upload was validated. | One exclusive upload slot owned by a single request (second client → `409 upload_busy`), a unique temp file per transfer outside `/midi`, abandoned-transfer cleanup, and full validation (name, extension, size, quota, real MIDI parse) **before** the existing file is replaced. | `test_midi_upload_is_single_owner_and_validates_before_replacing` |
| 9 | VL53L0X | P1 | The branch only checked that something ACKed at 0x29, then wrote the start register and read a range register that had never been configured. | New `TofSensor`: model-ID check, data init, reference SPAD selection, ST default tuning, GPIO interrupt configuration, VHV and phase reference calibrations, non-blocking single-shot with range-status validation. Distinct states: absent / unsupported device / init failed / ready / fault. | `tof_presence_is_not_initialisation`, `tof_nonblocking_stale_safety`, `test_tof_sensor_is_really_initialised_not_just_probed` |
| 10 | Wi-Fi transitions | P2 | AP/STA switches re-ran `MDNS.begin()` and the AppleMIDI setup without ever stopping them, and a `forceAP()` during a held note never released it. | `stopNetworkServices(panic) / startNetworkServices()` centralise every transition: transport-lost panic first, then a single teardown, then a single restart of mDNS, rtpMIDI and the captive DNS. | `test_network_transitions_are_centralised_and_safe` |
| 11 | Hotspot key | P2 | The WPA2 key was `"flute-%06X"` derived from the low 24 bits of the MAC — a value broadcast in clear in every 802.11 frame and in the BSSID. | `DeviceSecrets`: a 14-character key from the hardware RNG, generated at first boot, stored in NVS, printed on serial, regenerable. Never derived from MAC or BSSID. | `test_hotspot_key_is_a_real_secret_not_the_mac` |
| 12 | Web access control | P0 (security) | No authentication at all on the REST API or the WebSocket. | Session tokens (`WebAuth`): login with an admin password generated at first boot, `X-Auth-Token` on every mutating route, in-band WebSocket authentication, sliding expiry, constant-time comparison, BOOT-hold recovery. Informative routes stay public. | `web_auth_sessions`, `web_auth_secret_comparison`, `test_web_interface_requires_authentication` |
| 13 | JSON | P1 | `res_format` and several payloads were concatenated without escaping; the Wi-Fi scan used a hand-written escaper. | Every free-form field goes through ArduinoJson (`jsonStr()` or a full document): device name, SSID, file names, messages, `res_format`, colour, embouchure. Status broadcast, scan results and upload/load answers are fully serialised. | `test_runtime_strings_are_json_escaped` |
| 14 | Configuration validation | P1 | Floats (`vibratoFrequencyHz`, `vibratoMaxAmplitudeDeg`, `cc2ResponseCurve`) and many timings were never validated: NaN, zero periods and absurd delays reached the servo maths. | `isfinite()` plus bounds on every float, bounds on every remaining timing, ADC and storage value, 7-bit clamping of the CC defaults, and closed sets for `embouchure` / `resFormat` / `instrumentColor`. | `config_float_validation`, `config_string_and_timing_validation`, `test_config_validation_covers_floats_and_timings` |
| 15 | Diagnostics | P1 | `/api/diagnostics` answered `pca0 = warning, hardware probe requires device` although `InstrumentManager` already knew the probe result. | The endpoint reports the real state: hardware ready, PCA0/PCA1 detected and required, config load status, filesystem status, microphone, sensor (present / initialised / valid / stale), heap, MIDI transports, actuator session, restart pending, dropped commands. Still strictly passive. | `test_diagnostics_report_real_state` |

## Fields requiring restart

Unchanged from the section above. The decision is now a pure predicate
(`InstrumentManager::configChangeRequiresRestart()`) evaluated **before**
anything is written, so the commit can save the new configuration while keeping
the old one active until the reboot.

## Remaining hardware tests

The rows added at the end of `HARDWARE_TEST_MATRIX.md` (prefix `AUD-`) cover this
audit. They remain `NOT TESTED — requires hardware`.
