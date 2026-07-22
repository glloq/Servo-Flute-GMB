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

## Software-only validation status

Host unit tests (`tests/cpp/test_behavior.cpp`, run via `platformio test -e
native` and pytest) cover Hz→MIDI/cents conversion, neighbour/octave rejection,
exact-note acceptance, adaptive threshold, median filtering, minimum valid
frames, min/max/nominal detection from simulated audio, nominal relationship
validation, keeping old values on failure, the global-timeout safe stop, and the
mic-absent no-op. Physical behaviour remains **NOT TESTED — requires hardware**.

## Prior runtime safety and validation notes

Configuration is validated by the firmware, not only by HTML controls. PCA
channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only
output pins, invalid min/max/nominal relationships, invalid MIDI channels,
invalid fingering values, and unsafe pump/sensor bounds are detected before
saving. Parameters that change `pinMode()`, I2C/PCA routing, controller
`begin()` behavior, sensor setup, or serial MIDI setup require a restart.
Manual hardware tests must always be time-limited and followed by a safe state.
