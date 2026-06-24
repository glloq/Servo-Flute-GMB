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
