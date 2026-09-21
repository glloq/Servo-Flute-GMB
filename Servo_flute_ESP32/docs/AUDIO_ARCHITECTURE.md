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
essentially nothing. The likely cause is that **the Hann window is applied before
the YIN difference function**: the envelope makes `x[i] − x[i+τ]` depend on
position in the frame, which skews `d(τ)` and displaces its minimum. Windowing
belongs to the spectral path (FFT/Goertzel), not to an autocorrelation-family
estimator.

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
| 1 | Ring buffer, overlap, clipping, dBFS, I2S diagnostics | not started |
| 2 | YIN without window, expected-note tracking, richer result | not started |
| 3 | Goertzel + optional FFT | not started |
| 4 | `AcousticFeatures` | not started |
| 5+ | Noise model, classification, timing, quality, calibration | not started |

## Known limitations

- Nothing has been validated against a real microphone or a real flute.
- CPU time is estimated from operation counts, never measured on the device.
- Pitch accuracy is currently ±23 cents (A0-3); anything built on a cents
  measurement is therefore not yet trustworthy.
