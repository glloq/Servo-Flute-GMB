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

### Hotspot key

The hotspot is **never open**. When `AP_PASSWORD` is left empty in `settings.h`
(the default), the firmware generates a 14-character WPA2 key at first boot from
the ESP32 hardware random generator and stores it in NVS. The key is printed on
the serial console at every boot, together with the web admin password.

The key is a real secret: it is not derived from the MAC address or the BSSID,
both of which are broadcast in clear in every 802.11 frame and would make the key
trivially guessable. Setting `AP_PASSWORD` to at least 8 characters overrides the
generated key with a fixed one.

`POST /api/auth/hotspot` (authenticated) regenerates the key and prints the new
one on the serial console. Holding the BOOT button for 5 s while the board powers
up regenerates both the hotspot key and the web admin password — the
physical-presence recovery path for a headless instrument.

## Station mode

After credentials are saved, the ESP32 connects to the configured WiFi network and exposes the web UI and rtpMIDI on that network.

## Mode transitions

Every AP <-> STA switch goes through a single pair of entry points
(`stopNetworkServices()` / `startNetworkServices()`), so a transition always:

1. runs the transport-lost panic first when an rtpMIDI session could be open — a
   note held while `forceAP()` fires would otherwise never receive its Note Off
   and would leave the valve, the airflow, the pump and the fan running;
2. stops the captive DNS and mDNS exactly once (mDNS is now ended, not restarted
   on top of itself);
3. switches the radio;
4. restarts mDNS, rtpMIDI and the captive DNS once each.

This covers the station timeout fallback, a lost station link, `forceAP()` from
the BOOT button, and a reconnection requested from the web UI.

## Button behavior

- Short press: restart BLE advertising or show the WiFi IP address.
- Double press: open all fingers (refused while the hardware is not ready or an
  actuator session owns the fingers).
- Long press in WiFi mode: force AP setup mode.
- Held during power-up (5 s): regenerate the hotspot key and the web admin
  password, printed on the serial console.
