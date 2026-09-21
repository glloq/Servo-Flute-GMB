# Audio and acoustic analysis architecture

This document tracks the microphone / acoustic-analysis chain: what it measures,
how it is structured, what has actually been validated, and what has not.

## Levels of validation

Every claim in this document carries one of these labels. They are not
interchangeable, and a lower level is never presented as a higher one.

| Label | Meaning |
|---|---|
| **unit tested** | Covered by a native test with synthetic input. |
| **firmware compiled** | Builds for ESP32 in CI. Nothing about behaviour. |
| **simulated audio validated** | Exercised end-to-end on synthetic PCM. |
| **hardware microphone validated** | Run against a real INMP441 on the I2S bus. |
| **real flute validated** | Run on the assembled instrument, producing sound. |

As of this writing **nothing in the audio chain is above "simulated audio
validated"**. No INMP441 and no flute have been connected in this environment.

---

## PHASE 0 — Audit of the pre-existing chain

State audited at commit `c39a1b0`, before any change.

### Existing components

```
INMP441 ──I2S DMA──> AudioAnalyzer ──> PitchDetector (YIN) ──> IAudioSource
                                                                    │
                                                                    ▼
                                                            AutoCalibrator
                                                            WebConfigurator
```

| File | Lines | Role |
|---|---|---|
| `AudioAnalyzer.{h,cpp}` | 109 / 235 | I2S driver (IDF 4.x and 5.x), presence detection, RMS, pitch |
| `PitchDetector.{h,cpp}` | 54 / 127 | Hardware-free YIN + raw-signal classification |
| `PitchMath.h` | 63 | Hz ↔ MIDI ↔ cents (header-only, pure) |
| `IAudioSource.h` | 40 | Abstraction consumed by `AutoCalibrator` |
| `AutoCalMath.h` | 215 | Per-note scoring helpers, pure |

### Measured parameters

| Parameter | Value | Note |
|---|---|---|
| Sample rate | 32 kHz | Nyquist 16 kHz |
| Word format | 32-bit I2S, 24-bit left-aligned, mono (left slot) | INMP441 |
| Analysis frame | `MIC_BUFFER_SIZE` = 1024 samples = **32.00 ms** | |
| DMA capacity | 4 × 256 = 1024 samples = **32.00 ms** | `MIC_DMA_BUF_COUNT` × `MIC_DMA_BUF_LEN` |
| Analysis interval | `AUTOCAL_FRAME_SAMPLE_MS` = 40 ms → **25 frames/s max** | |
| Pitch range | 200 Hz – 4000 Hz | `MIC_PITCH_MIN_HZ` / `MIC_PITCH_MAX_HZ` |
| YIN lag range | τ = 8 … 160 | `tauMax` capped by `MIC_YIN_TAU_MAX` = 200 |
| YIN inner loop | 160 × 512 = **81 920 multiply-add per frame** | |

### Static RAM

| Buffer | Size |
|---|---|
| `PitchDetector::_hann[1024]` float | 4 096 B |
| `PitchDetector::_yinBuf[202]` float | 808 B |
| `AudioAnalyzer::_rawBuffer[1024]` int32 | 4 096 B |
| `AudioAnalyzer::_analysisBuffer[1024]` float | 4 096 B |
| **Total** | **13 096 B (12.8 kB)** |

CPU time per frame has **not** been measured on the device. The operation count
above is the only figure available; a real measurement needs hardware.

### Defects found

**A0-1 — A partial I2S read is treated as a complete frame.** *(critical)*

`readI2S()` issues a non-blocking read (timeout 0) and then does:

```cpp
_validSamples = bytesRead / sizeof(int32_t);
```

Whatever the DMA happened to hold becomes "the frame". Consequences:

- RMS is computed over a window of unknown length, so the level depends on DMA
  timing rather than on the sound;
- when fewer than ~322 samples are available, `W = n/2 < tauMax + 1` and YIN
  returns nothing at all — silently;
- `_frameSeq++` and `_frameTimestamp` are updated anyway, so a consumer sees a
  *fresh* frame that carries a degraded or empty measurement. The staleness
  guard added by the previous audit cannot catch this, because the frame is not
  stale — it is fresh and wrong.

**A0-2 — The DMA overflows on every cycle.** *(critical)*

The DMA holds 32 ms; the analysis runs every 40 ms. Each cycle produces 8 ms of
samples that no one reads, and the DMA ring overwrites them. Frames are not just
non-overlapping, they are separated by gaps, and the gap is exactly where a note
onset may fall.

**A0-3 — Pitch accuracy is ±23 cents, not sub-cent.** *(major)*

Measured on synthetic pure tones at 440 Hz:

| Input | Measured | Error |
|---|---|---|
| 429.95 Hz (−40 c) | 433.90 Hz | +15.8 c |
| 434.95 Hz (−20 c) | 440.73 Hz | +22.9 c |
| **440.00 Hz (0 c)** | **435.72 Hz** | **−16.9 c** |
| 442.55 Hz (+10 c) | 445.34 Hz | +10.9 c |
| 450.28 Hz (+40 c) | 450.24 Hz | −0.2 c |

One τ step is 23.6 cents at 440 Hz, so the parabolic interpolation is achieving
essentially nothing.

> **Correction (PHASE 2).** The first diagnosis written here blamed the Hann
> window applied before the YIN difference function. That was **wrong as the
> primary cause**. Measuring both variants showed removing the window alone moved
> the mean error from 20.9 to 19.6 cents — nothing. The dominant cause is that
> **parabolic interpolation over three integer τ values is a poor fit to `d(τ)`,
> which is strongly asymmetric near its minimum**, and the τ grid gets coarse as
> pitch rises (55 cents per step at 1046 Hz). See PHASE 2 for the measurement and
> the fix. The window *is* harmful — it costs a factor of 17 once the real fix is
> in place — but it was not what made the error 47 cents.

The note number stays correct throughout, which is why the auto-calibration
never surfaced this: it only ever compares MIDI note numbers and a ±50 cent
tolerance.

**A0-4 — No clipping detection on the analysis path.** `classifyRaw()` detects
permanent saturation once, at start-up, on a probe buffer. Nothing detects a
transient clip during playing.

**A0-5 — Level is linear RMS only.** No dBFS, no peak, so no usable dynamic
figure and no SNR.

**A0-6 — No overlap.** Consecutive frames share no samples (and, per A0-2, are
not even contiguous), so any time-resolved measurement is quantised to 40 ms.

**A0-7 — No spectral analysis at all.** No harmonics, no HNR, no centroid, no
flatness, so no way to distinguish breath from tone, or an overblow from a
correct octave.

**A0-8 — No expected-note tracking.** `PitchDetector` cannot be told which note
is being attempted, so it cannot check `f0 / 2f0 / 3f0` explicitly.

**A0-9 — Dead configuration.** `MIC_YIN_OCTAVE_RATIO` and
`MIC_PITCH_TOLERANCE_CENTS` are defined in `settings.h` and referenced nowhere.
The octave guard they configured was removed in an earlier pass.

### Reference tests added (PHASE 0)

`tests/test_native/test_audio.cpp`, driven by the reproducible generators in
`tests/test_native/audio_signals.h`:

| Test | What it locks down |
|---|---|
| `ref_pitch_pure_tones_in_range` | Exact MIDI note over the musical range; no octave error up to 3.8 kHz |
| `ref_pitch_degraded_inputs` | Silence, DC-only, white noise, very low level, zero/short buffer, null pointer, out-of-range pitch |
| `ref_pitch_saturated_and_harmonic` | Clipped tone, strong H2, flute-like spectrum → fundamental, never an octave error |
| `ref_pitch_cents_accuracy_current_limit` | **Records A0-3**: worst-case error is between 10 and 25 cents today |
| `ref_pitch_octave_relationships` | C4 / C5 / C6 each detected as themselves |
| `ref_rms_reference_values` | Silence, DC rejection, A/√2 for a sine, zero-length safety |
| `ref_raw_signal_classification` | all-zero / stuck / saturated / ok / zero-length |
| `ref_signal_generator_is_reproducible` | The generators themselves are deterministic and phase-continuous |

`ref_pitch_cents_accuracy_current_limit` deliberately asserts that the error is
**greater than 10 cents**. It is a tripwire: when PHASE 2 improves the estimator,
this test fails and must be tightened, so the improvement cannot go unnoticed or
be silently lost later.

---

## PHASE 1 — Robust acquisition

Fixes A0-1, A0-2, A0-4, A0-5 and A0-6.

### What changed

`AudioAnalyzer::update()` no longer does "read whatever the DMA holds, call it a
frame". It now does two separate things:

1. **Drain** the DMA into a continuous ring buffer, every `MIC_DRAIN_INTERVAL_MS`
   (8 ms — a 4× margin over the 32 ms the DMA can hold). A short read is counted
   as a `partialRead` and is no longer a problem: the samples go into the ring
   and the frame is assembled later.
2. **Analyse** exactly one frame, and only when `MIC_ANALYSIS_FRAME_SIZE`
   samples are genuinely available. A partial frame is never produced, so
   `_frameSeq` can no longer advertise a fresh-but-empty measurement.

### Why there is no analysis timer

There is deliberately **no throttle** on the analysis step. Each analysis
advances the ring by `MIC_ANALYSIS_HOP_SIZE`, so the equilibrium rate is exactly
`hop / Fs` = 16 ms. A timer *slower* than that would overrun the ring forever —
production is fixed by the hardware — and a timer *faster* would do nothing,
because the frame would not be ready. The old 40 ms timer against a 32 ms DMA
was precisely this mistake (A0-2). One frame per `update()` call bounds the time
spent in the loop.

### New components

| File | Role | Testable natively |
|---|---|---|
| `AudioRingBuffer.{h,cpp}` | Continuous ring, frame assembly with hop/overlap, capture counters | yes |
| `AudioLevel.h` | RMS / peak / dBFS / DC offset / clipping, pure functions | yes |

Overlap is 50 % by default (`FRAME` 1024, `HOP` 512), so a measurement lands
every 16 ms instead of every 40 ms.

### Overrun policy

When the ring is full, the **oldest** samples are dropped, never the newest: for
a real-time analyser, audio from 100 ms ago has no value while the present does.
Every loss is counted (`droppedSamples`, `bufferOverruns`) and surfaced in
`/api/diagnostics`, so a saturating ring shows up as a warning instead of
quietly degrading every measurement.

### Level and clipping

`rmsDbFS` / `peakDbFS` are **dBFS** — relative to digital full scale, where an
absolute sample value of 1.0 is 0 dBFS. They are **not dB SPL**, and cannot be:
that would need a calibrated microphone. Any display must say "dBFS".

Clipping distinguishes two things that were previously conflated: the
`clippingRatio` of the current frame, and the boolean `clippingDetected` that
only trips above `MIC_CLIP_RATIO_WARN` (0.5 %). A few clipped samples on a
transient is not a permanently saturated microphone.

### Cost

| | Before | After | Δ |
|---|---|---|---|
| `AudioRingBuffer` | — | 8 208 B | |
| I2S chunk (int32 + float) | 8 192 B (`_rawBuffer` + `_analysisBuffer`) | 2 048 B | |
| Analysis frame | — | 4 096 B | |
| `PitchDetector` | 4 904 B | 4 904 B | |
| `FrameLevel` + stats | — | 68 B | |
| **Total static RAM** | **13 096 B** | **19 324 B** | **+6 228 B (+6.1 kB)** |

CPU: the analysis rate rises from 25 to **62.5 frames/s**. YIN costs 81 920
multiply-add per frame, so **5.12 M MAC/s**, which at a rough 6 cycles/MAC is
**≈ 12.8 % of one 240 MHz core**. This is an *estimate from operation counts*,
not a measurement — no device was available.

Two things bound the impact. The analyser only runs when `setActive(true)`, i.e.
while the mic monitor is on or a calibration is running, not during ordinary MIDI
playback. And `MIC_ANALYSIS_HOP_SIZE` is the single knob: raising it to 768 or
1024 trades overlap for CPU without touching anything else.

### Tests added

| Test | What it proves |
|---|---|
| `ring_never_produces_a_partial_frame` | 1, 100, 300, 1023 samples all refuse to yield a frame; the refusal is counted — **this is A0-1** |
| `ring_frames_overlap_by_hop` | Frame *n*+1 starts at `hop`, the second half of frame *n* is the first half of frame *n*+1; a zero or oversized hop is clamped |
| `ring_overrun_drops_oldest_and_counts` | The oldest samples go first, the count is exact, a write larger than the whole ring keeps only the newest |
| `ring_wraps_without_corrupting_a_frame` | 200 wrap-arounds against a numbered ramp: every frame is byte-exact |
| `ring_steady_state_does_not_drift` | 5 simulated seconds at hardware rate: zero dropped samples, frame count matches `hop` |
| `level_rms_peak_and_dbfs` | 0 dBFS for a full-scale sine, −3.01 dBFS RMS, −6.02 dB per halving, DC measured but excluded, floor instead of −inf |
| `level_clipping_detection` | One clipped sample is counted but does not trip the alarm; a heavily clipped sine does |

---

## PHASE 2 — Pitch detection

Fixes A0-3 and A0-8.

### The real cause of A0-3

The Phase 0 hypothesis (the Hann window) was measured and rejected. The actual
mechanism, on a pure C6 whose true period is 30.578 samples:

```
d(30) = 0.579    d(31) = 0.308    d(32) = 3.470
```

The discrete minimum is τ = 31, but the true minimum is at 30.58. Point 32 is
already far up the steep side and drags the fitted parabola right, giving 31.42
— **−47 cents**. The three points do bracket the minimum, but `d(τ)` is nothing
like a parabola there.

### The fix: fractional-lag refinement

`refineTau()` now runs a ternary search on `d(τ)` evaluated at genuinely
**fractional** lags, with the shifted sample obtained by linear interpolation.
This removes the dependence on the τ grid entirely.

| Variant | Worst error | Mean error |
|---|---|---|
| Parabolic on integer τ, Hann window (PHASE 0) | 47.5 c | 20.9 c |
| Fractional refinement, Hann window kept | 4.4 c | 2.6 c |
| **Fractional refinement, no window (current)** | **0.17 c** | **0.15 c** |

Measured over MIDI 60–84 on pure tones. Both changes matter; the refinement is
the large one, the window removal is the last factor of 17.

Cost: `MIC_YIN_REFINE_ITERATIONS` = 10 → 20 extra evaluations of `d` over a
512-sample window = **+12 % on YIN**. 12 iterations would give 0.27 c for +15 %;
6 would give 2.6 c for +8 %. This is the knob if CPU becomes tight.

### Why the window had to go anyway

YIN belongs to the autocorrelation family: it compares `x[i]` with `x[i+τ]`.
Multiplying by an envelope gives those two samples *different* gains, so

```
d(τ) = Σ (w[i]·x[i] − w[i+τ]·x[i+τ])²
```

no longer measures periodicity alone — it also measures the slope of the
envelope, which grows with τ. Windowing belongs to the spectral path, where it
limits leakage between bins. It has no place in a time-domain estimator.

Removing it also made `analyse()` **non-destructive**: DC removal is a no-op for
a difference (`(x[i]−m) − (x[i+τ]−m) = x[i] − x[i+τ]`), so nothing needs to be
written back. The same frame therefore stays available for spectral analysis in
PHASE 3, with no copy. The 4 kB Hann table disappeared with it.

`detectWindowed()` is kept solely as the A/B reference for
`pitch_window_ab_comparison`, so the improvement is demonstrated rather than
asserted. It is not used in production.

### Expected-note tracking (A0-8)

`setExpectedMidiNote()` lets the detector evaluate the lags of `3·f0`, `2·f0`,
`f0` and `f0/2` explicitly. Candidates are walked **from the shortest lag
upward**, and the first one below threshold wins.

Taking the *deepest* dip instead would be wrong: for any periodic signal `d(2T)`
and `d(3T)` are naturally deep. An A4 overblown to 880 Hz has a very deep dip at
the 440 Hz lag (exactly two periods), and picking it would report a correct A4
while the instrument is sounding an octave high. Shortest-qualifying-lag is the
same principle as the generic path, and correct for the same reason.

`IAudioSource` gained `setExpectedMidiNote` / `clearExpectedMidiNote` with
**default no-op implementations**, so existing implementers (the test fakes) are
unaffected. `AutoCalibrator` declares the target note in `prepareNote()` and
clears it in `safeHardware()`, so the bias never outlives the calibration.

### Enriched result

`PitchResult` now carries `midi`, `cents`, `expectedMatch`, `octaveAbove`,
`octaveBelow` and `stability`. Stability is the spread of the last
`MIC_PITCH_HISTORY` (8 ≈ 128 ms) valid measurements in absolute cents, mapped
through `AutoCalMath::pitchStability`. Invalid measurements never enter the
history — they would make a steady note look unstable.

`MIC_EXPECTED_TOLERANCE_CENTS` was lowered from 50 to 35 cents. At 50 the cents
check was dead code: beyond 50 cents the frequency already rounds to the
neighbouring MIDI note, so the note *number* differs and the cents comparison
never fires. At 35 a note that is right in pitch class but badly out of tune is
distinguishable from a correct one.

### Cost

| | PHASE 0 | PHASE 1 | PHASE 2 |
|---|---|---|---|
| Static RAM | 13 096 B | 19 324 B | **15 300 B** |
| YIN per frame | 81 920 MAC | 81 920 MAC | 92 160 MAC |
| Frames/s | 25 | 62.5 | 62.5 |
| Estimated core load | ~5 % | ~12.8 % | **~14.4 %** |

RAM *fell* by 4 kB versus PHASE 1 because the Hann table is gone; the net cost
over the original baseline is +2.2 kB. The core-load figures remain estimates
from operation counts — **no device was available to measure them**.

### Tests added

| Test | What it proves |
|---|---|
| `pitch_cents_accuracy_is_sub_cent` | < 1 c over ±40 c detuning, < 2 c over MIDI 60–88, correct sign |
| `pitch_window_ab_comparison` | The windowed path is at least 5× worse on mean error, with the refinement held constant |
| `pitch_analysis_is_non_destructive` | Neither `analyse()` nor `detect()` modifies the caller's buffer |
| `pitch_expected_note_resolves_octave` | Match, octave above, octave below, neighbouring note, right note badly out of tune, invalid inputs |
| `pitch_expected_note_helps_on_dominant_h2` | A weak fundamental with a dominant H2 still resolves to the fundamental |
| `pitch_stability_tracking` | 0 until the history fills, ~1 on a held note, collapses on drift, reset works, invalid frames never pollute it |

---

## Target architecture

```
INMP441
   │
   ▼  I2S DMA
AudioCapture ──> AudioRingBuffer ──> frames (FRAME_SIZE, HOP_SIZE overlap)
                                         │
                            ┌────────────┴────────────┐
                            ▼                         ▼
                     PitchDetector              SpectralAnalyzer
                     (YIN, no window)           (Goertzel / FFT + Hann)
                            │                         │
                            └────────────┬────────────┘
                                         ▼
                                 AcousticFeatures
                                         │
                            ┌────────────┼────────────┐
                            ▼            ▼            ▼
                       AutoCalibrator  Diagnostics  Live monitor
```

Ownership rule: **every DSP operation runs on the `loop()` task**, inside
`AudioAnalyzer::update()`. No network, BLE or timer callback ever performs
analysis. This is the same ownership rule the actuator path already follows.

---

## Status by phase

| Phase | Subject | Status |
|---|---|---|
| 0 | Audit + reference tests | **unit tested** |
| 1 | Ring buffer, overlap, clipping, dBFS, I2S diagnostics | **unit tested** |
| 2 | YIN without window, expected-note tracking, richer result | **unit tested** |
| 3 | Goertzel + optional FFT | not started |
| 4 | `AcousticFeatures` | not started |
| 5+ | Noise model, classification, timing, quality, calibration | not started |

## Known limitations

- Nothing has been validated against a real microphone or a real flute.
- CPU time is estimated from operation counts, never measured on the device.
- Pitch accuracy on synthetic pure tones is now < 1 cent (was ±47). This has
  never been checked against a real microphone, where noise, room response and
  the flute's own spectrum all apply.
