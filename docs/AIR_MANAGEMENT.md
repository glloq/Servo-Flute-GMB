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

### What is detected, and what is only presumed

Only one of the three families can be identified. Saying otherwise is what let
the pump regulate on a sensor that was not there.

| Family | What `begin()` can establish | Reported state |
|---|---|---|
| ToF (VL53L0X / VL6180X) | The part answers its **model ID** and completes the full initialisation sequence. This is a real identification. | `ready` / `absent` / `unsupported_device` / `init_failed` |
| Hall KY-024 | Nothing on an ADC input says "KY-024". What *can* be established is that a reading is a **measurement**: a value pinned to either ADC rail (cut wire, short) or a configuration whose threshold band is degenerate is not one. | `presumed` (a plausible reading exists) / `presumed_no_reading` |
| Endstop (mechanical / optical) | **Nothing.** A disconnected input is electrically identical to a released dry contact; no pull direction distinguishes them. | `presumed` |

`isSensorDetected()` keeps its name for its callers but means "usable for
regulation", not "identified". The honest value is `sensorPresence()` /
`sensorStateName()`, which report `presumed` for Hall and endstops and never
claim a detection that did not happen. A fourth reported state, `no_effect`,
means the pump ran without the measurement moving (see below): the sensor may
well be fine and the fault be a leak or a closed valve.

### Pump safety rules, identical for every sensor family

The pump may run only while all three conditions hold. They do not depend on the
sensor family, and none of them depends on a configuration flag:

1. **A measurement was accepted.** Start-up included: before the first accepted
   reading there is no measurement, so the pump stays stopped. A rejected
   reading is never replaced by the last known value.
2. **That measurement is fresh.** `TOF_STALE_MS` (500 ms) for ToF sensors,
   `RESERVOIR_STALE_MS` (1000 ms) for Hall and endstops. Staleness used to
   return "fresh" unconditionally for the two non-ToF families, which disabled
   the guard for them entirely.
3. **The pump is doing something.** If it runs for `PUMP_MAX_RUN_MS` (60 s)
   without the fill measurement moving by at least `PUMP_PROGRESS_MIN_PERCENT`
   (5 %) in the direction the pump pushes it, the output is cut and **latched**.
   A legitimate fill resets that window every time it makes progress, so only a
   pump that achieves nothing is stopped — a dead sensor read as "empty", an
   endstop stuck in the "keep filling" direction whatever its polarity, a leak,
   a closed valve.

The latch clears on proof that the sensor is alive — the measurement moving
while the pump is stopped — or on an explicit `clearPumpRunawayFault()`, or on
restart. It does not clear merely because the pump stopped: that would turn the
time limit into a duty cycle and the pressure would keep climbing.

Manual single-pump tests are bounded by `PUMP_TEST_MAX_MS` (30 s). They are
started by one web command and stopped by another; a closed tab or a dropped
Wi-Fi connection must not leave a pump running.

The endstop input's internal pull follows the **declared inactive level**
(`INPUT_PULLDOWN` for an active-high endstop, `INPUT_PULLUP` for an active-low
one), because a dry contact only imposes one of the two levels. This is a
functional choice, not a safety one: rule 3 above is what covers a stuck or
disconnected endstop, in all four polarity combinations.

### ToF sensor states

A device acknowledging on I2C is **not** a working sensor. The driver
(`TofSensor`) identifies the part by its model ID and then runs the full
initialisation sequence — for the VL53L0X: data init, reference SPAD selection,
the ST default tuning table, interrupt configuration, and the VHV and phase
reference calibrations. Without that, the range register holds a value with no
metric meaning.

Diagnostics and the live status therefore distinguish these cases:

| State | Meaning | Pump behaviour in reservoir mode |
|---|---|---|
| `absent` | nothing answers at 0x29 | stopped |
| `unsupported_device` | something answers, but it is not the configured sensor | stopped |
| `init_failed` | the sensor answered but its initialisation did not complete | stopped |
| `ready` + valid measurement | initialised and returning a fresh, valid range | regulated |
| `ready` + stale measurement | no valid reading for `TOF_STALE_MS` | stopped |
| `fault` | repeated timeouts invalidated the sensor | stopped |
| `no_effect` | the range never moved while the pump ran for `PUMP_MAX_RUN_MS` | stopped and latched |

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

Reservoir mode can autostart from the persisted `reservoirTargetPercent` when
`reservoirAutoStart=true` and the reservoir sensor chain is usable. For a ToF
sensor that means **detected**: no model ID, no autostart. For a Hall sensor or
an endstop no detection exists, so autostart sets the *demand* and the pump
output is held at zero until a measurement is actually accepted — the autostart
gate is a convenience, never the safety.

This wording used to claim that autostart required the sensor to be "detected"
for every family. It was true only of the ToF path: for Hall sensors and
endstops, "detected" was a `pinMode()` call and nothing else, so a cut sensor
wire read as an empty reservoir and the PID drove the pump to full output
indefinitely, from power-on, with no note played and no web client connected.
What is guaranteed now is stated above, per family: the ToF sensor is
identified, the Hall reading is checked for being a measurement at all, the
endstop is not checked at all — and all three are covered by the same freshness
and no-progress rules, which stop the pump instead of continuing on the last
known value.

Reservoir mode still never falls back to direct-pump behavior when the sensor is
unusable: the pump stays stopped. `pump_stop`, panic, reset and factory-reset
paths must stop pumps.

That last sentence used to be an intention. It is now enforced: an order that
*removes* energy from an actuator no longer travels through the bounded
inter-task command ring, where a saturating WebSocket client could make it
disappear — `postCommand()` returned `false`, the web layer ignored that value,
and `endTestSession()` then disarmed the `TEST_SESSION_MAX_MS` net, leaving a
pump running at its setpoint with no time limit at all. `pump_stop`, `fan_stop`,
per-pump stop, `pump_enable=false`, closing the valve, **and a `pump_target` or
`fan_target` of 0** now set a dedicated flag that cannot be full, are applied at
the head of the next pass ahead of the per-pass work bound, and purge from the
ring anything that would re-energise the same actuator.

The zero setpoint deliberately keeps a channel of its own rather than being
folded into the hard stop: `PressureController::stop()` additionally ends a
running single-pump test and `FanController::stop()` skips the ramp-down, so
merging them would have changed what the sliders do. Only the loss is gone.

The asymmetry is the point. Commands that *add* energy — `pump_enable=true`,
opening the valve, any non-zero setpoint — stay in the ordinary ring and may
still be refused under saturation: losing a start-up is safe, losing a stop is
not.

Software cannot guarantee a pressure limit. Every rule above acts on the pump
*command*; none of them measures pressure, and a seized pump driver, a shorted
MOSFET or a firmware halt leaves the pump powered whatever the code decides. A
mechanical relief valve sized for the reservoir remains the only protection that
does not depend on this firmware. Direct-pump mode (mode 4) has no sensor and is
deliberately not time-limited: its demand is the player's, and cutting a held
note would be a musical failure with no safety gain.

Fan and direct-pump demands are intended to follow the effective acoustic note state, including monophonic replacement and minimum note duration. Hardware validation remains required for fan, pump and valve timing.
