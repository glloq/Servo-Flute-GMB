# PCA9685 Expansion

The firmware supports up to two PCA9685 boards, allowing channels 0-31.

## Current behavior

Board 1 uses address `0x40`. Board 2 uses address `0x41` and is initialized automatically when any configured channel is 16 or higher.

## Channel mapping

| Channel range | Board | Local channel |
|---------------|-------|---------------|
| 0-15 | PCA9685 #1 | 0-15 |
| 16-31 | PCA9685 #2 | 0-15 |

Users configure only the global channel number. The firmware maps it to the correct board internally.

## Hardware notes

- Solder A0 on the second PCA9685 board to set address `0x41`.
- Each board has its own V+ servo power terminal.
- Size the 5V supply for the total number of servos.
- Keep I2C wires short; add stronger pull-ups for long cables if needed.

## UI support

The web interface accepts PCA channels 0-31 for fingers, airflow, valve servo, and angle servo.
