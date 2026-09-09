# General-Midi-Boop automatic recognition

Servo Flute GMB implements the **General-Midi-Boop instrument recognition and
capability protocol, version 2**. When the instrument appears as a MIDI device,
General-Midi-Boop (GMB) probes it, reads a capability descriptor, and creates or
updates the instrument entry on its side without any manual entry.

Everything announced is derived from the **active, validated Servo-Flute
configuration**. There is no second capability model in the firmware: if a value
is not in `RuntimeConfig`, it is not announced.

---

## 1. Automatic discovery

```text
Servo-Flute starts
        ↓
loads the active configuration (LittleFS /config.json)
        ↓
validates it (validateAndNormalizeConfig)
        ↓
builds the GMB capability snapshot          gmb/Capabilities.cpp
        ↓
builds and caches the JSON descriptor       gmb/GmbDescriptor.cpp
        ↓
MIDI connection appears on the host
        ↓
GMB sends           F0 7D 00 01 00 F7
        ↓
Servo-Flute replies with the 24-byte v2 handshake
        ↓
GMB sees descriptor_size > 0
        ↓
GMB requests block 0x10 segments (or fetches the HTTP descriptor)
        ↓
GMB validates the JSON and creates/updates the instrument
        ↓
the user changes the Servo-Flute configuration
        ↓
validated + committed + activated
        ↓
revision++ → descriptor rebuilt → block 0x11 sent
        ↓
GMB refreshes the capabilities automatically
```

All GMB messages share the same header:

```text
F0 7D 00 <block> <direction> ... F7
```

`7D` is the experimental/educational manufacturer id and `00` the GMB id.
Directions are `00` request, `01` response, `02` notification.

---

## 2. Block 1 — handshake

Request (6 bytes):

```text
F0 7D 00 01 00 F7
```

Response, **exactly 24 bytes**:

```text
F0 7D 00 01 01
02                  proto_ver
<instance_id[5]>    32-bit, little-endian 7-bit
<firmware[3]>       major, minor, patch
<descriptor_size[3]> 21-bit, little-endian 7-bit
<revision[5]>       32-bit, little-endian 7-bit
<flags>
F7
```

| Offset | Size | Field | Source in this firmware |
|---|---|---|---|
| 0-4 | 5 | header | fixed |
| 5 | 1 | `proto_ver` | always `0x02` |
| 6-10 | 5 | `instance_id` | ESP32 eFuse MAC (see §7) |
| 11-13 | 3 | `firmware` | `FIRMWARE_VERSION_*` in `settings.h` |
| 14-16 | 3 | `descriptor_size` | byte length of the cached descriptor |
| 17-21 | 5 | `revision` | persistent capability revision (§8) |
| 22 | 1 | `flags` | bit 0 = HTTP descriptor reachable, bit 1 = change notifications |
| 23 | 1 | `F7` | |

**32-bit encoding.** Bytes 0-3 carry seven bits each and byte 4 carries bits
28-31 as a full nibble. This is what `DeviceManager.parseGmbHandshake()` decodes;
the legacy 3-bit high byte would silently drop bit 31 of an instance id.

**Flags bit 0** is only set while the embedded web server is running, which is
the Wi-Fi mode. In BLE mode there is no HTTP route, so the flag stays clear and
GMB uses the segmented SysEx transfer instead.

---

## 3. Block 0x10 — descriptor transfer

Request:

```text
F0 7D 00 10 00 <chunk_index[2]> F7
```

Response:

```text
F0 7D 00 10 01 <total_chunks[2]> <chunk_index[2]> <payload…> F7
```

- payload: **200 bytes maximum**, which keeps a whole message at 210 bytes and
  under the BLE-MIDI reassembly limit;
- `total_chunks` / `chunk_index` are 14-bit little-endian (2 × 7 bits);
- the document is 7-bit ASCII, so no packing is needed;
- any segment may be requested, in any order, any number of times;
- an out-of-range index produces **no response at all**, so a controller can
  never reassemble a document from a segment that does not exist.

**Stability during a transfer.** The descriptor is rendered once, when a
configuration is activated, and served from a cached string. A transfer is
pinned to the document it started on: if the user saves a new configuration
mid-transfer, the remaining segments still come from the original document and
the block 0x11 notification tells GMB to restart with the new revision. The pin
is released after the last segment, or after five seconds of silence if the
controller abandons the transfer.

A typical descriptor is 700-1200 bytes, i.e. 4 to 6 segments, fetched once per
connection.

---

## 4. Block 0x11 — capability change notification

Emitted spontaneously, direction `0x02`, exactly 12 bytes:

```text
F0 7D 00 11 02 <revision[5]> <change_flags> F7
```

| Bit | Name | Emitted when |
|---|---|---|
| 0 | `IDENTITY_CHANGED` | device name, MIDI channel, instrument name, GM program, type/subtype |
| 1 | `INSTRUMENTS_CHANGED` | playable notes, polyphony, expression, physical description |
| 2 | `TIMING_CHANGED` | any announced timing value |
| 3 | `RESTART_REQUIRED` | the change needs a reboot to take effect |

The notification is sent **only** after the new configuration has been

1. validated,
2. committed to LittleFS,
3. made active,
4. found to actually change the announced capabilities (revision++),
5. used to rebuild the cached descriptor.

An intermediate web-UI draft never produces a notification. A change that
requires a reboot is not announced either: it is not active, the device keeps
running the previous configuration, and the next boot detects the new
capabilities and advances the revision then.

The notification is an optimisation. GMB also re-reads block 1 periodically and
compares the revision, so a missed notification only delays the refresh.

---

## 5. Descriptor

```json
{
  "gmb_descriptor": 2,
  "revision": 1,
  "device": { "name": "ServoFlute", "model": "Servo-Flute-GMB" },
  "instruments": [
    {
      "channel": 0,
      "configured": true,
      "name": "Transverse flute",
      "gm_program": 73,
      "type": "pipe",
      "subtype": "flute",
      "notes": { "mode": "discrete", "list": [82, 83, 84, 86, 88, 89, 91, 93, 95, 96, 98, 100, 101, 103] },
      "polyphony": { "max": 1 },
      "timing": {
        "prepare": { "base_ms": 105, "max_ms": 105, "silent": true },
        "excite": { "latency_ms": 50 },
        "min_note_ms": 10,
        "rearticulation_ms": 50
      },
      "expression": {
        "cc": [1, 2, 7, 11, 73],
        "pitch_bend": { "supported": false },
        "channel_aftertouch": false,
        "poly_aftertouch": false,
        "velocity": true
      },
      "physical": {
        "family": "winds",
        "embouchure": "trav",
        "finger_count": 6,
        "thumb_hole_count": 0,
        "half_holes": false,
        "air_source": "solenoid_valve",
        "jet_angle_control": false
      }
    }
  ]
}
```

Servo-Flute exposes exactly one logical instrument. The document is restricted
to 7-bit ASCII: any non-ASCII character of the device name is escaped as
`\uXXXX`, so the same bytes are valid over SysEx and over HTTP.

Unknown fields are ignored by GMB, so the flute-specific keys under `physical`
are safe extensions.

---

## 6. How each field is derived from the configuration

| Descriptor field | Configuration source |
|---|---|
| `device.name` | `deviceName` |
| `device.model` | fixed project name `Servo-Flute-GMB` |
| `revision` | persistent capability revision (§8) |
| `channel` | `midiChannel`; omni (0) is announced as channel 0, otherwise `midiChannel - 1` |
| `configured` | validation result **and** at least one playable fingering **and** a usable airflow travel |
| `name`, `gm_program`, `type`, `subtype` | `embouchure` (§6.1) |
| `notes` | the active fingering table (§6.2) |
| `polyphony.max` | always 1: `NoteSequencer` owns one note at a time and the body has one air column |
| `timing.prepare` | `servoToSolenoidDelayMs` |
| `timing.excite.latency_ms` | `solenoidActivationTimeMs`, only with a solenoid valve |
| `timing.min_note_ms` | `minNoteDurationMs` |
| `timing.rearticulation_ms` | `minNoteIntervalForValveCloseMs`, only in an air mode with a physical valve |
| `expression.cc` | the control changes the firmware really consumes (§6.3) |
| `expression.velocity` | `airVelocityResponse > 0` |
| `physical.embouchure` | `embouchure` |
| `physical.finger_count` | `numFingers` |
| `physical.thumb_hole_count` | fingers flagged `isThumbHole` |
| `physical.half_holes` | at least one announced fingering uses a half hole |
| `physical.air_source` | `airMode` + `valveType` |
| `physical.jet_angle_control` | transverse embouchure **and** `angleServoEnabled` |

### 6.1 Embouchure → GMB vocabulary

`type` is always `pipe`, the `InstrumentTypeConfig.js` key that owns GM programs
72-79. The subtypes below are existing keys of that same table; no new
vocabulary is invented.

| `embouchure` | Instrument name | `subtype` | `gm_program` |
|---|---|---|---|
| `trav` | Transverse flute | `flute` | 73 |
| `bec` | Recorder | `recorder` | 74 |
| `naf` | Native American flute | `shakuhachi` | 77 |
| `end` | End-blown flute | `shakuhachi` | 77 |
| `oca` | Ocarina | `ocarina` | 79 |

General MIDI has no Native American flute program; `shakuhachi` is the closest
declared timbre. GMB may be overridden by the user, and the override survives
until the instrument itself changes the field.

### 6.2 Playable notes

The note set is built from the active fingering table, never from a hard-coded
range. A configured note is announced only when it is actually playable:

- the MIDI number is 0-127;
- `airflowMaxPercent > 0` — a closed airflow window can never produce a sound,
  so an uncalibrated or disabled fingering is not announced;
- `airflowMinPercent ≤ airflowNominalPercent ≤ airflowMaxPercent`;
- every finger pattern value is closed / open / half.

If every semitone between the lowest and the highest playable note is present,
the set is announced as a range:

```json
"notes": { "mode": "range", "min": 60, "max": 84 }
```

Otherwise the exact list is announced:

```json
"notes": { "mode": "discrete", "list": [60, 62, 64, 65, 67] }
```

When no fingering is playable, the instrument is announced as
`{"channel": …, "configured": false}` and nothing else. GMB then falls back to
manual entry without overwriting a previous configuration. The firmware never
fabricates a fallback capability set.

### 6.3 Announced control changes

Only what the firmware really implements **and** the active configuration really
enables:

| CC | Announced when | Effect |
|---|---|---|
| 1 (modulation) | `vibratoMaxAmplitudeDeg > 0` | vibrato depth |
| 2 (breath) | `cc2Enabled` | breath-driven airflow, can silence the note |
| 7 (volume) | always | scales the airflow ceiling |
| 11 (expression) | always | scales the airflow ceiling |
| 73 (attack time) | always | selects the attack shape (stable / accent / crescendo) |
| 74 (brightness) | transverse embouchure with `angleServoEnabled` | air-jet angle |

CC 120/121/123-127 are honoured as MIDI mode messages but are not announced:
they are not expression controls.

Pitch bend, channel aftertouch and poly aftertouch are **not implemented** in
any MIDI path and are announced as explicitly unsupported. That is a fact the
firmware knows, unlike a latency it has never measured.

### 6.4 Two-phase timing

Servo-Flute separates a slow **silent** gesture from a fast **sounding** one, and
the descriptor keeps them apart:

- **`prepare`** is the finger/servo positioning. The sequencer holds the valve
  closed for `servoToSolenoidDelayMs` while the servos travel, so the gesture is
  inaudible and `silent: true`. GMB can anticipate it during MIDI-file playback,
  and it is **not** charged to latency compensation. The window is fixed rather
  than a function of the interval, so `base_ms` equals `max_ms` and
  `per_semitone_ms` is not announced.
- **`excite`** is what remains between opening the air path and the note
  speaking. Only a solenoid valve exposes a configured figure —
  `solenoidActivationTimeMs`, the full-power drive window that bounds the
  mechanical opening time.

The slow preparation time is never folded into `excite.latency_ms`.

**Unknown is omitted, never zero.** `excite.latency_ms` is absent for a servo
valve, a fan or a direct pump; `excite.jitter_ms` and `release_ms` are always
absent because nothing measures them. `rearticulation_ms` is absent in an air
mode with no physical valve, where notes are never re-articulated by closing.

---

## 7. Instance id

`instance_id` is the key GMB uses to reattach a stored configuration to the
right physical exemplar, so two boards flashed with the same binary must never
share it.

It is derived from the **ESP32 eFuse MAC** by hashing its six bytes (FNV-1a) into
32 bits. Hashing rather than truncating matters: masking each MAC byte to seven
bits would drop one bit in eight and map two neighbouring boards onto the same
identity. A result of 0 is replaced by 1, because `{0,0,0,0,0}` reads as "no
identity".

Properties: stable across reboots, different between two boards, independent of
the user-visible instrument name, and independent of the configuration.

The announced value is printed on the serial console at boot and exposed as
`gmb.instance_id` by `GET /api/status`.

---

## 8. Revision

`revision` is an ETag: GMB only re-downloads the descriptor when it changes.

The counter is driven by a **signature of the capability-relevant part of the
active configuration**, not by "something was saved":

```text
configuration change → validated → committed → active
        → signature recomputed
        → different?  yes → revision++ → persisted → descriptor rebuilt → block 0x11
                       no → nothing at all
```

Consequences, all of them required by the protocol:

- a plain reboot recomputes the same signature and changes nothing;
- a no-op save changes nothing;
- saving Wi-Fi credentials or a UI preference changes nothing — they are not
  announced capabilities;
- flash is only written when the counter actually moves.

The counter lives in NVS (`Preferences`, namespace `gmb`), not in
`/config.json`: a configuration save never rewrites the counter, a counter bump
never rewrites the configuration, and the identity of the board survives a
factory reset of the configuration.

The first boot after flashing starts at revision 1. If the configuration changed
while the firmware was not running — a file replaced offline, or a
restart-required change applied by the reboot — the next boot notices the
different signature and advances the counter once.

Three sub-signatures (identity, instruments, timing) produce the block 0x11
change flags without a second source of truth.

---

## 9. HTTP descriptor endpoint

```text
GET /gmb/descriptor.json
```

Returns the same logical document as the block 0x10 transfer, because both read
the same cached string; they cannot diverge.

The route only exists in Wi-Fi mode, where the embedded web server runs, and
handshake flag bit 0 follows it exactly.

---

## 10. Supported MIDI transports

| Transport | SysEx discovery | Note / CC |
|---|---|---|
| BLE-MIDI | Yes — bidirectional once connected | Yes |
| rtpMIDI / AppleMIDI (Wi-Fi) | Yes — once a session has a participant; GMB normally prefers the HTTP descriptor here | Yes |
| Serial MIDI DIN | **No** — this board wires UART2 RX only, with no MIDI OUT | Yes, unchanged |
| Web keyboard / MIDI file player | Not a MIDI transport | Yes |

The protocol is implemented **once**, in `GmbSysExService`. A transport is only
a port:

```text
MIDI transport → SysEx message → GmbMidiBridge → GmbSysExService
              → protocol parsing → response bytes → same originating transport
```

A transport with no return path simply is not registered as a port and keeps
working normally for Note and CC traffic. Adding USB-MIDI later means
implementing `IGmbMidiPort` and registering it; no protocol code changes.

---

## 11. Real-time safety

GMB discovery is control-plane traffic and must not disturb the note path:

- the SysEx callback only **stages** the request into one fixed-size buffer; the
  response is built and sent from the main loop, never from inside the MIDI
  parse pass;
- a second request arriving before the loop runs is dropped, not queued, so a
  flood cannot grow memory;
- a token bucket serves a whole discovery burst (handshake plus every segment
  back to back) but caps a sustained flood;
- the descriptor is rendered once per configuration activation, never per
  request;
- no filesystem access and no web-server work happen on the SysEx path;
- a malformed, truncated, foreign-manufacturer, 8-bit or unknown-block message
  is ignored with no reply and no work;
- GMB processing never touches actuators: it cannot leave a servo, a valve or an
  air source in an unsafe state.

---

## 12. Troubleshooting

**GMB does not recognise the instrument at all.**
Check that the transport can carry SysEx both ways. A DIN-only connection has no
return path and can never be recognised automatically — select the instrument
manually in GMB, or use BLE or Wi-Fi.

**The handshake arrives but GMB stays in manual entry.**
Read `GET /api/status`: if `gmb.configured` is `false`, the active configuration
is not sufficient to play. The usual causes are a fingering table where every
note still has `airflowMaxPercent = 0` (never calibrated) or a configuration that
fails validation at boot; the serial console prints the validation error.

**GMB shows the instrument but no notes.**
Only playable fingerings are announced. Calibrate the per-note airflow (manually
or with the microphone auto-calibration) so each note has a non-zero maximum.

**Capabilities do not refresh after a configuration change.**
A change that reports `restart_required` is not active yet: restart the device,
and the revision advances on the next boot. A change that alters nothing GMB is
told about (Wi-Fi credentials, UI preferences) deliberately does not move the
revision.

**`gmb.revision` keeps increasing at every boot.**
That would mean the announced capabilities differ from what was persisted. Check
that the configuration loads cleanly — a configuration that falls back to
defaults at boot changes the signature.

**Two instruments behave as one in GMB.**
Compare `gmb.instance_id` on both boards via `GET /api/status`. They must
differ; the value is derived from the eFuse MAC.

**The descriptor is served but rejected.**
Fetch `GET /gmb/descriptor.json` and check it parses. The same bytes are sent
over SysEx, so an HTTP fetch is the quickest way to inspect what GMB receives.

---

## 13. Firmware modules

| File | Responsibility |
|---|---|
| `gmb/Capabilities.{h,cpp}` | builds the immutable capability snapshot from `RuntimeConfig` |
| `gmb/GmbDescriptor.{h,cpp}` | renders a snapshot as the v2 JSON descriptor (7-bit ASCII) |
| `gmb/GmbSysEx.{h,cpp}` | wire codec: handshake, descriptor segments, change notification, request parsing |
| `gmb/GmbSysExService.{h,cpp}` | transport-independent endpoint: cached descriptor, transfer pinning, rate limiting |
| `gmb/GmbInstanceId.{h,cpp}` | 7-bit codecs and the stable instance id |
| `gmb/GmbRevision.{h,cpp}` | capability signature and the revision decision logic |
| `gmb/GmbMidiPort.h` | what a transport must provide to take part |
| `gmb/GmbMidiBridge.{h,cpp}` | stages requests from transports, answers from the main loop |
| `gmb/GmbRuntime.{h,cpp}` | ESP32 glue: eFuse MAC, NVS persistence, the single change entry point |

Everything except `GmbRuntime.cpp` is platform-free and covered by the host
tests (`tests/test_native/test_gmb.cpp`, `tests/test_gmb_descriptor.py`).
