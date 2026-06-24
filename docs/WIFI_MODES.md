# WiFi Modes

The firmware supports Bluetooth mode and WiFi mode. A physical switch selects the active wireless mode.

## Bluetooth mode

Bluetooth mode exposes BLE-MIDI through NimBLE. It is useful for phones, tablets, and computers that support BLE-MIDI.

## WiFi mode

WiFi mode provides:

- the embedded web interface;
- rtpMIDI / AppleMIDI on the local network;
- mDNS access through `servo-flute.local` when available;
- captive-portal setup in AP mode.

## Access point mode

On first boot, or when no WiFi credentials are stored, the ESP32 starts the `ServoFlute-Setup` hotspot. Connect to it to open the setup portal.

## Station mode

After credentials are saved, the ESP32 connects to the configured WiFi network and exposes the web UI and rtpMIDI on that network.

## Button behavior

- Short press: restart BLE advertising or show the WiFi IP address.
- Double press: open all fingers.
- Long press in WiFi mode: force AP setup mode.
