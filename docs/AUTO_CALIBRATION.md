# Auto-Calibration (INMP441 microphone)

Auto-calibration uses an optional INMP441 I2S MEMS microphone to listen to the
instrument while the firmware sweeps the airflow servo, and to determine, for
each note, the usable airflow range and a recommended nominal airflow.

> **Status:** the software pipeline (audio analysis, state machine, scoring,
> persistence, WebSocket reporting) is covered by host unit tests. Everything
> that touches a real microphone, servo or flute is **NOT TESTED — requires
> hardware** until validated on an actual INMP441 + instrument. See
> `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.

## 1. Hardware and wiring

INMP441 is a 24-bit I2S MEMS microphone. Default pins (see `settings.h`):

| INMP441 | ESP32 GPIO | Meaning |
| --- | --- | --- |
| SCK / BCLK | `MIC_PIN_BCLK` = 14 | Bit clock |
| WS / LRCL | `MIC_PIN_LRCLK` = 15 | Word select |
| SD / DOUT | `MIC_PIN_DIN` = 32 | Serial data |
| L/R | GND | Selects the **left** slot (`I2S_STD_SLOT_LEFT`) |
| VDD | 3V3 | Do **not** use 5 V |
| GND | GND | Common ground |

Set `MIC_ENABLED false` in `settings.h` if no microphone is fitted; the whole
audio/calibration code is then compiled out.

### Microphone placement (physical, NOT TESTED — requires hardware)

- Position the microphone **5–15 cm** from the sounding end / embouchure.
- **Do not** place it directly in the air jet: wind noise saturates the capsule
  and destroys pitch detection. Offset it to the side of the jet.
- Use a **foam wind screen** over the capsule to reject air blast and turbulence.
- Keep it away from pumps, fans and servos where practical — their mechanical
  noise raises the measured noise floor and shrinks the usable dynamic range.

## 2. Audio acquisition

- I2S standard driver (ESP-IDF 5.x), 32-bit words, mono left slot.
- `MIC_SAMPLE_RATE` = **32 kHz** (Nyquist 16 kHz) with `MIC_BUFFER_SIZE` = 1024
  samples per analysis frame (~32 ms). DMA is 4 × 256 frames.
- Each frame is processed once per `AUTOCAL_FRAME_SAMPLE_MS`, with **no dynamic
  allocation** in the analysis path (all buffers are fixed members).
- **DC-offset removal:** the capsule bias is subtracted (`sample - mean`) before
  the RMS and pitch computations.
- **Hann window:** applied before pitch detection to limit edge/leakage errors.

### RMS is relative, not SPL

The reported `rms` is the root-mean-square of the **normalized** samples
(−1..+1), not a calibrated sound-pressure level (dB SPL). It depends on gain,
distance and placement, so thresholds are always **relative** to a per-note
measured noise floor (below), never an absolute SPL.

## 3. Pitch detection (YIN)

The analyzer runs a simplified YIN estimator on the windowed, DC-free frame:

- lag bounds derived from `MIC_PITCH_MIN_HZ`/`MIC_PITCH_MAX_HZ` and clamped to
  `MIC_YIN_TAU_MAX` (the lag buffer is a fixed member, never a large stack array);
- first dip below `MIC_YIN_THRESHOLD`, then descent to the local minimum;
- a **conservative octave guard** that only corrects a half-period artifact when
  the double-lag dip is much deeper (`MIC_YIN_OCTAVE_RATIO`), so harmonic-rich
  tones are not pushed an octave down;
- **parabolic interpolation** for sub-sample frequency accuracy;
- a **confidence** value `1 − aperiodicity`; the pitch is marked *valid* only when
  confidence ≥ `MIC_YIN_CONFIDENCE_MIN` and the frequency is in range.

Outputs exposed to the calibrator/UI: `pitchHz`, `pitchMidi`, `pitchCents`,
`pitchConfidence`, `pitchValid`.

> Frequency precision for the top octave (≈1–2 kHz) is limited by the 32 kHz lag
> resolution even with interpolation. That is why calibration relies on **exact
> MIDI-note matching** plus a cents tolerance rather than on absolute Hz.

## 4. Per-note calibration flow

For each configured note the calibrator runs a non-blocking sequence:

```
PREPARE → NOISE → COARSE → FINE_MIN → FINE_MAX → NOMINAL → FINALIZE
```

Each airflow position is evaluated with an explicit, transient-safe sub-sequence:

```
SET_AIRFLOW → WAIT_AIRFLOW_SETTLE → COLLECT_AUDIO → EVALUATE_AUDIO
```

1. **PREPARE** — position the fingers for the note; close the valve; airflow at
   rest.
2. **NOISE** — after the finger servos settle, measure the background noise RMS
   for `AUTOCAL_NOISE_MEASURE_MS` (valve closed, air at rest). The adaptive gate
   is `soundThreshold = max(MIC_RMS_ABSOLUTE_MIN, noiseRms × AUTOCAL_NOISE_RATIO)`.
   This is done **per note** so pump/fan/servo noise is accounted for.
3. **COARSE** — sweep airflow 0→100 % in `AUTOCAL_COARSE_STEP_PERCENT` steps to
   bracket where the correct note appears and where it decays / overblows.
4. **FINE_MIN / FINE_MAX** — refine `airMin` and `airMax` in
   `AUTOCAL_FINE_STEP_PERCENT` steps. A loss of validity must be confirmed over
   `AUTOCAL_LOSS_CONFIRM_STEPS` consecutive positions (or a clear overblow) to
   avoid one-off glitches.
5. **NOMINAL** — score several candidate points across `[airMin, airMax]` and pick
   the recommended nominal airflow.
6. **FINALIZE** — store `min / nominal / max`, confidence, cents, stability, SNR;
   valve closed and air at rest between notes.

### What makes a position valid

A position is accepted only when at least `AUTOCAL_REQUIRED_VALID_FRAMES` of
`AUTOCAL_AUDIO_FRAMES_PER_STEP` frames all satisfy:

- pitch is *valid* (YIN confidence high enough),
- RMS above the adaptive `soundThreshold`,
- detected MIDI note **exactly equals** the expected note,
- cents error within tolerance.

Exact MIDI matching automatically **rejects neighbour notes and the octave
above**. Two cents tolerances are used:

- `AUTOCAL_PITCH_TOLERANCE_ONSET_CENTS` (wide) — only to detect the note's first
  appearance;
- `AUTOCAL_PITCH_TOLERANCE_STABLE_CENTS` (strict) — for the stable plateau and the
  nominal value.

An **octave above** appearing after a valid plateau is treated as an **overblow**
and marks the upper airflow limit.

Per-position metrics use the **median** frequency and median cents of the valid
frames, the mean RMS, and the pitch spread (stability).

## 5. Nominal airflow and confidence score

- Default nominal = `airMin + AUTOCAL_NOMINAL_FRACTION × (airMax − airMin)`.
- The calibrator then selects the best-scoring candidate in the stable band. The
  score combines low absolute cents error, pitch stability, SNR over the noise
  floor, YIN confidence, and a margin from the min/max limits. The result is
  clamped so that `0 ≤ min ≤ nominal ≤ max ≤ 100`.
- **Confidence (0–100)** blends the same factors and is reported per note.
- **SNR** is `20·log10(signalRms / noiseRms)` in dB.
- **Stability** is `0..1` (1 = no pitch wobble across frames).

## 6. Safety, stop and timeout

Calibration is an **active actuator test**. It stops immediately and returns the
hardware to a safe state (valve closed, airflow at rest, fingers closed) on:

- user stop, the controlling WebSocket client disconnecting,
- the global firmware timeout `AUTOCAL_GLOBAL_TIMEOUT_MS`,
- panic / restart, or replacement by another manual test,
- a missing microphone (calibration refuses to start).

On a global-timeout abort the firmware publishes an `acal_error` WebSocket
message. Browser-side timers are **not** relied upon as a safety mechanism.

A failed note (no stable pitch found) is reported as `ok:false` and **does not
overwrite** the note's previous calibration.

## 7. Range finder

A separate range-finder mode sweeps the airflow servo 0–180° on a middle note to
discover the usable servo travel (`servoAirflowMin` / `servoAirflowMax`). It uses
the same strict pitch validity (exact MIDI note + confidence).

## 8. WebSocket reporting

During calibration the firmware publishes `acal_prog` frames (note index/name,
phase, airflow %, rms, noise, hz, midi, cents, confidence, valid/total frames)
and, at the end, an `acal_done` message with per-note `min / nominal / max`,
confidence, cents, stability and SNR. See `docs/API_WEB.md`.

## 9. Constants (see `settings.h`)

`MIC_SAMPLE_RATE`, `MIC_RMS_ABSOLUTE_MIN`, `MIC_YIN_CONFIDENCE_MIN`,
`MIC_YIN_OCTAVE_RATIO`, `AUTOCAL_NOISE_RATIO`, `AUTOCAL_NOISE_MEASURE_MS`,
`AUTOCAL_PITCH_TOLERANCE_ONSET_CENTS`, `AUTOCAL_PITCH_TOLERANCE_STABLE_CENTS`,
`AUTOCAL_AIR_SETTLE_MS`, `AUTOCAL_AUDIO_FRAMES_PER_STEP`,
`AUTOCAL_REQUIRED_VALID_FRAMES`, `AUTOCAL_COARSE_STEP_PERCENT`,
`AUTOCAL_FINE_STEP_PERCENT`, `AUTOCAL_LOSS_CONFIRM_STEPS`,
`AUTOCAL_NOMINAL_FRACTION`, `AUTOCAL_GLOBAL_TIMEOUT_MS`.

## 10. Test procedure (physical — NOT TESTED — requires hardware)

1. Wire the INMP441 as above; boot; confirm the microphone is *detected* in
   diagnostics.
2. Mechanically set up the instrument so notes can sound.
3. Position the mic 5–15 cm off-axis with a foam wind screen.
4. Run the range finder, apply if reasonable.
5. Run per-note auto-calibration; watch live RMS/noise/pitch/confidence.
6. Verify min/nominal/max look musically plausible and that failed notes keep
   their previous values.
7. Close the browser mid-run and confirm the instrument returns to a safe state.

Until executed on real hardware these steps remain **NOT TESTED — requires
hardware**.

## 2026 reliability update (what changed)

- **The calibrated nominal is really used when playing.** `AirflowController::
  setAirflowForNote()` maps the airflow source (velocity or CC2) through a
  two-segment curve pivoting on `airflowNominalPercent`: median input
  (`AIRFLOW_SOURCE_PIVOT` = 64) → nominal, below → min→nominal, above →
  nominal→max. `airVelocityResponse` = 0 holds the nominal. CC2 timeout, CC7
  volume, CC11 expression, attack and vibrato are preserved.
- **Fresh, distinct audio frames.** `IAudioSource` exposes `getFrameSequence()` /
  `getFrameTimestamp()`. The analyzer advances the sequence once per analysed
  I2S frame and invalidates the pitch after `MIC_FRAME_STALE_MS` with no new
  data. The calibrator stores only distinct-sequence frames and fails a note
  with `ACAL_FAIL_AUDIO_STALE` if a position gets no fresh frames within
  `AUTOCAL_AUDIO_FRAME_TIMEOUT_MS` (frozen/disconnected source).
- **Note-count-scaled timeouts.** Global timeout = `numNotes ×
  AUTOCAL_TIMEOUT_PER_NOTE_MS + AUTOCAL_GLOBAL_MARGIN_MS`, capped by the
  absolute `AUTOCAL_GLOBAL_TIMEOUT_MS`. A per-note timeout fails only that note
  (its old calibration is kept) and continues.
- **A real stable zone is required.** Wide tolerance is used only for the first
  coarse appearance; the plateau, fine min/max and nominal candidates use the
  strict tolerance. A note is valid only if a nominal candidate was
  strict-validated and the final confidence ≥ `AUTOCAL_MIN_RESULT_CONFIDENCE`.
- **Detailed failure reasons** (`AutoCalFailureReason`) plus per-note
  diagnostics (last note, RMS, valid frames, duration) shown in the UI.
- **Robust apply.** `applyResults()` returns `{applied, saved, validCount,
  failedCount}`; on a LittleFS failure it restores the previous config in RAM
  and never reports success. `acal_done` carries `ok/saved/validCount/
  failedCount`; `ok:false` when every note failed.
- **Actuator ownership + safety.** A calibration is owned by the WS client that
  started it; only the owner may stop/apply; a second start is refused; the
  owner's disconnect stops it and safes the hardware while a non-owner's does
  not disrupt it. `panic` always cancels calibration first. Concurrent
  actuator commands are refused during calibration and `POST /api/config`
  returns `409 calibration_active`. The mic-monitor preference is restored
  afterwards.
- **Range finder** reuses the same SET→SETTLE→COLLECT→EVALUATE engine (noise,
  multi-frame, exact note, confirmed loss) over a configurable safe angle
  window (`AUTOCAL_RF_MIN_SAFE_ANGLE`..`MAX` by `AUTOCAL_RF_STEP_DEG`) — never a
  blind 0–180° sweep.
- **Air-supply aware in every mode.** The calibrator drives the air source
  through `ICalibrationAirSupply` (`prepare` / `isReady` / `setDemandPercent` /
  `stopSafe` / `getError`). The concrete implementation delegates to the same
  `PressureController` / `FanController` calls used during play, so there is no
  duplicated air logic. Before anything is measured the pump / reservoir / fan is
  brought to a representative running state; the run waits for readiness
  (bounded by `AUTOCAL_AIR_READY_TIMEOUT_MS`) and, if the source never comes up,
  every note fails with `ACAL_FAIL_AIR_SUPPLY` and nothing is written. The noise
  floor is thus measured with the pump/fan actually running (valve closed), and a
  mid-run source drop-out is reported per note instead of as a false "no sound".
  Passive modes (solenoid, servo-valve, servo-only) report ready immediately.
- **Portable I2S.** The microphone driver compiles on both Arduino-ESP32 2.0.x
  (ESP-IDF 4.4, legacy `driver/i2s.h`) and 3.0.x (IDF 5.x `driver/i2s_std.h`),
  selected by `ESP_IDF_VERSION`.
- **Robust mic detection** (`detected` / `all_zero` / `stuck` / `saturated` /
  `read_error`) via signal classification, plus a `mic_reset` that re-probes the
  mic without rebooting.

### High-pitch precision caveat

The YIN core is unit-tested on synthetic PCM. At the fixed 32 kHz sample rate the
lag (`tau`) resolution is coarse for the top of the range: above ~2.5 kHz the
detected pitch can be off by up to ~1 semitone. Neighbour/octave errors do not
occur, but the very highest notes may be harder to calibrate by microphone. This
is a sample-rate limitation, not a bug.

## Validation scope (software vs hardware)

**Validated by software tests** (native `platformio test -e native` + pytest):
YIN pitch core on synthetic PCM (sine, +DC, +noise, harmonics, clipped,
broadband-noise rejection, silence, near-limit highs); Hz→MIDI/cents; neighbour
and octave rejection; adaptive noise threshold; median; minimum valid frames;
five distinct frames; frozen-source rejection; coarse/fine min-max-nominal from
simulated audio; nominal relationship validation; strict-nominal requirement;
+70-cent rejection with no config write; per-note timeout; 14-note run without
timeout; global-timeout safe stop; keep-old-values on failure; storage-failure
RAM restore; mic-signal classification; range-finder min/max; nominal driving
the produced airflow angle; ownership/lock tokens (static audit).

**Requires an ESP32** (not host-testable): the firmware build itself
(espressif32@6.10.0 and 6.11.0), the dual I2S driver install/read paths, the
WebSocket/REST server behaviour, LittleFS persistence.

**Requires an INMP441 microphone**: real pitch/RMS/SNR measurement, the
noise-floor capture, mic presence/stuck/saturated classification on real data,
`mic_reset` recovery.

**Requires the real instrument** (mic + flute + air system): that notes actually
sound, that min/nominal/max are musically correct, overblow limits, the two
clients / owner-disconnect flows on hardware, and every air mode (solenoid,
servo-valve, servo-only, fan, pump, reservoir) under calibration.

All physical procedures remain **NOT TESTED — requires hardware** until executed
on a real INMP441 and flute; see `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.

## Prior runtime safety and validation notes

Configuration is validated by the firmware, not only by HTML controls. PCA
channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only
output pins, invalid min/max/nominal relationships, invalid MIDI channels,
invalid fingering values, and unsafe pump/sensor bounds are detected before
saving. Parameters that change `pinMode()`, I2C/PCA routing, controller
`begin()` behavior, sensor setup, or serial MIDI setup require a restart.
Manual hardware tests must always be time-limited and followed by a safe state.
