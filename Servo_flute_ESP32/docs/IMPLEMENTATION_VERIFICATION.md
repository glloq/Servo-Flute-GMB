# Implementation verification counter-audit

Date: 2026-07-20  
Branch audited: `work`  
Base observed from history: `76edef7` / PR #68 merge; corrected scope merged by `67ca0fe` / PR #69.  
Physical hardware: not available in this execution environment.

## Global result

**PARTIAL — retested in code and static tests, but firmware compilation requires PlatformIO and all actuator/sensor paths require bench hardware.**

The counter-audit found one confirmed regression in `PressureController`: PID reservoir mode converted PID output into physical pump PWM before calling `setPumpPwm()`, whose contract already performs min/max physical mapping. That caused double application of pump min/max. It was corrected during this counter-audit and covered by a static regression test.

## Change scope reviewed

| Commit | Files concerned | Announced objective | Real implementation | Associated tests | Main risk |
|---|---|---|---|---|---|
| `f33378d` | `MidiFilePlayer.cpp`, `AirflowController.cpp` | Harden MIDI parser and vibrato against divide-by-zero | MIDI division zero is rejected and vibrato phase handles zero frequency | Static audit only in this repo snapshot; no native PlatformIO suite available | Embedded behavior not executed on target |
| `ff55c54` | Runtime config, pressure, sequencer, web API/UI, docs, `tests/test_static_audit.py` | Add audit fixes and matrices | Adds centralized validation, restart reporting, diagnostics endpoints, JSON WebSocket parsing, note minimum duration handling, hardware matrix | `python -m pytest -q` static tests | Tests mostly search source text and can miss logic regressions |
| `67ca0fe` | merge commit | Merge PR #69 correction branch | Brings the audit correction set onto `work` | Same as above | Diff is large and mixes firmware, UI, docs, and tests |
| counter-audit commit | `PressureController.cpp`, `tests/test_static_audit.py`, docs | Verify and fix incomplete correction | Removes double pump PWM conversion and documents verification | Added static regression for PID logical PWM contract | Needs hardware/simulation for electrical confirmation |

## Compilation and test results

| Environment | Command | Result | Warnings | Firmware size |
|---|---|---|---|---|
| ESP32 PlatformIO | `pio run` | **WARNING — not executed: `pio` command not installed** | `/bin/bash: line 1: pio: command not found` | Not available |
| Native PlatformIO tests | `pio test -e native` | **WARNING — not executed: `pio` command not installed** | `/bin/bash: line 1: pio: command not found` | Not applicable |
| Python static audit | `python -m pytest -q` | **PASS — 7 passed** | None | Not applicable |

## Initial issue traceability matrix

| ID | Initial problem | Severity | Origin file | Announced correction | Modified files | Function/class | Automatic test | Manual test | Result | Status | Comments |
|---|---|---:|---|---|---|---|---|---|---|---|---|
| CFG-001 | Configuration validation was split or browser-only | P0 | `ConfigStorage.*`, `WebConfigurator.cpp` | Central validator and API rejection | `ConfigStorage.cpp`, `WebConfigurator.cpp`, `.ino` | `validateAndNormalizeConfig` | `test_validation_entry_points_present` | Save invalid PCA/GPIO config via REST | Validator exists and is called on boot/load/save/API | PASS | Hardware impossible configs are rejected by API path, but not bench-tested |
| CFG-002 | Legacy angle channel keys could be lost or duplicated | P1 | `ConfigStorage.cpp`, web UI | Migrate `ang_pca`/`angle_ch` to canonical `angle_ch` | `ConfigStorage.cpp`, `web_content.h` | `ConfigStorage::load/save` | `test_legacy_angle_migration_and_no_valve_dir_save` | Load-save legacy JSON files | Save emits canonical key only | PASS | Exhaustive order-independent migration still requires native JSON tests |
| CFG-003 | Runtime changes requiring hardware reinit were silently applied | P1 | `WebConfigurator.cpp` | Report `restart_required` | `ConfigStorage.cpp`, `WebConfigurator.cpp`, `web_content.h` | validation diff with previous config | `test_restart_ui_and_api_present` | Save/restart workflow in browser | API/UI strings present; no browser run | PARTIAL | Dynamic/local reinit categorization is not exhaustive |
| JSON-001 | WebSocket parsed commands with fragile string search | P1 | `WebConfigurator.cpp` | Use ArduinoJson and body length limit | `WebConfigurator.cpp` | `processWsMessage` | `test_websocket_uses_arduinojson_not_indexof_type_parsing` | Send malformed/oversized WebSocket frames | Parser uses `deserializeJson` and 512-byte limit | PASS | Static only; target heap impact not measured |
| PRESS-001 | Cascade threshold 100 could divide by zero | P0 | `PressureController.cpp` | Clamp threshold and guard denominator | `PressureController.cpp`, `ConfigStorage.cpp` | `setPumpPwm`, validator | `test_pressure_zero_division_guards` | Pump cascade 99/100 on bench | Threshold normalized to <=99 and denominator guarded | PASS | Electrical behavior not bench-tested |
| PRESS-002 | Pump PWM could be mapped twice | P0 | `PressureController.cpp` | Keep `setPumpPwm()` as only physical min/max mapper | `PressureController.cpp` | PID branches, `writePumpHw` | `test_pressure_pid_outputs_logical_pwm_not_physical_pwm` | Pump PID ramp on bench | Corrected during counter-audit | PASS | Needs hardware/simulated analog sensor |
| PRESS-003 | Reservoir could keep pumping with missing sensor | P0 | `PressureController.cpp` | Stop pumps if sensor absent | `PressureController.cpp` | `update` | Static code inspection | Disconnect sensor in reservoir mode | Code stops pumps and resets bang-bang/PID | PARTIAL | Requires physical sensor-loss test |
| AIR-001 | Fan/pump modes depended on web commands | P1 | `AirflowController.cpp`, `FanController.cpp`, `PressureController.cpp` | Autonomous note targets and persistent reservoir target | multiple controllers/config | `setAirflowForNote`, `setTargetPercent` | Static inspection only | MIDI without browser | Code path exists from note handling to controllers | PARTIAL | Must be run with BLE/DIN/rtpMIDI and browser closed |
| AIR-002 | Vibrato frequency zero risk | P1 | `AirflowController.cpp` | Guard zero frequency/depth | `AirflowController.cpp` | vibrato update | Indirect static search from commit | Play with 0 Hz vibrato | Code inspection indicates guard | PARTIAL | No unit or hardware test executed |
| SEQ-001 | `minNoteDurationMs` had no effect | P1 | `NoteSequencer.cpp` | Delay Note Off until min duration | `NoteSequencer.cpp` | `handlePlaying`, `stop` | Static inspection only | Very short note on bench | Code defers matching Note Off | PARTIAL | Queue can still block on unrelated leading events; needs deeper tests |
| EVT-001 | Queue overflow/reference handling ambiguous | P2 | `EventQueue.cpp` | Capacity check and reference reset | `EventQueue.cpp` | `enqueueScheduledEvent`, `dequeue` | Static inspection only | Fill queue and recover | Code rejects full queue and resets empty reference | PARTIAL | No production-code native test exists |
| SAFE-001 | Startup could enable outputs before config validation | P0 | `Servo_flute_ESP32.ino` | Disable OE early, load/validate then initialize | `.ino`, `InstrumentManager.cpp` | `setup`, `safePreInitOutputs` | Static inspection only | Boot with invalid config/PCA absent | Order appears safe by inspection | PARTIAL | Requires target boot tests and PCA fault injection |
| TEST-001 | Manual actuator tests could leave outputs active | P0 | `WebConfigurator.cpp`, controllers | Central stop/timeout behavior | `WebConfigurator.cpp`, `AirflowController.cpp`, `PressureController.cpp` | WebSocket handlers/loop checks | Static inspection only | Disconnect WebSocket during each test | Some timeout hooks exist | PARTIAL | Owner/session semantics not fully proven |
| DIAG-001 | Diagnostics could report configured devices as present | P1 | `WebConfigurator.cpp` | Add diagnostics endpoint and states | `WebConfigurator.cpp` | `/api/diagnostics`, `/api/diagnostics/run` | Static inspection only | Run with missing PCA/mic/ToF | Diagnostics endpoints exist | PARTIAL | Need hardware to prove real detection |
| DOC-001 | Documentation overstated unverified hardware behavior | P2 | `docs/*` | Add audit notes and hardware matrix | docs and `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md` | docs | `test_hardware_matrix_marks_not_tested` | Review docs vs bench notes | Matrix explicitly marks no hardware tests | PASS | Keep matrix updated after bench tests |

## RuntimeConfig inventory summary

The full structure contains instrument shape, MIDI, timing, airflow, angle servo, vibrato, CC defaults, CC2, solenoid, attack, Wi-Fi/device/power, six air-system modes, valve, fan, pumps, reservoir sensors, PID/endstop/Hall, UI, MIDI storage, and color/keyboard fields. During this counter-audit, the highest-risk fields were traced as follows:

| Field group | JSON keys | Validation | Load/save | UI exposure | Firmware consumer | Apply category | Status |
|---|---|---|---|---|---|---|---|
| Fingers/notes | `num_fingers`, `fingers`, `notes` | Bounds, PCA conflicts, fingering values | Yes | Calibration/settings | `FingerController`, `NoteSequencer` | restart for count/PCA; dynamic for patterns | PARTIAL |
| Airflow servo | `air_pca`, `air_off`, `air_min`, `air_max` | PCA and min/max | Yes | Air/calibration | `AirflowController` | restart for PCA; dynamic for angles | PASS |
| Angle servo | `angle_ch`, legacy `ang_pca`, `ang_off`, `ang_min`, `ang_max` | PCA and min/max | Yes | Transverse UI | `AirflowController` | restart for PCA | PASS |
| Fan | `fan_pin`, `fan_min`, `fan_max`, `fan_idle_pct`, `fan_idle_timeout`, autonomous fan keys | PWM/pin/ranges | Yes | Air UI | `FanController` | restart for pin; dynamic for targets | PARTIAL |
| Pumps | `pump_*`, `num_pumps` | count, pins, min/max, cascade <=99 | Yes | Air UI | `PressureController` | restart for count/pins/type; dynamic for targets | PASS after counter-audit fix |
| Sensors/PID | `sens_*`, `hall_*`, `endstop_*`, `pid_*` | ranges and min/target/max order | Yes | Air UI | `PressureController` | restart for sensor/pins; dynamic PID | PARTIAL |
| Wi-Fi/MIDI/UI | `wifi_*`, `midi_channel`, `serial_*`, `ui_*`, colors | partial | Yes | Settings/UI | network/MIDI/UI | restart for serial; dynamic for UI | PARTIAL |

## Additional defects found

| Priority | Defect | Evidence | Correction |
|---|---|---|---|
| P0 | PID reservoir pump output was converted against `pumpMinPwm[0]`/`pumpMaxPwm[0]` before entering `setPumpPwm()`, which converts logical PWM to physical PWM for every pump. | `PressureController.cpp` PID branches | Changed both PID branches to pass logical `0..255` PWM to `setPumpPwm()` and added regression test. |
| P1 | Automated tests are mostly static text checks, not production-code execution. | `tests/test_static_audit.py` | Documented as residual risk; PlatformIO unavailable here. |
| P1 | Diagnostics cannot be fully trusted without target hardware because PCA/mic/ToF presence must be measured physically. | docs/hardware matrix | Kept hardware rows as `NOT TESTED — requires hardware`. |
| P2 | NoteSequencer only handles the current note's Note Off when it is at the queue head; overlapping or unrelated due events need native behavioral tests. | `NoteSequencer.cpp` inspection | Not changed in this pass; flagged for follow-up. |

## Tests still requiring physical hardware

All rows in `HARDWARE_TEST_MATRIX.md` remain **NOT TESTED — requires hardware**, specifically six air modes, one/two PCA boards, fan PWM, one/two/three pump modes, Hall/ToF/endstop sensors, microphone auto-calibration, angle servo, panic during calibration, WebSocket disconnect during actuator tests, BLE/DIN/rtpMIDI autonomous playback, filesystem corruption recovery, and watchdog reset safety.

## Recommendation

**Retested on hardware before merge.** The code-level counter-audit fixed a P0 pump-control regression and the Python checks pass, but the requested final PASS criteria cannot be met in this environment because PlatformIO is unavailable and no physical ESP32/PCA/sensor/actuator hardware is attached.
