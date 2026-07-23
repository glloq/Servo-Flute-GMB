/***********************************************************************************************
 * PitchDetector - Hardware-free YIN pitch detection + microphone signal classification
 *
 * Extracted from AudioAnalyzer so the pitch core can be unit-tested natively with
 * synthetic PCM, with no I2S / Arduino dependency. Operates on a caller-provided
 * float buffer and owns its Hann window and YIN lag buffer (no dynamic allocation).
 *
 * Pipeline: DC-offset removal -> Hann window -> simplified YIN (absolute threshold,
 * local-minimum descent, conservative octave-up guard, parabolic interpolation).
 ***********************************************************************************************/
#ifndef PITCH_DETECTOR_H
#define PITCH_DETECTOR_H

#include <stdint.h>
#include <stddef.h>
#include "settings.h"

struct PitchResult {
  bool valid;        // confidence >= MIC_YIN_CONFIDENCE_MIN and frequency in range
  float hz;          // detected fundamental, 0 if none
  float confidence;  // YIN periodicity 0..1 (1 - aperiodicity)
};

// Classification of a raw microphone frame (for robust presence detection).
enum MicSignalClass {
  MIC_SIG_OK = 0,        // varied, in-range signal (mic present and sane)
  MIC_SIG_ALL_ZERO,      // all samples zero (bus idle / not wired)
  MIC_SIG_STUCK,         // non-zero but (near) constant (stuck line / no clock)
  MIC_SIG_SATURATED      // permanently clipped near full scale
};

class PitchDetector {
public:
  PitchDetector() { buildHannWindow(); }

  // Detect the pitch of `n` float samples (any DC offset is removed internally).
  // The buffer is modified in place (centred + windowed). `n` is clamped to
  // MIC_BUFFER_SIZE.
  PitchResult detect(float* samples, size_t n) const;

  // RMS of the DC-removed samples (does not modify the buffer).
  static float rms(const float* samples, size_t n);

  // Classify a raw 32-bit I2S frame (INMP441 packs 24 bits left-aligned). Pure
  // function so the presence heuristic can be unit-tested with synthetic buffers.
  static MicSignalClass classifyRaw(const int32_t* raw, size_t n);

private:
  float _hann[MIC_BUFFER_SIZE];
  mutable float _yinBuf[MIC_YIN_TAU_MAX + 2];
  void buildHannWindow();
};

#endif  // PITCH_DETECTOR_H
