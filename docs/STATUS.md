# Project Status and Validation

This document centralizes the validation state and known limitations of Servo Flute GMB. It prevents temporary audit notes and repeated validation warnings from overloading the main README and the feature documentation.

## Status convention

- **Implemented:** present in the firmware or web interface.
- **Software tested:** covered by automated host tests, build checks, or static audits.
- **NOT TESTED — requires hardware:** the procedure is defined, but it has not been executed on the corresponding physical components.
- **Known limitation:** intentionally documented behavior that still requires future work.

## Current status

| Area | Implementation | Validation |
|---|---|---|
| ESP32 firmware build | Implemented | Automated build available |
| PlatformIO native behavior tests | Implemented | Software tested |
| Runtime configuration and LittleFS persistence | Implemented | Software tested |
| Central configuration validation | Implemented | Software tested |
| Safe boot and PCA output-enable ordering | Implemented | Software tested; physical power-on test required |
| Finger servo control | Implemented | NOT TESTED — requires hardware |
| Six air-management modes | Implemented | Software behavior tested; physical validation required |
| BLE-MIDI | Implemented | NOT TESTED — requires hardware |
| rtpMIDI / AppleMIDI | Implemented | NOT TESTED — requires hardware |
| Serial MIDI DIN | Implemented | NOT TESTED — requires hardware |
| MIDI file playback | Implemented | Software tested; acoustic validation required |
| Embedded web interface | Implemented | Software tested |
| INMP441 acoustic analysis (8 phases) | Implemented | **Simulated audio validated** — no microphone, no flute; see below |
| Per-note automatic airflow calibration | Implemented | Software tested; flute validation required |
| Pumps, reservoir, fan, and sensors | Implemented | NOT TESTED — requires hardware |
| General-Midi-Boop v2 recognition (blocks 1 / 0x10 / 0x11) | Implemented | Software tested; recognition by a real controller over BLE / rtpMIDI requires hardware |
| GMB announced acoustic latency (`timing.excite.latency_ms`) | Deliberately not announced | Known limitation — requires an acoustic measurement on a real flute |
| REST/WebSocket authentication | Implemented | Software tested; end-to-end check on hardware required |
| Cross-task command queue (AsyncTCP / NimBLE → `loop()`) | Implemented | Software tested |
| Transactional configuration commit | Implemented | Software tested |
| `hardware_not_ready` actuator lockout | Implemented | Software tested; PCA-absent bench check required |
| Fail-safe LittleFS mount (no automatic format) | Implemented | Software tested; corrupted-partition check required |
| Randomly generated hotspot key and admin password (NVS) | Implemented | Software tested; first-boot check on hardware required |
| VL53L0X full initialisation | Implemented | Software tested against a simulated device; **real sensor required** |
| Actuator-session ownership during calibration | Implemented | Software tested end-to-end (InstrumentManager + web session + AutoCalibrator); **real flute required** |
| Undroppable Note Off and calibration cancel | Implemented | Software tested |
| Air-source demand following the real breath state (CC2) | Implemented | Software tested; pump/fan bench check required |
| WebSocket session expiry and revocation | Implemented | Software tested; end-to-end check on hardware required |
| Configuration reads serialised with the commit | Implemented | Software tested |
| Actuator GPIOs driven inactive before the I2C probe | Implemented | Software tested — **a hardware gate pull-down remains the reference protection** |
| Undroppable actuator stop orders and zero setpoints | Implemented | Software tested; saturation bench check required (`FIN-STOP-*`) |
| Transactional MIDI file replacement | Implemented | Software tested; flash-failure bench check required (`FIN-MIDI-1`) |
| Bounded MIDI file playback and UART drain | Implemented | Software tested; dense-burst bench check required (`FIN-MIDI-2`) |

## Safety and reliability work completed

The 2026-09 concurrency, actuator-safety and security audit added:

- an atomic `EventQueue` consumption API (`tryPopDueEvent` / `peekCopy`): reading
  an event and removing it happen under one lock, and no pointer into the queue
  is ever handed out, so a concurrent `clear()`, a forced Note Off on a full
  queue or a panic can no longer make the sequencer play a stale event;
- a cross-task `CommandQueue`: AsyncTCP and NimBLE callbacks post commands and
  `loop()` applies them, so no web or Bluetooth callback performs an I2C/PCA
  transaction, drives an actuator GPIO, or mutates the active configuration;
- an undroppable panic flag that also discards commands issued before it;
- a transactional configuration commit (candidate → validate → save → single
  atomic activation) with no intermediate state visible to the controllers;
- a central `hardware_not_ready` refusal applied at the single point where any
  actuator command is executed, plus an explicit REST/WebSocket answer;
- a fail-safe LittleFS mount: no automatic format, a recovery state with
  actuators disabled, and formatting only on an explicit confirmed request;
- a signed read of the vibrato sine table (the oscillation was unipolar);
- MIDI channel-mode messages (CC 120-127) exempted from the rate limiter and
  CC2 coalescing so a breath burst can never strand a blown note;
- an exclusive MIDI upload slot that validates a transfer completely before it
  replaces an existing file;
- a real VL53L0X initialisation sequence with distinct present / initialised /
  valid / stale / fault states;
- centralised Wi-Fi transitions that panic before tearing a session down;
- a randomly generated, NVS-stored hotspot key and web admin password, and
  session-token authentication for every mutating route and the WebSocket.

The second 2026-09 pass, run on the post-audit firmware, added:

- an **idempotent** actuator session: taking ownership again mid-calibration no
  longer stops the sequencer, so the valve the calibrator opened and the airflow
  angle it set survive the whole measurement window. Ownership is now taken once
  at the start of a calibration and released once at the end. This was a P0: the
  automatic calibration measured a silent instrument and every note failed;
- air-source ownership during a session: the sequencer no longer overwrites the
  demand set by `CalibrationAirSupply`;
- an undroppable calibration cancel (a dedicated flag, like the panic) so a
  panic can never leave the calibrator running;
- undroppable Note Off: a 128-note bitmap outside the command ring, so a
  saturated queue can no longer strand a held note with the valve open;
- an air-source demand that follows the real breath state: a CC2 silence on a
  held note drops the pump/fan to idle instead of pushing against a closed valve;
- WebSocket sessions that store the token and revalidate it on every command, so
  an open socket expires with its session and a password change revokes it;
- an unconditional `Serial.begin()`, because the serial console is the only
  channel that carries the hotspot key and the admin password;
- configuration reads serialised with the atomic commit, with the lock held only
  for the assignment itself;
- a FreeRTOS mutex instead of a spinlock on the WebOp queue (copying a `WebOp`
  allocates, which a critical section forbids);
- symmetric vibrato rounding, a full expression reset on CC121, and actuator
  GPIOs driven to their inactive level before the I2C probe.

The earlier 2026 firmware audit introduced or reinforced:

- actuator outputs disabled before configuration is loaded and validated;
- inert behavior when required PCA9685 hardware cannot be initialized safely;
- centralized validation of GPIO capability, reserved pins, PCA channel conflicts, MIDI limits, fingering values, and sensor ranges;
- atomic configuration persistence: the candidate is written to `.tmp` and
  re-parsed, then the live file is **renamed** to `.bak` — never deleted — before
  the `.tmp` is promoted, and the `.bak` is restored if that promotion fails. At
  every instant at least one of the three paths holds a complete configuration.
  The boot path recovers from either companion. The previous sequence deleted the
  live file before renaming, so a failed rename destroyed both copies;
- controlled restart for hardware-routing changes;
- firmware-side time limits for manual actuator tests;
- actuator-session ownership for calibration and manual tests;
- panic and disconnect paths returning hardware to a safe state;
- non-blocking ToF sensor reads and stale-sensor pump shutdown;
- pump and fan demand tied to accepted sequencer note transitions;
- bounded calibration timeouts and preservation of previous values when a note fails calibration;
- **reset and factory reset no longer touch the active configuration.** It
  describes the hardware that was actually initialised; overwriting it in RAM
  while the loop runs would drive the actuators from a description that no
  longer matches the wiring. Factory reset was the worse of the two: it
  destroyed the active configuration without persisting anything;
- **the configuration commit refuses to activate without its lock.** It used to
  write the active configuration anyway and merely add a `config_lock_timeout`
  warning — visible, but not prevented, which is the opposite of what a lock
  does. A refused lock now leaves the active configuration untouched, reports
  `saved && !activated`, and schedules the controlled reboot that reconciles RAM
  and flash;
- **the auto-calibrator goes through that same transactional commit** instead of
  writing the active configuration field by field: candidate, validate, persist,
  activate. A calibration that fails at any step leaves it bit-for-bit unchanged;
- **cross-task requests are taken and cleared atomically**, and the actuator work
  done per loop pass is bounded so a burst of web commands cannot starve MIDI;
- **allocation failures reach the degraded paths that were already written for
  them.** Several `new` calls were followed by careful null handling that could
  never run, because a plain `new` on this platform aborts instead of returning
  null. The handling is unchanged; it is now reachable.

The 2026-09 finalisation pass, run on the merged hardening firmware, added:

- **an actuator stop order that cannot be lost.** This was the P0 of the pass:
  a stop travelled through the bounded command ring, `push()` returned `false`
  when that ring was full, the web layer ignored the value, and
  `endTestSession()` then disarmed the `TEST_SESSION_MAX_MS` net — a pump could
  stay powered at its setpoint with *no time limit*. Stop orders now leave the
  ring entirely, and so does a `pump_target`/`fan_target` of 0, which is how the
  interface sliders actually ask for a shutdown;
- **correct Note On / Note Off ordering.** Every Note Off used to go to the
  undroppable bitmap, applied after the ring, so a `NOTE_OFF 60` followed by a
  `NOTE_ON 60` between two passes applied backwards and left the note silent.
  The bitmap is now a fallback on refusal only — which is what the comment
  justifying the ordering had always assumed;
- **a save reported as failed no longer applies itself at the next boot.** Boot
  recovery promoted the `.tmp` before the `.bak`, but `bak + tmp, no live` is
  exactly what a *failed* save leaves behind. The user got an error and the
  reboot applied the rejected configuration anyway — possibly describing wiring
  that is not the wiring installed;
- **a factory reset that cannot report failure after destroying the
  configuration**: residues first, abort on the first refusal with the live file
  intact, live file last, and success confirmed by `exists()` rather than by
  what `remove()` claims;
- **one list of the fields that require a restart**, walked field by field
  (`ConfigTopology`). The hand-written list it replaces was short by seven
  fields, including the three passed to `pinMode()` for the endstop and Hall
  inputs;
- **web-layer concurrency discipline**: `volatile` removed, the AsyncTCP↔`loop()`
  hand-off turned into an explicit state machine where abandonment and
  publication are decided in the same critical section, the configuration
  candidate taken from a locked snapshot instead of copied live, and two
  read-after-release/wrong-target defects found in the process;
- **transactional MIDI file replacement**, with boot repair of an interrupted
  install and the backup kept outside the listing and the quota;
- **bounded work** in the MIDI file player and the serial-MIDI UART drain, the
  last two unbounded loops in the firmware;
- **an unbuildable GMB descriptor no longer reboots the instrument**, and the
  counter that says the served descriptor is stale is now visible in
  `/api/status` and `/api/diagnostics`;
- **three WebSocket replies the browser was discarding in silence** —
  `stop_escalated`, `noise` and `mic_reset` — plus a CI guard that compares the
  replies the firmware emits with the ones the interface handles, in both
  directions.

The defect-by-defect account — how each one was reproduced before being
touched, which mutation proves each fix, and what this pass does *not* prove —
is in [Hardening report](HARDENING_REPORT.md), which now carries one table per
pass.

Two rules are now enforced by CI rather than by attention: no test function may
be defined without being reachable from `main()` and without asserting anything
(two tests had been dead for weeks), and no line of the hardware matrix may
leave `NOT TESTED — requires hardware` without recording when, and on which
firmware, the test was actually run.

These software protections do not replace electrical protection, a physical emergency stop, appropriate fusing, correct power sizing, or physical verification.

## Before connecting real hardware

A four-axis hardware audit found defects that could destroy actuators, and three
risks that **cannot be fixed in software** because they live in the window
between reset and the firmware's first instruction. The staged power-on
procedure, with a cut-off criterion at each step, is in
[Bring-up procedure](BRINGUP.md). Do not skip step 0: it is the measurement that
decides whether the coil and the servos can be connected at all.

## Known General-Midi-Boop limitation

`timing.excite.latency_ms` is the delay between the MIDI order and the note
being **audible**. General-Midi-Boop uses it to line several instruments up on
the same beat, so a figure the firmware has not measured is worse than no figure
at all — and the GMB rule is that an absent field means unknown.

The field is **not announced**, and in particular is never announced as `0`,
which GMB would read as "this flute speaks instantly".
`solenoidActivationTimeMs` is not that figure either: it is the full-power drive
window of the solenoid coil before the PWM drops to its holding level, an
electrical parameter of the valve.

**What blocks it has changed — the mechanism now exists.** This page used to say
that nothing in the firmware measured the acoustic onset. That stopped being
true with PHASE 7: `Servo_flute_ESP32/AcousticTiming.{h,cpp}` measures exactly
this quantity — `commandToSoundLatency`, from the accepted MIDI order to audible
sound — fed on the signal side by `AudioAnalyzer` and on the order side by hooks
in `NoteSequencer` and `AirflowController`, each measure carrying its own
validity flag.

Three things still stand between that and an announcement, and none of them is a
missing mechanism:

1. **It only runs while the analysis is active** — the microphone monitor or a
   calibration. In ordinary MIDI playback the analyser is idle and no frame
   arrives; the timing machine now *refuses* to open a cycle in that case,
   precisely so it cannot manufacture a verdict.
2. **Nothing has been validated on a real microphone or a real flute.** Every
   figure in the acoustic chain comes from synthetic PCM.
3. **The measurement carries a bias that is not constant.** Instants are dated
   at the end of a 32 ms analysis window, and the onset bias depends on the
   margin between the note's level and the applied threshold — so it does not
   cancel in a difference. See PHASE 7 in
   [`AUDIO_ARCHITECTURE.md`](../Servo_flute_ESP32/docs/AUDIO_ARCHITECTURE.md)
   for the measured values.

Announcing a figure the firmware has measured but never verified would be worse
than announcing none, which is the rule this whole section rests on.

The seam is unchanged and still ready: `gmb::measuredExciteLatencyMs()`
(`Servo_flute_ESP32/gmb/Capabilities.h`) is the single point the value goes
through, and the descriptor, the capability signature and the block 0x11
notification already carry it end to end, so the announcement and the revision
bump follow on their own once a real measurement exists.

## Network access model

Every mutating REST route and every WebSocket command requires a session token.
The admin password and the hotspot WPA2 key are generated randomly at first boot,
stored in NVS, and printed on the serial console; holding BOOT while the board
powers up regenerates both.

Remaining operational precautions (authentication does not replace them):

- there is no TLS, so the token travels in clear on the local link — treat the
  network as the trust boundary;
- do not expose the ESP32 directly to the Internet;
- disconnect actuator power when the instrument is unattended.

Full details: [API access model](API_WEB.md#access-model).

## Physical validation

The detailed procedures and expected results are maintained in:

- [`Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`](../Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md)
- [`Servo_flute_ESP32/docs/AUDIT_CODE.md`](../Servo_flute_ESP32/docs/AUDIT_CODE.md)

A hardware row must remain **NOT TESTED — requires hardware** until the test is physically executed and its actual result is recorded.

## Recommended validation order

1. Verify power rails, common ground, fuse, emergency disconnect, and PCA9685 OE behavior without servos connected.
2. Validate one finger servo with current limiting or a conservative supply.
3. Validate all fingers at low speed and check mechanical collisions.
4. Validate the selected valve and airflow servo.
5. Test panic, browser disconnect, and manual-test timeout.
6. Test BLE, Wi-Fi, serial MIDI, and local MIDI playback separately, including
   several AP ↔ STA cycles while a note is held.
7. Add the INMP441 and validate microphone detection and placement.
8. Run per-note airflow calibration on a real instrument.
9. Validate fan or direct-pump modes.
10. Validate reservoir sensors and pump shutdown on sensor loss, including a real
    VL53L0X (the full initialisation sequence has never addressed a physical
    sensor) and a foreign device answering at 0x29.
11. Run the `AUD-` rows of the hardware test matrix: actuator lockout with PCA
    absent, corrupted filesystem, restart-required configuration change, safety
    CCs under load, vibrato symmetry, concurrent uploads, and web authentication.

## Documentation maintenance rule

Feature documents should explain behavior and configuration. Audit history, temporary validation notes, and global hardware status should be maintained here or in the hardware test matrix rather than duplicated across every document.
