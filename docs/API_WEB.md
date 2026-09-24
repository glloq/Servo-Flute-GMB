# Web API

The ESP32 exposes a REST API and a WebSocket endpoint used by the embedded web UI.

## REST endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/` | public | Main single-page web interface |
| GET | `/api/auth/status` | public | Whether the caller holds a valid session |
| POST | `/api/auth/login` | public | Exchange the admin password for a session token |
| POST | `/api/auth/password` | token | Change the admin password |
| POST | `/api/auth/hotspot` | token | Regenerate the hotspot WPA2 key |
| GET | `/api/status` | public | Runtime status JSON |
| GET | `/api/diagnostics` | public | Passive hardware / configuration diagnostics |
| POST | `/api/diagnostics/run` | public | Same passive report (no actuator movement) |
| GET | `/api/config` | public | Full runtime configuration (carries no secret) |
| POST | `/api/config` | token | Partial configuration update (transactional) |
| POST | `/api/config/reset` | token | Reset configuration to defaults |
| POST | `/api/config/factory` | token | Factory reset and reopen first-use wizard |
| POST | `/api/fs/format` | token | Recovery: erase and re-create LittleFS (explicit confirmation) |
| POST | `/api/restart` | token | Safe the hardware and restart |
| POST | `/api/midi` | token | Upload a MIDI file |
| GET | `/api/midi/list` | token | List stored MIDI files |
| POST | `/api/midi/load` | token | Load an existing MIDI file |
| POST | `/api/midi/delete` | token | Delete a stored MIDI file |
| GET | `/api/wifi/scan` | token | Start WiFi scan |
| GET | `/api/wifi/results` | token | Poll WiFi scan results |
| POST | `/api/wifi/connect` | token | Save WiFi credentials and connect |
| GET | `/api/wifi/status` | public | Current WiFi state |
| GET | `/gmb/descriptor.json` | public | General-Midi-Boop v2 capability descriptor |

### `GET /gmb/descriptor.json`

Returns the General-Midi-Boop v2 capability descriptor for the active
configuration, as `application/json`. It is byte-for-byte the document served
over the SysEx block `0x10` transfer — both read the same cached string, so the
two can never diverge. The route exists only in Wi-Fi mode, and the SysEx
handshake advertises it through flag bit 0 exactly when it does.

See [General-Midi-Boop protocol](GMB_PROTOCOL.md).

### `GET /api/status` — recognition keys

`/api/status` additionally reports the firmware version and the recognition
state, for troubleshooting automatic discovery:

```json
{
  "firmware": "1.1.0",
  "gmb": {
    "instance_id": "0x7AEFE22E",
    "revision": 3,
    "descriptor_size": 744,
    "configured": true,
    "flags": 3
  }
}
```

`flags` is the handshake flag byte: bit 0 = HTTP descriptor reachable, bit 1 =
change notifications supported. `configured` is `false` when the active
configuration is not sufficient to play, which is the case General-Midi-Boop
reads as "hand this instrument back to manual entry".

## WebSocket endpoint

`/ws` is used for real-time control and status updates.

Common client messages:

| Type | Example | Description |
|------|---------|-------------|
| `non` | `{"t":"non","n":82,"v":100}` | Note On |
| `nof` | `{"t":"nof","n":82}` | Note Off |
| `cc` | `{"t":"cc","c":7,"v":100}` | Control Change |
| `velocity` | `{"t":"velocity","v":100}` | Default keyboard velocity |
| `panic` | `{"t":"panic"}` | All sound off |
| `test_finger` | `{"t":"test_finger","i":0,"a":90}` | Move one finger servo |
| `test_note` | `{"t":"test_note","n":84}` | Test a full note position |
| `air_live` | `{"t":"air_live","v":50}` | Live airflow test |
| `pump_stop` | `{"t":"pump_stop"}` | Stop pumps |
| `fan_stop` | `{"t":"fan_stop"}` | Stop fan |

Server messages include status broadcasts, MIDI file events, audio-monitoring data, and auto-calibration progress.

### Microphone auto-calibration messages (server → client)

Client control messages: `{"t":"auto_cal","mode":"air"}` starts per-note airflow
calibration, `"mode":"range"` starts the servo range finder, `"mode":"stop"`
stops, `"mode":"apply_range"` applies range-finder results.

**Range-finder lifecycle.** When the range finder finishes, the server broadcasts
`rf_done` **once** and keeps the result *pending* (the calibrator stays in its
completed state). `apply_range` therefore remains valid until the owner applies,
cancels (`stop`), starts a new calibration, or disconnects. `rf_done` reports
`ok:false` with `reason`/`reasonName` (and `airError` for an air-supply failure)
when no usable range was found. Applying replies with `{"t":"rf_applied","ok":true,
"min":…,"max":…}` on success, or `{"t":"rf_applied","ok":false,"error":"…"}`
(`storage_failed` / `no_valid_range` / `config_busy`) when nothing was activated —
the servo range goes through the same transactional commit as the per-note
results. `config_busy` means persisted but not activated (lock refused); the
reply then carries `restart_required:true` and a controlled reboot is scheduled.

Live progress (`acal_prog`), one per airflow position:

```json
{
  "t": "acal_prog", "idx": 0, "note": "C6", "total": 14,
  "phase": "coarse", "air": 32,
  "rms": 0.043, "noise": 0.006,
  "hz": 1046.5, "midi": 84, "cents": -3.2,
  "confidence": 91, "validFrames": 5, "totalFrames": 5
}
```

`phase` is one of `prepare`, `noise`, `coarse`, `fine`, `nominal`, `done`,
`range`.

Completion (`acal_done`) reports overall persistence status and one result per
note:

```json
{
  "t": "acal_done", "ok": true, "applied": true, "saved": true,
  "validCount": 12, "failedCount": 2,
  "results": [
    { "name": "C6", "ok": true, "min": 20, "nominal": 39, "max": 68,
      "confidence": 91, "cents": -3.2, "stability": 0.94, "snr": 18.5,
      "reason": 0, "reasonName": "none" }
  ]
}
```

- `ok` is `true` only when the results were both **applied and saved**
  (`applied && saved`). On a LittleFS write failure nothing was ever written to
  the active configuration in the first place, so `applied` and `saved` are both
  `false`, `ok` is `false`, and an `"error":"storage_failed"` field is added — a
  client is never told a partial success while nothing was written.
- If the calibration was persisted but the configuration lock was refused, the
  reply carries `saved:true, applied:false`, `"error":"config_busy"` and
  `"restart_required":true`: the flash holds the new calibration, the RAM the
  old one, and a controlled reboot is scheduled to reconcile them.
- The calibrator now goes through the same transactional commit as the web path:
  it builds a candidate, validates it, persists it, and only then activates it
  under the lock. A calibration that fails at any step leaves the active
  configuration bit-for-bit unchanged.
- A failed note has `ok:false`, keeps its previous calibration, and reports both a
  numeric `reason` (`AutoCalFailureReason`: 0 none, 1 no-sound, 2 wrong-note,
  3 low-confidence, 6 no-stable-nominal, 7 audio-stale, 8 note-timeout,
  9 global-timeout, 10 air-supply-not-ready, …) and a textual `reasonName`
  (e.g. `"audio_stale"`).
- On a global-timeout abort the server sends `{"t":"acal_error","msg":"..."}`.
- The audio monitor stream (`{"t":"audio",...}`) carries much more than the
  calibration fields — see **Acoustic analysis stream** below.

**Ownership / concurrency.** A calibration is owned by the WS client that
started it. A second `auto_cal` start returns `{"t":"acal_error","msg":
"calibration_busy"}`; a non-owner stop/apply returns `{"t":"error","msg":
"not_calibration_owner"}`. Actuator commands (`test_*`, `non/nof/cc`, `play`,
pump/fan targets **and `pump_stop`/`fan_stop`**, `mic_mon`, `mic_reset`) sent
during a calibration are refused with `{"t":"error","msg":"calibration_active"}`.
The player `stop` command, when a calibration is active, cancels the calibration
cleanly (owner-gated) instead of fighting it with all-sound-off. Every
config-mutating REST route — `POST /api/config`, `POST /api/config/reset` and
`POST /api/config/factory` — returns HTTP `409 {"ok":false,
"error":"calibration_active"}` while a calibration (including a pending
range-finder result) is active.

**Microphone.** `GET /api/config` exposes `mic_status` (`detected` / `all_zero`
/ `stuck` / `saturated` / `read_error` / `not_init`). `{"t":"mic_reset"}` re-probes
the microphone without rebooting and replies `{"t":"mic_reset","ok":...,
"status":"..."}`.

Each `notes[]` entry in `GET/POST /api/config` includes `anm` (nominal airflow
percent) next to `amn`/`amx`; it is derived from min/max when absent
(backward-compatible migration) and validated as `0 ≤ amn ≤ anm ≤ amx ≤ 100`.

## Acoustic analysis stream and diagnostics

The microphone chain publishes through **two deliberately asymmetric channels**.
The split is not an oversight: this firmware forbids a permanent audio stream
over WebSocket on an ESP32-WROOM. Sending the full timing block and the named
`missing` flags on the periodic push would add ~403 bytes per message — a 2.6×
larger message and about +16 kB/s towards four clients — so they go only to the
on-demand diagnostics. An 11-byte bitmask carries the essential part live.

### `{"t":"audio", ...}` — periodic push (~100 ms, short keys)

Fields are **omitted when not measured**; an absent key means "unknown", never
zero.

| Key | Meaning | Emitted when |
|---|---|---|
| `rms`, `rms_dbfs`, `peak_dbfs` | frame level (linear, and dBFS — *digital* full scale, never dB SPL) | always |
| `clip`, `clip_ratio` | clipping, measured on the **raw** block before filtering | always |
| `snd` | sound above the monitoring gate | always |
| `hz`, `midi`, `cents`, `conf`, `valid` | pitch, nearest note, deviation, YIN confidence, and the **pitch detector's** verdict | a pitch was found |
| `stab` | pitch stability 0..1 | **only once measured** — see the trap below |
| `snr`, `snr_fb` | signal-to-noise against the machine-state profile, and whether a fallback profile was used | a noise profile exists |
| `centroid`, `flatness` | spectral shape | the FFT ran on *this* frame |
| `hnr`, `hnr_sp` | harmonic-to-noise ratio **and the scale it was measured on** | a spectrum is available |
| `h2`, `h3` | harmonic ratios | idem |
| `overblow` | octave-above pitch detected | when set |
| `st` | acoustic state name | the frame was classified |
| `miss` | bitmask of what the classification lacked | non-zero only |
| `q`, `qw` | quality score **and the weight it was computed on** | the score is valid |
| `br`, `brw` | breathiness **and its weight** | the value is valid |
| `squeak` | a *confirmed* squeak (candidates are not announced) | when confirmed |

`miss` bits: 0 pitch, 1 SNR, 2 spectrum, 3 expected note, 4 stability,
5 squeak history, 6 SNR fallback.

### `GET /api/diagnostics` → `audio` — full contract (long names)

Everything above, named rather than packed: `acoustic_state`,
`acoustic_classified`, `quality_score`, `quality_weight_used`, `quality_valid`,
`breathiness`, `breathiness_weight_used`, `breathiness_valid`, `hnr_valid`,
`hnr_db`, `hnr_is_spectral`, `snr_valid`, `snr_db`, `snr_fallback`, plus the
sub-objects `missing` (`pitch`, `snr`, `spectrum`, `expected_note`, `stability`,
`squeak_history`, `snr_fallback`) and `timing`.

`timing` carries `has_last`, `outcome`, `baseline_valid`, the two diagnostic
counters `rejected_frames` / `rejected_events`, and five measures —
`command_to_sound`, `air_to_sound`, `attack`, `pitch_stabilization`, `release` —
each as `{"valid":bool,"ms":float}`.

### Four traps a client must not walk into

These are not style notes. Each one has already produced a wrong verdict in this
project, and each was found by an audit rather than by the tests.

1. **Never average two scores with different `weightUsed`.** The score is a
   weighted mean renormalised over the criteria *actually* measured, and the
   harmonic component only exists on one frame in `MIC_SPECTRAL_DECIMATION`. In
   the nominal configuration `quality_weight_used` alternates between **0.90**
   (FFT frame) and **0.75** (decimated frame); `breathiness_weight_used` swings
   further, 1.00 to 0.30. A naive one-second mean would blend ~15 complete
   scores with ~47 partial ones and produce a number that means nothing. Group
   by the weight, or plot only the complete frames. (0.75 / 0.60 is the same
   alternation with no noise profile captured — a degraded configuration the
   diagnostics flags with a warning of its own.)
2. **Never read the HNR without its scale flag.** Two incomparable scales can
   fill the same field: a spectral measurement over the full analysis band, and
   a four-line Goertzel approximation used as a fallback. They differ by more
   than 30 dB on the *same* note and rank notes the wrong way round. `hnr_sp` /
   `hnr_is_spectral` says which one filled it. Comparing the raw number to a
   threshold makes the verdict flicker at 15.6 Hz.
3. **`valid` is the pitch detector's flag, not the stability flag.** They sit
   next to each other and mean different things. `stab` is emitted *only* when
   stability has actually been measured, which needs a full pitch history —
   about 112 ms. Zero would otherwise mean "not measured yet" just as much as
   "wildly unstable", and a consumer would mark the first 100 ms of every note
   as a defect.
4. **A timing measure without its `valid` is meaningless.** An `attack` of
   `{"ms":0}` read without the flag says "instantaneous attack". Every duration
   goes through a single emitter that cannot produce a bare `ms`.

`acoustic_state` takes the values of the acoustic-state enumeration — `silence`,
`good`, `weak`, `breathy`, `unstable`, `wrong_note`, `overblow`, `squeak`,
`clipping` — **plus `unclassified`**, which is not a state but the absence of
one, published when nothing could be classified. `acoustic_classified` carries
the same information as a boolean.

### Validation level

Everything this section describes is exercised on **synthetic PCM**, now passed
through the production filter chain. No INMP441 and no flute have ever been
connected to this project. See
[`Servo_flute_ESP32/docs/AUDIO_ARCHITECTURE.md`](../Servo_flute_ESP32/docs/AUDIO_ARCHITECTURE.md)
for the per-phase validation levels and for what the audit found still wrong.

## 2026 runtime safety and validation update

Configuration is validated by the firmware, not only by HTML controls. PCA channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only output pins, invalid min/max relationships, invalid MIDI channels, invalid fingering values, and unsafe pump/sensor bounds are detected before saving.

Parameters that change `pinMode()`, I2C/PCA routing, controller `begin()` behavior, sensor setup, or serial MIDI setup require a restart. Dynamic musical values such as CC defaults, note airflow/angle percentages, fingering patterns, and temporary fan/pump test targets can be applied without hardware reinitialization.

Manual hardware tests must always be time-limited and followed by a safe state. In software-only validation, hardware procedures are documented but marked `NOT TESTED — requires hardware` in `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.


## JSON serialization and diagnostics contract

REST and WebSocket payloads that include user-provided strings are serialized with ArduinoJson so escaping is correct for examples such as `deviceName = Flute "A"`, `SSID = atelier\wifi`, and `fichier = étude "test".mid`.

`GET /api/diagnostics` reports `dropped_commands` (actuator commands refused by a
full cross-task ring) and `queues_ok` (both internal queues really own their
storage). A queue whose allocation failed refuses everything instead of
dereferencing a null pointer; the command ring already showed up through
`dropped_commands` climbing, but a dead *event* queue was visible nowhere — the
instrument simply appeared to stop playing.

`GET /api/diagnostics` is passive: it reports `ok`, `warning`, `error`, `not_tested`, or `not_applicable` without moving actuators. `POST /api/diagnostics/run` is reserved for an explicit active, timeout-bounded hardware sequence that announces the tested components, supports stop/panic, returns outputs to a safe state, and keeps physical checks marked `NOT TESTED — requires hardware` until executed on real hardware.

## Post-audit API safety contract

`GET /api/diagnostics` and `POST /api/diagnostics/run` are passive and never move
hardware. An active test is requested explicitly through the WebSocket test
commands, is bounded by `TEST_SESSION_MAX_MS`, is cancellable, and belongs to the
client that started it. Variable strings in JSON responses go through a JSON
serializer so SSIDs, filenames, device names and error text are escaped.

Configuration reset and factory reset report `applied:false` and
`restart_required:true` after safing the hardware; they do not claim the defaults
are already active until the controlled reboot has happened. They no longer touch
the active configuration **at all**: it describes the hardware that was actually
initialised — PCA channels, GPIOs, angles — and overwriting it in RAM while the
loop runs would drive the actuators from a description that no longer matches the
wiring. `GET /api/config` therefore still returns the previous configuration
during the short window before the reboot.

Factory reset also removes the `.tmp` and `.bak` companions of the configuration
file. Leaving them behind did not make `isFirstBoot()` fail, it made it *lie*: the
boot path promotes a pending `.tmp` when the final file is missing, so the next
boot resurrected the configuration that had just been erased.

### Transactional configuration write

`POST /api/config` never mutates the running configuration while it parses.
It copies the active configuration into a candidate, applies the JSON to that
candidate, normalises and fully validates it, decides whether the change needs a
hardware re-init, persists the candidate, and only then replaces the active
configuration in a single assignment performed by the main loop. Consequences
visible from the API:

| Outcome | Response | Device state |
|---|---|---|
| Invalid candidate | `400 {"ok":false,"error":"<reason>"}` | unchanged (configuration and controllers) |
| Save failed | `500 {"ok":false,"saved":false,"error":"storage_failed"}` | unchanged; keeps running on the previously persisted configuration |
| Applied | `200 {"ok":true,"saved":true,"applied":true,"restart_required":false}` | new configuration active |
| Needs a hardware re-init | `200 {"ok":true,"saved":true,"applied":false,"activated":false,"restart_required":true,"restarting":true}` | new configuration saved, **old one still active**, actuators safed, controlled reboot scheduled |
| Configuration lock refused *during* the commit | `200 {"ok":true,"saved":true,"applied":false,"activated":false,"restart_required":true,"restarting":true}` + `warnings:["config_lock_timeout"]` | new configuration saved, **old one still active and untouched**, controlled reboot scheduled |
| Loop busy / not answering | `503 {"ok":false,"error":"busy"}` | unchanged |
| Configuration lock not obtained *before* the commit | `503 {"ok":false,"error":"config_busy"}` | unchanged — only `GET /api/config` and `GET /api/diagnostics` can answer this |

`activated` answers the one question the other flags only imply: **was the
active configuration really replaced?** It is never true unless the
configuration lock was actually held. The commit refuses to write the active
configuration without that lock — copying 5 KB while another task reads it is
exactly what the lock exists to prevent — so a refused lock leaves the flash
ahead of the RAM. That divergence is not hidden: it is bounded to one commit,
reported by `saved && !activated`, and resolved by the controlled reboot, which
reloads the flash with the matching hardware init.

Clients should treat `saved` as "it is in flash", never as "the device is now
running it". The WiFi credentials endpoint is the cautionary case: it used to
switch the network on `saved` alone, which left the active configuration
carrying the previous SSID — and the next `POST /api/config`, building its
candidate from that configuration, would have written the old credentials back
to flash.

A configuration change bumps the General-Midi-Boop revision only in the "applied"
row — validated, saved *and* active. A restart-required change is announced after
the reboot, when it really takes effect.

### MIDI upload

`POST /api/midi` holds an exclusive server-side upload slot. A second concurrent
client gets `409 {"error":"upload_busy"}` and never touches the transfer in
flight; an abandoned transfer releases the slot after `UPLOAD_LOCK_TIMEOUT_MS`.
Each transfer writes to its own temporary file outside `/midi`, so it is neither
listed nor counted against the quota, and the destination file is replaced only
**after** the upload has been fully validated (name and extension, size, storage
quota, real MIDI parse). A rejected upload therefore never destroys the file it
was meant to replace. Error codes: `upload_busy`, `unauthorized`, `invalid_name`,
`too_large`, `write_failed`, `storage_full`, `invalid_midi`, `storage_error`.

## Access model

### Authentication

Every route that changes something — configuration, reset, restart, filesystem
recovery, MIDI files, Wi-Fi — and every WebSocket command requires a session
token. Purely informative routes (`/`, `/api/status`, `/api/config` read,
`/api/diagnostics`, `/api/wifi/status`, `/gmb/descriptor.json`,
`/api/auth/status`) stay open: they expose no secret (the Wi-Fi password is
never serialized) and are what a controller or a monitoring page needs.

```
POST /api/auth/login   {"password":"<admin password>"}
      -> 200 {"ok":true,"token":"<32 hex chars>","ttl_ms":3600000}
      -> 401 {"ok":false,"error":"invalid_credentials"}
```

The token is then sent on every protected call, as the `X-Auth-Token` header or
as a `?token=` query parameter. Sessions live in RAM only (at most four, sliding
one-hour expiry, constant-time comparison) and are dropped on reboot.

The WebSocket is authenticated in-band: the server answers a new connection with
`{"t":"auth_required"}` and refuses every command with
`{"t":"error","msg":"unauthorized"}` until the client sends
`{"t":"auth","token":"<token>"}`. The embedded UI does this automatically and
shows a sign-in overlay when a 401 comes back.

### Noise-profile capture

```
{"t":"noise_cal","mode":"start"}   -> {"t":"noise","ok":true,"capturing":"pump_high"}
{"t":"noise_cal","mode":"stop"}    -> {"t":"noise","ok":true,"profile":"pump_high",
                                       "frames":48,"rms_dbfs":-52.3,"flatness":0.85}
{"t":"noise_cal","mode":"reset"}   -> {"t":"noise","ok":true}
```

`flatness` is a *measured* value and the figure above is only an order of
magnitude: a broadband machinery profile reads around 0.85, a profile dominated
by one mechanical line far lower. It was ~0.37 in an earlier firmware — spectral
flatness was then computed over the whole spectrum of a signal whose filter
chain empties 56 % of the bins, so it measured the filter as much as the signal.
It is now measured inside the analysis band, and **a stored profile is only
comparable to another profile captured by the same firmware**.

The flute's machinery is part of the noise and its level depends on the
operating point, so the SNR is measured against a profile of the **current**
state rather than one global floor. Bring the instrument to the wanted state
first (`pump_target`, `fan_target`), then capture: the analyser writes into the
profile matching the state it reads.

A capture is refused with `note_playing` while a note sounds — it would measure
the note, not the noise — and with `no_microphone` when none is detected. A
capture shorter than `MIC_NOISE_MIN_FRAMES` is rejected with `too_short` rather
than stored as a low-confidence profile.

`GET /api/diagnostics` lists every profile under `audio.noise`, says which was
captured, and warns when the current SNR falls back to `ambient` because the
matching profile is missing — that fallback likely overstates quality.

The server stores the **token** for each authenticated socket, not just the
connection id, and revalidates it on every command. An open socket therefore
expires with its session exactly like an HTTP caller (and slides its window the
same way), and a password change — which revokes all sessions — takes effect on
the WebSocket immediately instead of leaving long-lived connections authenticated
under the old secret. A socket whose token has expired gets
`{"t":"error","msg":"unauthorized"}` again and must re-send `{"t":"auth",...}`.

`GET /api/config` and `GET /api/diagnostics` read the whole active configuration
(about 5 KB) from the network task while `loop()` may be committing a new one.
Both take a short lock around the read and answer `503 config_busy` rather than
block the TCP stack; the commit holds the same lock only for the single atomic
assignment, never across validation or the flash write.

### Initial credentials

Both secrets are generated **randomly at first boot** from the ESP32 hardware RNG
and stored in NVS:

- the hotspot WPA2 key (14 characters) — see [Wi-Fi modes](WIFI_MODES.md);
- the web admin password (14 characters).

Both are printed on the serial console at boot. The admin password can be changed
from the UI (`POST /api/auth/password`, minimum 8 characters); a user-chosen
password is stored only as a salted, iterated SHA-256 digest and is therefore no
longer displayable. Changing it revokes every open session.

**Recovery:** holding the BOOT button while the board powers up (5 s) regenerates
both secrets and prints them on the serial console. This is a physical-presence
path, so a headless instrument whose password was lost is never bricked.

### What authentication does and does not cover

Authentication bounds *who* may command the instrument. It does not replace
electrical protection: keep actuator power switchable, and do not expose the
device directly to the Internet. There is no TLS — the token travels in clear on
the local link, so treat the network as the trust boundary it is.

### `hardware_not_ready`

A command that would physically move something (finger servo, airflow servo,
angle servo, solenoid/valve, pump, fan, test note, automatic calibration, range
finder) is refused whenever the hardware did not initialise: missing PCA9685 at
0x40 (or 0x41 when the configuration needs it), unsafe boot configuration, or an
unmountable filesystem.

- REST: `503 {"ok":false,"error":"hardware_not_ready"}`
- WebSocket: `{"t":"error","msg":"hardware_not_ready"}`

The refusal is enforced twice: once in the web layer, so the client gets an
explicit answer, and once in `InstrumentManager::applyCommand()`, which is the
single point where any actuator order is applied. After a PCA failure there is
therefore no path — web, MIDI, calibration or physical button — that can energise
an actuator.

Diagnostics, configuration read and write, reset, filesystem recovery and network
recovery remain available in that state, on purpose: they are what you need to
get out of it.

## Finalisation pass — answers a client must not ignore

Three server replies and one status key existed in the firmware without ever
reaching a client. They are listed here with the decision each one calls for,
because in every case ignoring the reply means believing something that is not
true.

### `{"t":"stop_escalated","cmd":"pump_stop"|"fan_stop"}` (WebSocket)

The stop order could not be handed to the actuator task, so the server
**escalated it to a full emergency stop**. The actuators are safe — but not by
the path that was asked for, and *everything else that was playing was cut with
it*. A client that ignores this message shows a normal "stopped" state while the
instrument has actually panicked.

This message is defence in depth and should be unreachable in practice: stop
orders no longer travel through the command ring at all (see below). It is kept
because the guarantee must still hold if that routing were ever removed.

### Orders that can no longer be lost

`ACMD_PUMP_STOP`, `ACMD_FAN_STOP`, `ACMD_PUMP_STOP_SINGLE`, `pump_enable=false`,
`test_sol=0` — and, since this pass, **`pump_target` and `fan_target` with
`v = 0`** — never travel through the bounded command ring. They set a dedicated
flag that cannot be full, so `postCommand()` always reports success and the order
is applied at the head of the very next pass.

A zero setpoint is *not* converted into a hard stop: it keeps its own channel and
is applied by the original command, because `PressureController::stop()`
additionally cancels a running single-pump test and `FanController::stop()` skips
the ramp-down. The observable behaviour of the sliders is unchanged; only the
loss is gone.

The energising variants (`pump_enable=true`, `test_sol=1`, any `v > 0`) stay
ordinary and may still be refused under saturation: losing a start-up is safe,
losing a stop is not.

### `{"t":"midi_error","error":"storage_error","preserved":true|false}`

Returned by a MIDI upload that replaces an existing file when the swap failed.

- `preserved: true` — **the previous file is still there and intact.** The
  replacement did not happen; nothing was lost. Retry when space allows.
- `preserved: false` — the destination is gone. This is the only case that calls
  for re-uploading the original.

The file is replaced transactionally (`dest → .bak`, `tmp → dest`, `.bak` deleted
only on confirmed success), and an interrupted swap is repaired at boot. The
backup lives at the filesystem root as `/.mbk_<name>`, outside `MIDI_DIR`, so it
never appears in `GET /api/midi/list` and never counts against the quota.

### `GET /api/wifi/status` → `"ssid_busy": true`

The configuration lock was held by a commit in progress, so `ssid` was returned
**empty rather than half-copied**. The field is absent when the read succeeded.
This route is purely informative, which is why it degrades a single field instead
of answering `503 config_busy` the way `GET /api/config` does. An empty `ssid`
without `ssid_busy` genuinely means no SSID is configured — the two are not the
same and a client must not merge them.

### `gmb.descriptor_rebuild_failures` (`/api/status`, `/api/diagnostics`)

Non-zero means **the descriptor being served is behind the active
configuration**: a rebuild ran out of memory and the previous, coherent pair was
kept instead of rebooting the instrument. Playing is unaffected; only GMB
discovery is stale, and the next successful activation clears it.

`/api/diagnostics` also reports it as the `gmb_descriptor` check, `warning` when
non-zero.
