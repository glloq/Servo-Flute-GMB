/***********************************************************************************************
 * AutoCalMath - Hardware-free numeric core of the auto-calibrator
 *
 * All decision logic that turns raw audio frames into per-note calibration results
 * lives here as pure functions: adaptive noise threshold, median filtering, per-frame
 * validity, per-position aggregation, nominal airflow, confidence and SNR scoring.
 *
 * Keeping this logic free of I2S / Arduino dependencies lets the native unit tests
 * exercise the exact production code with simulated audio, without real hardware.
 ***********************************************************************************************/
#ifndef AUTO_CAL_MATH_H
#define AUTO_CAL_MATH_H

#include <math.h>
#include <stdint.h>
#include "settings.h"
#include "PitchMath.h"

namespace AutoCalMath {

// Upper bound for the small per-position sample buffers processed here.
constexpr int kMaxFrames = 16;

// One analysis frame as seen by the calibrator (a snapshot of the audio source).
struct AudioFrame {
  bool  pitchValid;   // YIN confidence high enough to trust the pitch
  float rms;          // RMS level of the (DC-removed) frame
  float hz;           // detected fundamental (Hz), 0 if none
  int   midi;         // nearest MIDI note, 0 if none
  float cents;        // signed cents deviation from `midi`
  float confidence;   // YIN confidence 0..1
};

// Aggregated statistics for a single airflow position (a set of frames).
struct PositionStats {
  uint8_t validFrames;     // frames matching the expected note within tolerance
  uint8_t overblowFrames;  // frames matching the octave above (overblow)
  uint8_t totalFrames;     // number of frames evaluated
  float   medianHz;        // median frequency of the valid frames
  float   medianCents;     // median cents deviation of the valid frames
  float   meanRms;         // mean RMS across all frames
  float   centsSpread;     // max-min cents among valid frames (lower = more stable)
};

// --- Adaptive threshold -------------------------------------------------------

// soundThreshold = max(MIC_RMS_ABSOLUTE_MIN, noiseRms * AUTOCAL_NOISE_RATIO)
inline float computeSoundThreshold(float noiseRms) {
  float dynamic = noiseRms * AUTOCAL_NOISE_RATIO;
  return (dynamic > MIC_RMS_ABSOLUTE_MIN) ? dynamic : MIC_RMS_ABSOLUTE_MIN;
}

// --- Median -------------------------------------------------------------------

// Median of up to kMaxFrames values. Copies into a local buffer (does not mutate
// the caller's data). Returns 0 for an empty input.
inline float median(const float* values, int n) {
  if (n <= 0) return 0.0f;
  if (n > kMaxFrames) n = kMaxFrames;
  float buf[kMaxFrames];
  for (int i = 0; i < n; i++) buf[i] = values[i];
  // Insertion sort (n is tiny).
  for (int i = 1; i < n; i++) {
    float key = buf[i];
    int j = i - 1;
    while (j >= 0 && buf[j] > key) { buf[j + 1] = buf[j]; j--; }
    buf[j + 1] = key;
  }
  if (n & 1) return buf[n / 2];
  return 0.5f * (buf[n / 2 - 1] + buf[n / 2]);
}

// --- Per-frame classification -------------------------------------------------

// A frame counts as the expected note when the pitch is trustworthy, loud enough,
// exactly the expected MIDI note, and within the cents tolerance. Exact MIDI
// matching automatically rejects neighbour notes and the octave above.
inline bool frameIsNote(const AudioFrame& f, int expectedMidi, float soundThreshold,
                        float toleranceCents, float minConfidence) {
  if (!f.pitchValid) return false;
  if (f.confidence < minConfidence) return false;
  if (f.rms <= soundThreshold) return false;
  return PitchMath::isNoteMatch(f.midi, f.cents, expectedMidi, toleranceCents);
}

// A frame is an overblow when it is trustworthy, loud, and matches the octave
// (or twelfth's octave) above the expected note.
inline bool frameIsOverblow(const AudioFrame& f, int expectedMidi, float soundThreshold,
                           float minConfidence) {
  if (!f.pitchValid) return false;
  if (f.confidence < minConfidence) return false;
  if (f.rms <= soundThreshold) return false;
  return PitchMath::isOctaveAbove(f.midi, expectedMidi);
}

// --- Per-position aggregation -------------------------------------------------

inline PositionStats evaluatePosition(const AudioFrame* frames, int n, int expectedMidi,
                                     float soundThreshold, float toleranceCents,
                                     float minConfidence) {
  PositionStats s{0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f};
  if (n <= 0) return s;
  if (n > kMaxFrames) n = kMaxFrames;
  s.totalFrames = (uint8_t)n;

  float validHz[kMaxFrames];
  float validCents[kMaxFrames];
  int   nv = 0;
  float rmsSum = 0.0f;
  float minCents = 0.0f, maxCents = 0.0f;

  for (int i = 0; i < n; i++) {
    rmsSum += frames[i].rms;
    if (frameIsNote(frames[i], expectedMidi, soundThreshold, toleranceCents, minConfidence)) {
      validHz[nv] = frames[i].hz;
      validCents[nv] = frames[i].cents;
      if (nv == 0 || frames[i].cents < minCents) minCents = frames[i].cents;
      if (nv == 0 || frames[i].cents > maxCents) maxCents = frames[i].cents;
      nv++;
    } else if (frameIsOverblow(frames[i], expectedMidi, soundThreshold, minConfidence)) {
      s.overblowFrames++;
    }
  }

  s.validFrames = (uint8_t)nv;
  s.meanRms = rmsSum / n;
  if (nv > 0) {
    s.medianHz = median(validHz, nv);
    s.medianCents = median(validCents, nv);
    s.centsSpread = maxCents - minCents;
  }
  return s;
}

// A position is accepted when enough frames matched the expected note.
inline bool positionValid(const PositionStats& s, int requiredValidFrames) {
  return s.validFrames >= requiredValidFrames;
}

// A position is treated as overblown when the octave above dominates (majority of
// frames), which marks the upper airflow limit.
inline bool positionOverblown(const PositionStats& s) {
  return s.overblowFrames > 0 && s.overblowFrames >= (s.totalFrames + 1) / 2;
}

// --- Result scoring -----------------------------------------------------------

inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// Nominal airflow: min + fraction*(max-min), clamped to [min, max].
inline uint8_t computeNominalPercent(uint8_t airMin, uint8_t airMax, float fraction) {
  if (airMax <= airMin) return airMin;
  int span = (int)airMax - (int)airMin;
  int nominal = (int)airMin + (int)lroundf(fraction * (float)span);
  if (nominal < airMin) nominal = airMin;
  if (nominal > airMax) nominal = airMax;
  return (uint8_t)nominal;
}

// Signal-to-noise ratio in dB (guards against a zero/negative noise floor).
inline float snrDb(float signalRms, float noiseRms) {
  if (noiseRms < 1e-9f) noiseRms = 1e-9f;
  if (signalRms < 1e-9f) return 0.0f;
  float db = 20.0f * log10f(signalRms / noiseRms);
  return db < 0.0f ? 0.0f : db;
}

// Pitch stability 0..1 from the cents spread of a position (0 spread = perfectly
// stable = 1). `referenceCents` is the spread that maps to 0.
inline float pitchStability(float centsSpread, float referenceCents) {
  if (referenceCents <= 0.0f) return 1.0f;
  return clamp01(1.0f - centsSpread / referenceCents);
}

// Quality of a candidate nominal position, 0..1. Combines pitch accuracy, pitch
// stability, loudness over noise and YIN confidence. The margin from the min/max
// limits is applied separately at finalize time (it needs the final range).
inline float positionQuality(float absMedianCents, float stability, float snr,
                            float yinConfidence, float toleranceCents) {
  float centsScore = clamp01(1.0f - absMedianCents / toleranceCents);
  float snrScore   = clamp01(snr / 24.0f);       // ~24 dB counts as an excellent SNR
  float confScore  = clamp01(yinConfidence);
  return 0.40f * centsScore + 0.25f * clamp01(stability) + 0.20f * snrScore + 0.15f * confScore;
}

// Overall per-note confidence 0..100 for the UI / persistence.
inline uint8_t computeConfidence(float absMedianCents, float stability, float snr,
                                float yinConfidence, float validRatio, float toleranceCents) {
  float centsScore = clamp01(1.0f - absMedianCents / toleranceCents);
  float snrScore   = clamp01(snr / 24.0f);
  float confScore  = clamp01(yinConfidence);
  float score = 0.30f * centsScore + 0.20f * clamp01(stability) +
                0.20f * snrScore + 0.15f * confScore + 0.15f * clamp01(validRatio);
  int pct = (int)lroundf(score * 100.0f);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return (uint8_t)pct;
}

// Enforce 0 <= min <= nominal <= max <= 100, keeping a guard margin from the
// limits when the range is wide enough. Returns a clamped nominal.
inline uint8_t clampNominal(uint8_t airMin, uint8_t airMax, uint8_t nominal, uint8_t guard) {
  if (nominal < airMin) nominal = airMin;
  if (nominal > airMax) nominal = airMax;
  int span = (int)airMax - (int)airMin;
  if (span > 2 * guard) {
    if (nominal < airMin + guard) nominal = airMin + guard;
    if (nominal > airMax - guard) nominal = airMax - guard;
  }
  return nominal;
}

}  // namespace AutoCalMath

#endif  // AUTO_CAL_MATH_H
