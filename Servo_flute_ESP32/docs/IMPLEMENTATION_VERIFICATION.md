## GitHub Actions executable verification

Status: FAIL (GitHub connectivity blocked from this environment; CI workflow added but not pushed or observed).

Date: 2026-07-20 UTC.

### Real repository/context checks requested for this pass

Local commands executed before modifying files:

- `git status`: branch `work`, clean tree.
- `git branch --show-current`: `work`.
- `git remote -v`: no configured remotes.
- `git log --oneline --decorate --all -30`: local `HEAD` was `9d97a6f Merge pull request #71 from glloq/codex/poursuivre-la-contre-verification-du-depot`; the clone contains merge history through PR #71.
- `git show --stat 79490e5835378a794468fa9c80f8f3c4f2e081d4`: failed with `fatal: bad object 79490e5835378a794468fa9c80f8f3c4f2e081d4`.

GitHub/network checks attempted:

- `git ls-remote https://github.com/glloq/Servo-Flute-GMB.git`: failed with `CONNECT tunnel failed, response 403`.
- GitHub REST API requests to `https://api.github.com/repos/glloq/Servo-Flute-GMB`, `/branches`, and `/pulls?state=all`: failed with `Tunnel connection failed: 403 Forbidden`.
- `python -m pip install --upgrade platformio`: failed with repeated `Tunnel connection failed: 403 Forbidden`.

Because neither GitHub nor PyPI was reachable from this execution environment, no verified GitHub branch, PR URL, Actions run id, ESP32 flash/RAM usage, or PlatformIO-native run result can be claimed in this file. The workflow added in this pass is intended to make those checks reproducible once pushed from an environment with GitHub/PyPI access.

### Commit presence table

| Commit | Present on GitHub | Branch | Pull Request | Ancestor of current branch | Action required |
| --- | --- | --- | --- | --- | --- |
| `87a4afe221d2f770fdc97e2ead07baa5bbfd60b8` | Unknown: GitHub blocked | Unknown | Unknown | No: absent locally | Re-query GitHub from connected environment; pressure PWM equivalent is present locally in `965e003`. |
| `79490e5835378a794468fa9c80f8f3c4f2e081d4` | Unknown: GitHub blocked | Unknown | Unknown | No: absent locally | Re-query GitHub; do not assume this disconnected clone contains it. |
| `c7a6fd4` | Unknown: GitHub blocked | Unknown | Unknown | No: absent locally | Re-query GitHub; local equivalent PlatformIO/native work appears as `838e5eb`/PR #71 history. |
| `67ca0fe` | Unknown: GitHub blocked | Local merge history | PR #69 by local commit message only | Yes | Use as historical local context only until GitHub is verified. |
| `76edef7` | Unknown: GitHub blocked | Local merge history | PR #68 by local commit message only | Yes | Use as historical local context only until GitHub is verified. |

### CI added

`.github/workflows/firmware-ci.yml` adds a non-optional workflow with:

- `ESP32 firmware build`: Python 3.12, PlatformIO install, `python -m platformio run -e esp32dev`.
- `PlatformIO native tests and Python audits`: Python 3.12, PlatformIO + pytest install, `python -m platformio test -e native`, then `python -m pytest -q`.

The workflow intentionally does not use `continue-on-error` or `|| true`; missing dependencies, firmware compile failures, native test failures, and pytest failures keep the PR red. Recommended required checks before merge are: `ESP32 firmware build`, `PlatformIO native tests and Python audits`, and the pytest step inside the native job.

### PlatformIO project audit update

`platformio.ini` now declares the real sketch/source directory as `Servo_flute_ESP32`, the ESP32 target as `esp32dev`, the Arduino framework on `espressif32@6.10.0`, and pinned dependencies for ArduinoJson, Adafruit PCA9685/BusIO, ESPAsyncWebServer/AsyncTCP, BLE-MIDI, AppleMIDI, and NimBLE. The native environment compiles selected production translation units, including the extracted production `ConfigValidator.cpp`, instead of relying on a validation stub.

### Config validation update

The pure validation function has been extracted from `ConfigStorage.cpp` into `ConfigValidator.cpp`. `ConfigStorage.cpp`, the firmware entry points, native host tests, and the future PlatformIO native test path use the same production `validateAndNormalizeConfig(RuntimeConfig&, const RuntimeConfig*)` implementation. The former native validation stub has been removed; only the global `RuntimeConfig cfg` test instance remains in `native_test/src/config_test_stub.cpp`.

### Local executable checks in this pass

- `python -m pytest -q`: PASS, `8 passed in 7.96s`.
- `python -m platformio test -e native`: FAIL locally because PlatformIO is not installed and PyPI access is blocked.
- `python -m platformio run -e esp32dev`: NOT EXECUTED locally for the same PlatformIO/PyPI limitation.

### GitHub Actions run results

| Job | Run URL | Commit | Result | Flash | RAM |
| --- | --- | --- | --- | --- | --- |
| ESP32 firmware build | Not available: workflow not pushed/observable from blocked environment | Not available | FAIL / not observed | Not available | Not available |
| PlatformIO native tests and Python audits | Not available: workflow not pushed/observable from blocked environment | Not available | FAIL / not observed | N/A | N/A |

### Result for this pass

Overall result remains **FAIL** under the requested criteria because the branch is still not connected to a verified GitHub remote in this environment, no real PR URL was obtainable, GitHub Actions could not be observed, ESP32 firmware compilation was not proven, and PlatformIO native tests were not executed. Hardware rows remain **NOT TESTED — requires hardware**.

---

## Executable verification pass

Status: FAIL

Date: 2026-07-20 UTC.

### Scope and repository checks

Commands started with `git status`, `git branch --show-current`, `git log --oneline --decorate -20`, `git show --stat 87a4afe221d2f770fdc97e2ead07baa5bbfd60b8`, and `git merge-base work main`.

Findings:

- Active branch: `work`.
- Working tree before changes: clean.
- Local history contains the prior pressure-controller fix commit `965e003 fix: correct pressure controller PWM scaling audit` and the merge commit `108564b`.
- The announced object `87a4afe221d2f770fdc97e2ead07baa5bbfd60b8` is not present in the local clone, even after `git fetch --all --prune`.
- `main` is not present as a local ref, so `git merge-base work main` failed with `fatal: Not a valid object name main`.
- The actual pressure fix is present in `PressureController.cpp`: PID branches now pass logical `0..255` PWM to `setPumpPwm()`, and `writePumpHw()` remains the single physical pump min/max mapper.

### Tool versions

- Python: `Python 3.14.4`.
- PlatformIO: NOT INSTALLED. `python3 -m pip install --user --upgrade platformio` failed because the package index tunnel returned `403 Forbidden` for every retry. `python3 -m platformio --version` failed with `No module named platformio`.
- Existing PlatformIO lookup checked `~/.platformio`, `~/.local/bin`, `/usr/local/bin`, `/opt`, and `import platformio`; no installation was found.
- Native compiler used for fallback executable proof: `g++` with `-std=c++17`.

### PlatformIO project definition added

A repository `platformio.ini` was added with:

- `env:esp32dev`: `platform = espressif32`, `board = esp32dev`, `framework = arduino`, ArduinoJson and Adafruit PCA9685/BusIO libraries.
- `env:native`: `platform = native`, custom test framework, `-DUNIT_TEST`, native Arduino/Wire stubs, and source isolation so native tests compile selected production files instead of uncontrolled ESP32 hardware code.

PlatformIO commands attempted:

- `python3 -m platformio project config` — FAIL, module missing.
- `python3 -m platformio run --list-targets` — FAIL, module missing.
- `python3 -m platformio run` — FAIL, module missing.
- `python3 -m platformio test -e native` — FAIL, module missing.

Because the ESP32 firmware did not compile in PlatformIO, the overall result for this pass is `FAIL` under the requested criteria.

### Executable native fallback proof

A pytest-driven native C++ executable was added and run successfully as fallback evidence while PlatformIO was unavailable.

Command:

```text
python3 -m pytest -q
```

Result:

```text
8 passed in 11.89s
```

C++ behavioral executable command used by pytest:

```text
g++ -std=c++17 -DUNIT_TEST -Inative_test/include -IServo_flute_ESP32 native_test/src/arduino_stubs.cpp native_test/src/config_test_stub.cpp Servo_flute_ESP32/PressureController.cpp Servo_flute_ESP32/EventQueue.cpp Servo_flute_ESP32/FingerController.cpp Servo_flute_ESP32/AirflowController.cpp Servo_flute_ESP32/FanController.cpp Servo_flute_ESP32/NoteSequencer.cpp tests/cpp/test_behavior.cpp -o <tmp>/behavior
```

Production files directly compiled and executed by the native behavioral executable:

- `Servo_flute_ESP32/PressureController.cpp`
- `Servo_flute_ESP32/EventQueue.cpp`
- `Servo_flute_ESP32/FingerController.cpp`
- `Servo_flute_ESP32/AirflowController.cpp`
- `Servo_flute_ESP32/FanController.cpp`
- `Servo_flute_ESP32/NoteSequencer.cpp`

Native fakes/stubs capture GPIO/PWM writes, analog reads, digital reads, I2C calls, serial logging, and `millis()`.

### Native behavioral coverage added

C++ behavioral cases added:

- PressureController direct logical PWM conversion for `0`, `64`, `128`, `192`, `255` with physical range `80..200`.
- PressureController per-pump physical mapping for three pumps with ranges `60..180`, `90..220`, `110..255` equivalent coverage in configured arrays.
- On/Off pump branch rejects intermediate PWM as a hardware state and writes only HIGH/LOW.
- Hall PID branch executes production PID code and verifies that logical PID PWM is physically mapped exactly once.
- Guard coverage for `hallThresholdLow == hallThresholdHigh`, `pumpCascadeThreshold = 100`, `numPumps = 0`, and `numPumps > MAX_PUMPS` through executable code and validation stub.
- EventQueue empty, full, insertion failure, drain, reference reset, reuse, purge, and timestamp near `uint32_t` overflow.
- NoteSequencer live note-on path, finger-before-air sequencing, minimum note duration (`100 ms`), deferred note-off at `t=20`, still playing at `t=99`, stop after minimum duration, and panic stop before the minimum duration.
- FanController autonomous no-WebSocket operation: note-on speed, idle after note-off, idle timeout, and panic/stop.
- Six airflow modes execute initialization, note-on/open, note-off/close, rest/safe path, and no-WebSocket operation through production `AirflowController` code.

Remaining gaps keep the software verification from being a full `PASS`:

- PlatformIO could not be installed due network/index `403 Forbidden`; ESP32 firmware compilation was not executed.
- PlatformIO native environment could not be executed.
- The validation tests call a native validation stub because production `ConfigStorage.cpp` still depends on ArduinoJson/LittleFS and was not isolated into a pure validation translation unit in this pass.
- Hardware tests were not executed.

### PWM mutation test

Temporary mutation introduced in `PressureController.cpp`:

```text
setPumpPwm(cfg.pumpMinPwm[0] + (uint16_t)(cfg.pumpMaxPwm[0] - cfg.pumpMinPwm[0]) * pwm / 255);
```

This reintroduced double conversion in the Hall PID branch only. The mutation was restored before commit.

Mutated command result:

```text
python3 -m pytest -q tests/test_native_behavior.py
FAILED tests/test_native_behavior.py::test_cpp_behavioral_production_sources
Assertion `__analog_writes[25]==physical(80,200,127)' failed.
```

After restoration:

```text
python3 -m pytest -q tests/test_native_behavior.py
1 passed in 11.59s
```

### Test classification

- `tests/test_static_audit.py`: static source inspection.
- `tests/test_native_behavior.py`: integration-style host test that compiles and executes selected production C++ sources with native fakes.
- `tests/cpp/test_behavior.cpp`: behavioral unit/integration executable for production controllers under fake hardware.
- No hardware tests were executed.

### Compilation and memory

| Environment | Result | Flash | RAM | Warnings |
| --- | --- | --- | --- | --- |
| esp32dev | FAIL | NOT TESTED | NOT TESTED | PlatformIO unavailable (`No module named platformio`) |
| native | PARTIAL | N/A | N/A | Native fallback executable passed under g++; PlatformIO native unavailable |

### Tests

| Suite | Type | Executed | Passed | Failed | Skipped |
| --- | --- | ---: | ---: | ---: | ---: |
| `python3 -m pytest -q` | static source inspection + native C++ behavioral fallback | 8 | 8 | 0 | 0 |
| `python3 -m pytest -q tests/test_native_behavior.py` | native C++ behavioral fallback | 1 | 1 | 0 | 0 |
| `python3 -m platformio test -e native` | PlatformIO native | 0 | 0 | 1 | 0 |
| `python3 -m platformio run` | ESP32 firmware build | 0 | 0 | 1 | 0 |

### Hardware tests remaining

Status: `NOT TESTED — requires hardware`.

- ESP32 boot with custom `/config.json` and PCA9685 attached.
- PCA9685 OE behavior during reset, invalid config, and panic.
- Real pump PWM electrical response for PWM and On/Off pumps.
- Reservoir sensor loss and recovery with VL53L0X/VL6180X/Hall/endstop hardware.
- BLE-MIDI, MIDI DIN, rtpMIDI, and MIDI file playback on target hardware.
- WebSocket manual actuator timeout/disconnect ownership on target hardware.
- Fan current draw/ramp behavior under load.

### Pull request status

`gh` is not installed in the environment. Commands attempted:

- `gh pr status` — FAIL: `gh: command not found`.
- `gh pr list --head work` — FAIL: `gh: command not found`.

A PR could not be verified or created with the GitHub CLI in this environment. A PR creation/update was requested via the repository PR helper after committing.

### Recommendation

`requires software corrections`
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
