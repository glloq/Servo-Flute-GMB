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

Reservoir mode supports ToF distance sensors, Hall sensors, and endstops. PWM motors can use PID control; On/Off motors use threshold control.

## Valve and flow servo

Physical valves prevent sound while fingers are moving. The flow servo controls note airflow and expression.

## Web UI

The Air tab displays a dynamic pneumatic diagram and live values for pump, fan, valve, reservoir, sensor, and servo state.

## 2026 runtime safety and validation update

Configuration is validated by the firmware, not only by HTML controls. PCA channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only output pins, invalid min/max relationships, invalid MIDI channels, invalid fingering values, and unsafe pump/sensor bounds are detected before saving.

Parameters that change `pinMode()`, I2C/PCA routing, controller `begin()` behavior, sensor setup, or serial MIDI setup require a restart. Dynamic musical values such as CC defaults, note airflow/angle percentages, fingering patterns, and temporary fan/pump test targets can be applied without hardware reinitialization.

Manual hardware tests must always be time-limited and followed by a safe state. In software-only validation, hardware procedures are documented but marked `NOT TESTED — requires hardware` in `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.
