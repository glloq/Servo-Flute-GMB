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

### RAM — and it is heap, not static

**Correction (audit, PHASE 7).** Every "RAM" figure in this document, in this
section and in the per-phase tables below, is `sizeof(AudioAnalyzer)` — and
`AudioAnalyzer` is allocated with `new` in `WebConfigurator::begin()`. It lives
on the **heap**, not in `.bss`. The linker's static-RAM figure is a different
number and did not move at all across PHASES 0–7: **64 768 B (19.8 %)** before
and after, measured in CI on `espressif32@6.10.0`.

The distinction is not pedantic. On an ESP32 the heap is shared with the WiFi
stack and AsyncTCP buffers, and it is the heap that actually runs out; calling
this "static RAM" makes it look accounted for at link time when it is not.
Flash, which *is* static, went from 1 690 373 B (80.6 %) to **1 702 009 B
(81.2 %)** over PHASES 6–7, i.e. **+11 636 B**, leaving 395 kB of headroom.

| Buffer (heap, inside `AudioAnalyzer`) | Size |
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
| **Total (heap object)** | **13 096 B** | **19 324 B** | **+6 228 B (+6.1 kB)** |

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
| Heap object | 13 096 B | 19 324 B | **15 300 B** |
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

## PHASE 3 — Spectral analysis

Fixes A0-7. New file: `SpectralAnalyzer.{h,cpp}` — hardware-free, natively
tested.

### Two paths, two jobs

**Goertzel** answers "how much energy is there at *exactly* this frequency?".
When the expected note is known, that is all that is needed for f0, 2f0, 3f0,
4f0. It costs *n* operations per target — about **4 % of YIN** for four
harmonics — and is exact at the requested frequency, with no dependence on bin
resolution. It runs on **every** frame.

**FFT** answers "what does the whole spectrum look like?" — centroid, flatness,
band energy. It costs far more and the timbre moves far more slowly than the
pitch, so it runs on one frame in `MIC_SPECTRAL_DECIMATION` (4 → every 64 ms).

### Windowing, again

Here the Hann window **is** legitimate and necessary: it limits leakage between
bins for a signal that is not periodic over the window. This is exactly the
distinction PHASE 0 got wrong — windowing belongs to the frequency domain, not
to a time-domain period estimator.

Goertzel is applied **without** a window: it measures power at a known
frequency, not a spectrum, and a window would only attenuate the result by an
alignment-dependent factor.

### Why not ESP-DSP

ESP-DSP would give a faster FFT but would make this class uncompilable on the
host, so untestable. A 512-point radix-2 FFT is ~4 600 butterflies, modest next
to YIN's 92 160 operations. If a measurement on hardware ever shows the FFT
dominating, it can be swapped in behind this same interface without touching
callers or tests.

Twiddle factors use the stable Numerical Recipes recurrence: **2 trig calls per
stage** (18 total for 512 points) instead of the naive 2 304.

`MIC_FFT_ENABLED` compiles the FFT out entirely, saving ~6 kB; Goertzel remains.

### Tests added

| Test | What it proves |
|---|---|
| `spectral_goertzel_isolates_a_frequency` | 100× rejection off-target, A²/4 power law, ×4 on doubled amplitude, DC rejected, absurd targets refused instead of aliased |
| `spectral_harmonic_ratios` | Known H2/H3 ratios recovered to 2 %, high notes report how many harmonics actually fit under Nyquist, `totalPower` = A²/2 |
| `spectral_fft_places_the_peak_correctly` | Bin-centred tones peak in exactly the right bin, 50× above neighbours |
| `spectral_centroid_and_flatness` | Centroid tracks the tone within ~15 %, flatness orders noise > harmonic tone > pure sine, band energy is 100× in-band |
| `spectral_accessors_are_safe_without_spectrum` | No descriptor invents a value before a spectrum exists |
| `spectral_fft_agrees_with_goertzel` | **Two independent methods agree**: the FFT amplitude ratio squared equals the Goertzel power ratio |

---

## PHASE 4 — Acoustic features

New files: `AcousticFeatures.{h,cpp}`.

`AcousticFeatures` is the meeting point between the measurement chain and every
consumer. Two design rules:

**Everything is measured, nothing is guessed.** A field holds its default until
the corresponding measurement has been made. Spectral fields are only filled on
one frame in `MIC_SPECTRAL_DECIMATION`, and `spectralValid` says which — a
consumer that reads `spectralCentroid` without checking would be reading an
older frame.

**The assembly logic is pure.** `AudioAnalyzer` depends on I2S and cannot be
built on the host, so the logic that decides *which* fields get filled and *how*
they are derived lives in `AcousticFeatureBuilder`, which is pure and fully
tested. `AudioAnalyzer` only wires it up.

`fillPitch` refuses to derive `overblowDetected` or `expectedNoteDetected` from
an invalid pitch: announcing an overblow on noise is worse than announcing
nothing.

### HNR is an approximation, and is labelled as one

`harmonicToNoiseRatio` treats "noise" as everything the four measured partials
do not capture — which includes breath, but also harmonics above the fourth. A
rigorous HNR needs the full spectrum and a noise-floor estimate. It is bounded
at ±`MIC_HNR_MAX_DB` because a synthetic pure tone would otherwise give
infinity. It behaves correctly in the direction that matters (adding breath
drops it by more than 6 dB).

**Superseded in PHASE 6, and it was worse than "approximate".** Measured against
the full spectrum, this approximation did not merely blur the distinction — it
ranked a breathy note *above* a timbred one, because H5..H8 fall outside the
four measured lines and were therefore counted as noise. It survives only as the
fallback used when the FFT is compiled out of the binary, and
`AcousticFeatures::hnrIsSpectral` says which of the two scales filled the field.
See PHASE 6.

### Tests added

| Test | What it proves |
|---|---|
| `features_defaults_are_honest` | No field suggests a measurement before one is made; `reset()` restores the dBFS floor, not 0 dBFS |
| `features_level_and_pitch_assembly` | dBFS, expected-note and overblow verdicts; **an invalid pitch produces no verdict at all** even when its flags are set |
| `features_spectral_assembly` | Known ratios, HNR drops >6 dB on added breath, bounded, nothing filled without a reliable f0, works with the FFT absent |
| `features_end_to_end_on_one_frame` | Ring → frame → level → pitch → spectrum on a flute-like note: right note, <5 cents, expected-note match, no clipping, plausible level and H2 |

### Exposure

`/api/diagnostics` gained an `audio` block (capture counters, dBFS, clipping,
DC offset) and the `{"t":"audio"}` WebSocket message now carries dBFS, clipping,
stability, H2/H3, HNR, centroid and flatness. Spectral fields are **omitted**
when not measured on this frame rather than repeated from an older one. No PCM
is ever streamed.

### Cumulative cost

| | PHASE 0 | PHASE 1 | PHASE 2 | PHASE 4 |
|---|---|---|---|---|
| Heap object | 13 096 B | 19 324 B | 15 300 B | **21 532 B** |
| — without FFT | | | | 15 388 B |
| Operations/frame | 81 920 | 81 920 | 92 160 | 99 712 |
| Frames/s | 25 | 62.5 | 62.5 | 62.5 |
| Estimated core load | ~5 % | ~12.8 % | ~14.4 % | **~15.6 %** |

Net RAM over the original baseline: **+8.2 kB** (+2.2 kB with the FFT compiled
out). All core-load figures are **estimates from operation counts** — no device
was available to measure them, and that remains the single largest unknown.

---

## PHASE 5 — Filtering and noise model

New files: `AudioFilters.{h,cpp}`, `NoiseModel.{h,cpp}`. Both hardware-free and
natively tested.

### 5.1 — Filtering, and *where* it is applied

```
retrait du continu  ->  passe-haut 100 Hz  ->  passe-bas 7 kHz
```

The chain runs on the **stream**, inside `drainI2S()`, before the ring — not
frame by frame. Two reasons, and the second is the decisive one:

1. An IIR filter has memory. Resetting it per frame would produce a settling
   transient at the start of *every* frame — a periodic artefact at exactly the
   analysis rate.
2. **Frames overlap by 50 %.** Filtering per frame would push each sample
   through the filter twice, with different states: the overlapping half of two
   consecutive frames would not even hold the same values. The test
   `filters_must_run_on_the_stream_not_per_frame` measures that divergence
   rather than asserting it.

**Clipping is not measured here.** The converter saturates, not the filtered
signal: a railed sample can drop back under the threshold after a high-pass, and
clipping would become invisible exactly when it matters. `countClipped()` runs
on the **raw** chunk, before the chain.

**What the filter must not break.** Measured gain of the default chain:

| Frequency | Gain | dB |
|---|---|---|
| 25 Hz | 0.044 | −27.1 |
| 100 Hz (HP cutoff) | 0.679 | −3.4 |
| **200 Hz (`MIC_PITCH_MIN_HZ`)** | **0.963** | **−0.33** |
| 1000 Hz | 1.002 | +0.02 |
| **4000 Hz (`MIC_PITCH_MAX_HZ`)** | **0.972** | −0.25 |
| 7000 Hz (LP cutoff) | 0.709 | −3.0 |
| 14000 Hz | 0.027 | −31.4 |

The pitch range moves by less than half a decibel. A `static_assert` enforces
`MIC_FILTER_HP_HZ ≤ MIC_PITCH_MIN_HZ / 2` at compile time, and the test asserts
the ±0.5 dB budget — a cutoff raised to 130 Hz gives −0.79 dB at 200 Hz and
fails both. An absurd cutoff makes the cell **transparent**, never unstable.

### 5.2 — Why one noise floor is not enough

The auto-calibration measured one noise floor per note, valve closed and air at
rest. That is insufficient for a simple reason: **the flute's machinery is part
of the noise, and its level depends on the operating point.** A pump at 90 %
does not sound like a stopped pump, and does not have the same spectrum. An SNR
computed against a floor measured with the pump off therefore overstates the
quality of every note played with the pump running — which is every note.

Seven profiles: `ambient`, `pump_idle/medium/high`, `fan_idle/medium/high`.
`profileForState(airMode, pumpPercent, fanPercent)` selects the one matching the
**real** state, which `WebConfigurator::update()` declares before every analysis.

The test `noise_profiles_are_per_state` measures the size of the error: the same
0.20 signal scores **more than 20 dB better** against `ambient` than against
`pump_high`.

### What is stored

No PCM. A profile keeps only statistics — mean level, per-band energy, spectral
flatness, dominant peak. One second of raw audio would cost 128 kB; a complete
profile costs **48 bytes**.

Six bands (100/250/500/1000/2000/4000/8000 Hz) let a profile distinguish two
noises of the *same level* but different spectra — a tonal mechanical line from
broadband breath. Band energies need the FFT; without it a profile still carries
a usable level, which is less rich but not wrong.

### What is not measured is not invented

- A profile that was never captured stays `valid = false`.
- `snrDb()` returns `valid = false` rather than a number computed against an
  imaginary floor.
- The fallback to `ambient` is **explicit** (`usedFallback`), because it likely
  overstates quality — and `/api/diagnostics` raises a warning saying so.
- A capture shorter than `MIC_NOISE_MIN_FRAMES` (32 frames ≈ 0.5 s) is
  **rejected**, not filed as a low-confidence profile. Capture length is capped
  at `MIC_NOISE_MAX_FRAMES` so a caller that forgets to stop cannot accumulate
  forever.
- Capturing while a note is sounding is refused (`note_playing`): it would
  measure the note, not the noise.
- A capture **stops by itself** at `MIC_NOISE_MAX_FRAMES` and files what it
  collected. Without that, a browser tab closed mid-capture would leave the
  microphone and the whole DSP running indefinitely. The web layer notices and
  returns the analyser to its previous activity state, broadcasting
  `{"t":"noise","auto_stopped":1}`.

### Reachable, not theoretical

A capability nobody can trigger is not delivered. The WebSocket carries
`{"t":"noise_cal","mode":"start"|"stop"|"reset"}`. The operator brings the
instrument to the wanted state with the existing test commands (`pump_target`,
`fan_target`), then captures; the analyser writes into the profile matching the
state it reads. `/api/diagnostics` lists every profile, which were captured, and
which one the current SNR is measured against.

### A trap found while wiring this up

`analyzeSpectrum()` computes nothing without a reliable fundamental — and a
noise capture, by definition, has no note. Passing `_spectral` straight to
`accumulate()` would therefore have made the profile accumulate, frame after
frame, **the spectrum left over from the last note played**: the profile would
have described that note rather than the noise, and `spectralValid` would have
looked perfectly healthy throughout.

The spectrum is now recomputed explicitly for each captured frame, and
`nullptr` is passed if that fails, so a stale spectrum can never be mistaken for
a fresh one. A capture is a few tenths of a second of explicit operator action;
paying one FFT per frame there is the right trade.

The unit tests did not catch this — they feed `NoiseModel` a freshly computed
spectrum directly. It is guarded by a structural assertion in
`test_audio_phase5_noise_is_per_state_and_honest`.

### Persistence — deliberately not yet

Profiles live in RAM and are lost on reboot. Making them persistent needs the
versioned configuration format that a later phase introduces; doing it here
would create a format to migrate twice.

### Cost

| | PHASE 4 | PHASE 5 |
|---|---|---|
| `AudioFilterChain` | — | 76 B |
| `NoiseModel` (7 × 48 B) | — | 388 B |
| `AcousticFeatures` | 80 B | 88 B |
| **Total (heap object)** | 21 532 B | **22 004 B** |
| Operations/frame | 99 712 | 105 344 |
| Estimated core load | ~15.6 % | **~16.5 %** |

**+472 B** for the whole phase. Net over the original baseline: +8.7 kB.

### Tests added

| Test | What it proves |
|---|---|
| `filters_shape_is_correct` | −3 dB at both cutoffs, steep rejection outside, **< 0.5 dB across the whole pitch range** |
| `filters_can_be_disabled_and_are_safe` | Zero/negative/absurd cutoffs disable the stage instead of destabilising it; no NaN over 20 loud blocks |
| `filters_must_run_on_the_stream_not_per_frame` | Stream filtering keeps overlapping halves byte-identical; per-frame filtering measurably does not |
| `clipping_must_be_measured_before_filtering` | 200 railed samples counted raw, fewer after filtering — the reason clipping is measured first |
| `noise_capture_requires_enough_frames` | Short captures rejected, abort stores nothing, invalid ids handled |
| `noise_profiles_are_per_state` | Rising levels per state, **>20 dB SNR error from using the wrong profile**, recapture overwrites |
| `noise_snr_is_honest_when_unmeasured` | Invalid when nothing captured, explicit fallback, floored at 0, bounded, degenerate inputs refused |
| `noise_profile_selection_follows_the_real_state` | Pump vs fan vs passive modes, exact threshold boundaries, every profile named |
| `noise_bands_distinguish_spectra` | A tonal line and broadband noise of similar level are told apart by band energy, flatness and peak |
| `noise_works_without_spectrum` | Level profile still usable with the FFT compiled out; bands stay zero rather than invented |
| `noise_capture_is_bounded` | Capture capped; accumulating outside a capture does nothing |

Sensitivity was verified by reintroducing each defect: an SNR that ignores the
requested profile, an accepted short capture, and a high-pass raised into the
pitch range each make the corresponding test fail.

---

## PHASE 6 — Acoustic classification and quality

Components: `AcousticQuality.{h,cpp}` (pure, host-testable), driven once per
analysed frame by `AudioAnalyzer::analyzeAcoustics()`.

### What it produces

| Output | Meaning |
|---|---|
| `AcousticClassification` | one `AcousticState` + *why it could not do better* |
| `QualityScore` | weighted 0..1 mark over seven criteria, with `weightUsed` |
| `BreathinessResult` | 0 = pure and timbred, 1 = essentially breath |
| `OverblowResult` | three conjoint criteria, never one alone |
| `SqueakResult` | retroactively confirmed, not announced on suspicion |

Priority order of the classifier: clipping → silence → squeak → overblow →
wrong note → weak → breathy → unstable → good. "Good" is last on purpose: it is
what remains when nothing else is wrong, never a default.

### The HNR changed scale, and the two scales must never mix

PHASE 4 filled `harmonicToNoiseRatio` with a Goertzel approximation over four
partials. It did not merely confuse a timbred note with a breathy one — **it
ranked them the wrong way round**. Two notes matched to within 0.29 dB of
level, through the full chain:

| | Goertzel HNR | spectral HNR | breathiness | quality |
|---|---|---|---|---|
| timbred note | −0.06 dB | **+32.33 dB** | 0.139 | 0.973 |
| breathy note | +9.30 dB | **+9.03 dB** | 0.671 | 0.575 |

The four Goertzel lines captured 49 % of the timbred note's power (H5..H8
counted as *noise*) against 69 % of the breathy one. The spectral measurement's
harmonic windows capture 100.0 %.

`SpectralAnalyzer::harmonicNoiseRatio()` measures it over the full spectrum:
harmonic windows around each usable partial, and a noise floor taken as the
**median** of the non-harmonic bins — not the mean, which a few strong bins drag
upward. The median is found by bisection on the value, so it needs no sort
buffer and no copy of the spectrum: `sizeof(SpectralAnalyzer)` is 6152 bytes
before and after.

Thresholds were recalibrated on the new scale:

| Threshold | before | after | measurement that justifies it |
|---|---|---|---|
| `AQ_BREATH_HNR_TONE_DB` | 20.0 dB | **34.0 dB** | breath 0.010 → 34.40 dB, the point where spectral flatness also crosses its own "tone" threshold — both components declare *more breath* at the same place |
| `AQ_QUALITY_HNR_GOOD_DB` | 20.0 dB | **34.0 dB** | at 20 dB a note carrying 5 % breath (20.55 dB) saturated the component at 1.00 — the same harmonic mark as a note with no breath at all |
| `AQ_BREATH_HNR_NOISE_DB` | 0.0 dB | 0.0 dB | unchanged value, justification redone: on the spectral scale 0 dB is *literal* — line energy equals the floor extended over the band |

**The two scales are never averaged together.** `computeBreathiness()` and
`computeAcousticQuality()` use the HNR component only when
`AcousticFeatures::hnrIsSpectral` is true. Without that guard the *same held
note* would be declared "almost pure breath" on one frame in
`MIC_SPECTRAL_DECIMATION` and "clean" on the next: the gap measured between two
consecutive frames of an identical signal reaches 31.88 dB. This is verified by
mutation — removing the guard makes `quality_breathiness_is_monotone` fail, and
that is a *pre-existing* test, not one written for the occasion.

### Why the spectral HNR is not held between FFT frames

The FFT runs one frame in four, so the harmonic component exists at 15.6 Hz
while the rest of the score updates at 62.5 Hz. Holding the last spectral HNR
across the other three frames would make the component always available, at the
price of a value up to 64 ms old presented as current.

That is exactly what `fftValid` exists to forbid, and exactly the defect fixed
in `0ab0881`, where noise capture was accumulating the stale spectrum of the
previous note. The option was considered and rejected twice, independently.

**Consequence a consumer must handle.** `QualityScore::weightUsed` alternates
between **0.90** (spectral frame) and **0.75** (decimated frame); breathiness
likewise drops from 1.00 to 0.30. Two scores with different `weightUsed` are
**not the same measurement** and must not be averaged together: a naive
one-second mean would blend 15 spectral scores with 47 partial ones and produce
a number that means nothing. Group by `weightUsed`, or plot only the spectral
frames.

*Corrected by the audit.* This paragraph previously said 0.75 / 0.60. Those are
the values when **no noise profile has been captured** either, i.e. a degraded
configuration that `/api/diagnostics` flags with a warning of its own — not the
nominal case. The weights sum to 1.00; attack (0.10) is structurally absent, so
a spectral frame reaches 0.90, and a decimated frame loses harmonic (0.15) as
well, reaching 0.75. Since this number is the *only* instruction this document
gives for grouping scores, the error was not cosmetic: filtering on 0.75 would
have kept the decimated frames and discarded the spectral ones — the exact
opposite of the intent.

The missing-ness is published rather than hidden — the same discipline as
`snrUsedFallback` and `missing*` elsewhere in this chain. `stabilityValid` was
named here too, wrongly: until the audit it was the one flag whose value was
published (`stab`, on the WebSocket push) while the flag itself reached neither
channel. It is now gated like the others.

### What is not measured is not invented

`attackQuality` stays at `AQ_ATTACK_NOT_MEASURED`. PHASE 7 measures
milliseconds; the score wants a 0..1; and no threshold in this project says
which attack duration is worth 1. Inventing one would dress a matter of taste
as a measurement. The score is renormalised over the other six criteria, covers
90 % of the specification, and `weightUsed` carries the gap.

The `missing*` flags (`pitch`, `snr`, `spectrum`, `expectedNote`, `stability`,
`squeakHistory`) exist for the same reason: a "good" reached because nothing
could be measured is not a "good", and the interface must be able to tell.

### A trap found while wiring this up: four lifecycle holes, not one

`AudioAnalyzer::update()` returns on its first line when the analyser is
inactive or uninitialised. The `MIC_FRAME_STALE_MS` ceiling therefore **never**
applies there, and nothing expires on its own. Four paths each left the last
verdict published as though it described the present instant:

- `resetMicrophone()` — republished the state and score from *before* the reset,
  and the brightness baseline learned before it judged the frames after. It also
  zeroes `_frameTimestamp`, which *disarms* the staleness ceiling.
- `end()` — same defect.
- `begin()` — a restarting stream inherited the entire past.
- `setActive(false)` — a pause froze the verdict for its whole duration. This
  fourth one had been seen by nobody.

`setActive()` now acts on **transitions**: several callers re-post the value it
already holds, and acting on the value would wipe the pitch history at every
event, so stability would never be measured at all.

### Where the classification runs, and why the order matters

`analyzeAcoustics()` is called from `update()` **after** `_features` carries its
sequence number and timestamp — not from `analyzeFrame()`. `updateSqueak()`
refuses a non-contiguous sequence, and `AcousticTiming` dates its instants on
`timestamp`. Called one step earlier it would have seen the *previous* frame's
identity: a history gap on every frame, and a systematic one-period offset on
every temporal measurement.

`computeBreathiness()` is deliberately **not** called separately: `classify()`
already computes it and stores it in `out.breathiness`. It is a pure function,
same inputs, identical result; calling it again would cost eight extra
1024-point Goertzel passes per frame, 62.5 times a second. A static-audit
assertion locks that choice in place.

### Cost

| Item | Measured |
|---|---|
| `SqueakDetector` | 52 B |
| `AcousticClassification` | 72 B |
| `QualityScore` | 24 B |
| `AcousticFeatures::hnrIsSpectral` | **0 B** — fits existing padding next to `fftValid`; `sizeof` is 96 B before and after |
| `HarmonicNoiseRatio` | 28 B, stack temporary |
| CPU, per analysed frame | ≈ 12 % of one YIN pass (8 Goertzel for breathiness, overblow, brightness) |
| CPU, per FFT frame | one `harmonicNoiseRatio()` ≈ 3.5 % of the FFT that just ran on the same frame |

Zero dynamic allocation in the frame path, locked by a static-audit assertion.
CPU figures are **operation counts, not microseconds** — nothing has run on an
ESP32 here.

### Tests added (PHASE 6)

| Test | What it proves |
|---|---|
| `quality_hnr_scale_is_the_one_the_thresholds_describe` | prints the full measured range and locks the three thresholds onto it |
| `quality_breathiness_rises_with_breath_on_the_spectral_scale` | six sweep points, strict growth — **fails with the old thresholds** |
| `quality_hnr_component_is_absent_on_the_goertzel_scale` | over 16 consecutive frames, `usedHnr == hnrIsSpectral` |
| `quality_score_ranks_a_timbred_note_above_a_breathy_one` | level-matched pair through the real chain |
| `quality_hnr_is_absent_without_fft` (`#else` branch) | without FFT the component disappears rather than being compared to thresholds that do not describe it |
| `features_fft_fields_never_claim_to_be_fresh` | `hnrIsSpectral` true on an FFT frame, **false again** on a decimated one |

Three assertions written *before* measurement were replaced by measured ones,
each with an **added** constraint rather than a relaxed one — including one that
was simply wrong (`noiseOnlyDb > -MIC_HNR_MAX_DB`: on pure noise the measurement
legitimately reaches the bound for two seeds out of three).

**Validation level: simulated audio validated.** The three thresholds are
numbers drawn from synthetic PCM. The upper reference sits only 6 dB below the
`MIC_HNR_MAX_DB` bound: on a real microphone, room noise and pump noise will
probably prevent a genuinely clean note from reaching 34 dB, and the component
will then report breath that is not there. **This is the most likely
recalibration to redo**, and the suite prints the whole table (`[hnr-calib]`
lines) precisely so it can be compared line by line against a microphone
capture.

---

## PHASE 7 — Acoustic timing

Components: `AcousticTiming.{h,cpp}` (pure, host-testable), fed on the *signal*
side by `AudioAnalyzer` and on the *order* side by the actuator chain.

### What it measures

| Measure | From | To |
|---|---|---|
| `commandToSoundLatency` | accepted MIDI order | audible sound |
| `airToSoundLatency` | air setpoint applied | audible sound |
| `attackTime` | sound onset | level established |
| `pitchStabilizationTime` | sound onset | pitch stable |
| `releaseTime` | stop order | sound gone |

Every measure carries its own `valid` flag. An `attackTime` of 0 without it
would read as an instantaneous attack — a defect this project has already made
once, and the reason the web layer routes every duration through a single
function that cannot emit a bare `ms`.

### The safety property, and how it is guaranteed

The specification says the audio engine must never disturb actuator safety and
must never be able to hold an actuator on if it crashes. So the flow is
**one-way**: the actuator chain *notifies*, it never *reads*.

- The observer is an optional `AcousticTiming*`, `nullptr` by default.
- All four notification helpers open with a null check.
- Every `bool` return is discarded through an explicit `(void)` cast, so no
  branch can depend on it. Refusals are already counted inside `AcousticTiming`
  (`rejectedEvents()`), so a caller-side counter would be redundant.
- The only condition present (`!_noteActive`) reads the *controller's own*
  state, never the observer's.

This is verifiable by inspection — four small functions — and it is also tested:
a full instrument scenario (out-of-range note, nominal note, CC on a held note,
monophonic replacement, too-short note, transport panic, calibration session,
All Sound Off) is played **three times** — without an observer, with one, and
with one that misbehaves (resets mid-note, invents orders, back-dates an order
into the future so every later hook is refused) — and the complete actuator
trace must be identical character for character.

### Where the hooks sit, and the trap

The instant that matters is when the order is **really accepted and acts on the
actuator**, not when a MIDI message arrives.

| Hook | Site | Why there |
|---|---|---|
| `noteCommanded` | `NoteSequencer::startNoteSequence()` | a refused note (missing PCA, actuator session, out of range) or one lost to a full or panic-flushed queue never reaches this line, so it never enters the latency statistics. `InstrumentManager::noteOn()` is only an enqueue — nothing is timed there |
| `airCommanded` | `AirflowController::computeAirflow()`, onset path only | distinguishes the setpoint that *starts* the air from a periodic recomputation (CC7/CC11/CC2 on an already-sounding note) |
| `valveOpened` | `AirflowController::openValve()`, guarded by `_noteActive` | a bench opening (`testSolenoid` from the web UI or the calibrator) belongs to no note and must not inflate `rejectedEvents()` |
| `noteReleased` | real stop paths, including `stop()` | the *real* extinction, not the Note Off message: a note shorter than `minNoteDurationMs` is deferred and notified only when extinction is actually commanded. `stop()` covers panic, All Sound Off, lost transport and calibrator takeover — without it the state machine would stay armed to its 60 s ceiling |

### The wiring, and the one real hazard

`WebConfigurator` owns the `AudioAnalyzer` (`new` in `begin()`, `delete` in the
destructor) and receives the `InstrumentManager`: it is the only place the two
worlds meet.

- `begin()`: `_instrument->setTimingObserver(&_audio->timing())`, conditioned on
  the microphone being **detected**, not merely on the instrument existing.
  Without a microphone nobody feeds the timing any frames, so every note would
  open a cycle ending in `TIMING_TIMEOUT` and the interface would read "no sound
  measured" when the truth is "nobody was listening".
- destructor: `setTimingObserver(nullptr)` **before** `delete _audio`. The
  observer points inside `_audio`, which `WebConfigurator` destroys, while
  `InstrumentManager` outlives it. Without the detach, the next note would write
  into freed memory.

Known limitation, shared with `_autoCal`: a microphone plugged in after startup
does not re-arm this wiring. In BLE mode the web server never starts, so no
audio analysis exists at all — a pre-existing architectural fact.

### Cost

`AcousticTiming` 404 B (inside `AudioAnalyzer`), plus 12 B of observer pointers
across the actuator chain. Total for PHASES 6+7 on `AudioAnalyzer`: **+552 B**,
bringing it to 22 648 B. `AcousticTiming::update()` is O(1) over 4-to-8 element
windows. No dynamic allocation anywhere in either path.

### Tests added (PHASE 7)

| Test | What it proves |
|---|---|
| `timing_observer_cannot_touch_actuators` | three runs, identical actuator traces — plus anti-vacuity: the observer must really have received all four orders |
| `timing_nominal_reports_four_orders_in_order` | `commandToSound − airToSound == servoToSolenoidDelayMs`; a zero gap would mean the two hooks sit at the same place |
| `timing_refused_note_is_never_commanded` | four refusal cases **and** the same note accepted, so the test cannot pass by everything being broken |
| `timing_monophonic_replacement_closes_before_opening` | the old note closes as `TIMING_ABORTED` with `releaseTime` left **invalid**, never zero |
| `timing_from_features_trusts_the_detector_verdict` | the reconstructed pitch criterion *assumed* `runYin()`'s range invariant instead of expressing it, and accepted an aliased out-of-range frequency the detector had rejected |

**Validation level: unit tested + firmware compiled.** The frames in these tests
are dB values placed by hand, not PCM. `AudioAnalyzer.cpp` itself is not in the
host build (it depends on I2S), so the suite does not execute it; it was checked
by a scratch execution harness that really runs `update()` on 56 frames
(62.2 frames/s measured, FFT 13/56, zero history gap), by targeted compilation,
and by adversarial diff review. That harness is **not in CI** — and the static
audit verifies that a call is *present*, not that it is *reachable*: an early
`return` inserted before it would slip past.

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
                            ┌────────────┼──────────────┬───────────────┐
                            ▼            ▼              ▼               ▼
                       AutoCalibrator  Diagnostics  AcousticQuality  AcousticTiming
                                                    (state, score)   (latencies)
                                                         │               ▲
                                                         ▼               │ order instants
                                                    Live monitor    NoteSequencer /
                                                                    AirflowController
```

The arrow into `AcousticTiming` from the actuator chain is **one-way**: the
instrument notifies, it never reads. No actuator decision depends on the
observer's presence or on any value it returns.

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
| 3 | Goertzel + optional FFT | **unit tested** |
| 4 | `AcousticFeatures` | **unit tested** |
| 5 | Filtering + per-state noise model | **unit tested** |
| 6 | Classification, quality, spectral HNR + recalibration | **simulated audio validated** |
| 7 | Acoustic timing (order side + signal side) | **unit tested** + **firmware compiled** |
| 8+ | Persistence, learned parameters, closed-loop calibration | not started |

## Next phase recommended

**PHASE 8 — persistence of what has been learned**, and it is now the blocking
one rather than merely the next.

PHASES 5, 6 and 7 all produce state that is *measured on this machine* and lost
on reboot: the seven noise profiles, and now the timing references of a given
instrument. Every one of them has to be re-measured at each power-up, which
means the quality score is not comparable between two sessions of the same
flute. That is the gap that keeps this chain a live monitor instead of an
instrument model.

It also needs the split the specification asks for and which does not yet
exist: firmware parameters stay in `settings.h`, instrument parameters and
learned parameters belong in LittleFS, versioned, so a firmware update does not
silently invalidate a calibration.

**Before any of that, one measurement is worth more than any new phase: connect
a real INMP441.** Nothing here is above "simulated audio validated", and the
single most likely thing to be wrong is `AQ_BREATH_HNR_TONE_DB` at 34 dB, only
6 dB below the bound (see PHASE 6). The suite prints the whole calibration table
for exactly that comparison.

## Known limitations

- Nothing has been validated against a real microphone or a real flute. Every
  measurement in this document comes from synthetic PCM.
- CPU time is estimated from operation counts, never measured on the device.
  This is the largest unknown in the whole chain.
- `harmonicToNoiseRatio` is now measured on the full spectrum when the FFT ran
  on that frame, and remains the PHASE 4 Goertzel approximation otherwise;
  `hnrIsSpectral` says which. `snrDb` is a *different* quantity (level against
  the machine's own noise floor) and the two coexist deliberately.
- **`QualityScore::weightUsed` alternates between 0.90 and 0.75** at frame rate,
  because the harmonic component only exists on FFT frames (15.6 Hz). Scores
  with different `weightUsed` are not the same measurement and must not be
  averaged together. (0.75 / 0.60 is the same alternation with no noise profile
  captured — a degraded configuration, not the nominal one.) See PHASE 6.
- Attack quality is measured in milliseconds by PHASE 7 but does not feed the
  PHASE 6 score: no threshold says which duration is worth 1, and inventing one
  would dress taste as measurement.
- The static audit checks that a call is *present*, not that it is *reachable*.
  An early `return` inserted before it would pass. `AudioAnalyzer.cpp` is not in
  the host build, so only that audit and an out-of-repo execution harness cover
  it — putting it in the host build behind an I2S seam would close this gap.
- A microphone plugged in after startup does not re-arm the timing observer or
  the auto-calibrator. In BLE mode the web server never starts, so no audio
  analysis exists at all.
- Noise profiles are lost on reboot (see PHASE 5, persistence).
- The 62.5 frames/s rate only applies while `setActive(true)` — mic monitor or
  calibration — not during ordinary MIDI playback.
- Pitch accuracy on synthetic pure tones is now < 1 cent (was ±47). This has
  never been checked against a real microphone, where noise, room response and
  the flute's own spectrum all apply.
