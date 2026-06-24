# Serial MIDI

Serial MIDI receives MIDI from hardware modules, keyboards, sequencers, or pedalboards through a standard 5-pin DIN input.

Serial MIDI runs in parallel with BLE-MIDI and WiFi rtpMIDI. Configure it from **Settings > MIDI Serial (DIN)** in the web interface.

## Electrical interface

Use a standard MIDI IN optocoupler circuit between the DIN connector and the ESP32 RX GPIO. The firmware uses the standard MIDI baud rate of **31250 baud**.

## RX pin

The RX GPIO is selectable in **Settings > MIDI Serial (DIN) > Pin RX**.

| GPIO | Notes |
|------|-------|
| 16 | Default RX2 pin, recommended |
| 17, 18, 19, 23, 25, 26, 27, 33 | General-purpose pins |
| 34, 35, 36, 39 | Input-only pins, ideal for RX |

Pins 34-39 are input-only and work well for MIDI reception.

## Notes

- A GPIO change requires an ESP32 restart.
- MIDI Serial is MIDI IN only; MIDI OUT/THRU is not implemented.
- Serial MIDI is filtered by the configured MIDI channel, just like the other MIDI inputs.
