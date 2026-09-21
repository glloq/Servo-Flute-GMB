# Air Management

The air system is modular. The user selects an air mode and the web interface shows only the relevant controls.

## Modes

| Mode | Name | Description |
|------|------|-------------|
| 0 | Solenoid + servo flow | A GPIO solenoid opens/closes air; a PCA servo controls flow. |
| 1 | Servo valve + servo flow | A PCA servo replaces the solenoid valve. |
| 2 | Servo flow only | The flow servo also cuts air between notes. |
| 3 | Fan + servo flow | A PWM fan blows continuously; the servo directs the flow. |
| 4 | Pump(s) + valve | One to three pumps provide direct air through a valve. |
| 5 | Pump(s) + reservoir + valve | Pumps fill a reservoir and a sensor regulates pressure/level. |

## Pumps

Pump modes support one to three independent pumps. PWM motors can be controlled proportionally. On/Off motors use bang-bang control with hysteresis.

## Reservoir sensors

Reservoir mode supports ToF distance sensors (VL53L0X, VL6180X), Hall sensors,
and endstops. PWM motors can use PID control; On/Off motors use threshold
control.

### ToF sensor states

A device acknowledging on I2C is **not** a working sensor. The driver
(`TofSensor`) identifies the part by its model ID and then runs the full
initialisation sequence — for the VL53L0X: data init, reference SPAD selection,
the ST default tuning table, interrupt configuration, and the VHV and phase
reference calibrations. Without that, the range register holds a value with no
metric meaning.

Diagnostics and the live status therefore distinguish four things:

| State | Meaning | Pump behaviour in reservoir mode |
|---|---|---|
| `absent` | nothing answers at 0x29 | stopped |
| `unsupported_device` | something answers, but it is not the configured sensor | stopped |
| `init_failed` | the sensor answered but its initialisation did not complete | stopped |
| `ready` + valid measurement | initialised and returning a fresh, valid range | regulated |
| `ready` + stale measurement | no valid reading for `TOF_STALE_MS` | stopped |
| `fault` | repeated timeouts invalidated the sensor | stopped |

Ranging is non-blocking: a single-shot measurement is started and its status is
polled once per loop iteration, so MIDI, WebSocket, audio and servo timing are
never stalled waiting on I2C. A measurement the sensor itself rejects (range
status other than 11) is treated as "no measurement", never as a distance.

## Valve and flow servo

Physical valves prevent sound while fingers are moving. The flow servo controls note airflow and expression.

## Web UI

The Air tab displays a dynamic pneumatic diagram and live values for pump, fan, valve, reservoir, sensor, and servo state.

## 2026 runtime safety and validation update

Configuration is validated by the firmware, not only by HTML controls. PCA channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only output pins, invalid min/max relationships, invalid MIDI channels, invalid fingering values, and unsafe pump/sensor bounds are detected before saving.

Parameters that change `pinMode()`, I2C/PCA routing, controller `begin()` behavior, sensor setup, or serial MIDI setup require a restart. Dynamic musical values such as CC defaults, note airflow/angle percentages, fingering patterns, and temporary fan/pump test targets can be applied without hardware reinitialization.

Manual hardware tests must always be time-limited and followed by a safe state. In software-only validation, hardware procedures are documented but marked `NOT TESTED — requires hardware` in `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.

## Finalization validation status

Software CI covers ESP32 firmware build, PlatformIO native behavior tests, pytest audits, JSON escaping regressions, MIDI 7-bit WebSocket bounds, request-size limits, diagnostics vocabulary, and supported air-management modes. Physical validation remains explicitly marked **NOT TESTED — requires hardware** until executed on the corresponding PCA9685, microphone, ToF, Hall, endstop, pump, fan, solenoid, and servo hardware.

### Calibration assistants by hardware type

The web UI must expose only steps applicable to the selected air mode: microphone auto-calibration remains limited to airflow servo range and per-note min/max breath; guided manual assistants cover angle servo rest/min/max/per-note/CC74, servo valve closed/open/cycle/return-closed, solenoid polarity/PWM activation/PWM hold/activation time/auto-close, fan start threshold/min/max/idle/timeout/velocity tracking, direct pumps min/max/order/cascade/stagger/safety stop, and reservoir Hall/ToF/endstop/PID/hysteresis/sensor-absent checks.

## Post-audit autonomous air safety

Reservoir mode can autostart from the persisted `reservoirTargetPercent` only when `reservoirAutoStart=true` and the configured reservoir sensor is detected. If the sensor is absent, the firmware keeps pumps stopped and does not fall back to direct-pump behavior. `pump_stop`, panic, reset and factory-reset paths must stop pumps.

Fan and direct-pump demands are intended to follow the effective acoustic note state, including monophonic replacement and minimum note duration. Hardware validation remains required for fan, pump and valve timing.
