/***********************************************************************************************
 * PitchMath - Hardware-free pitch / frequency helpers
 *
 * Pure functions shared by AudioAnalyzer (I2S/YIN), AutoCalibrator and the native
 * unit tests. No Arduino, I2S or floating hardware dependency so the file compiles
 * unchanged on the ESP32 firmware target and on the host test runner.
 *
 * Conventions:
 *   - MIDI note number: 69 = A4 = 440 Hz.
 *   - "cents" is the signed deviation of a frequency from the equal-tempered pitch
 *     of a given MIDI note (1200 cents = 1 octave).
 ***********************************************************************************************/
#ifndef PITCH_MATH_H
#define PITCH_MATH_H

#include <math.h>

namespace PitchMath {

// Reference pitch (A4) used for the equal-tempered scale.
constexpr float kA4Hz = 440.0f;
constexpr int   kA4Midi = 69;
constexpr float kCentsPerOctave = 1200.0f;
constexpr float kCentsPerSemitone = 100.0f;

// Frequency (Hz) of an equal-tempered MIDI note.
inline float midiToHz(int midi) {
  return kA4Hz * powf(2.0f, (float)(midi - kA4Midi) / 12.0f);
}

// Nearest MIDI note for a frequency. Returns 0 for non-positive input.
inline int hzToMidi(float hz) {
  if (hz <= 0.0f) return 0;
  return (int)lroundf((float)kA4Midi + 12.0f * log2f(hz / kA4Hz));
}

// Signed cents deviation of hz from the equal-tempered pitch of `midi`.
inline float hzToCents(float hz, int midi) {
  if (hz <= 0.0f || midi <= 0) return 0.0f;
  float expected = midiToHz(midi);
  return kCentsPerOctave * log2f(hz / expected);
}

// True when `detectedMidi` is the same pitch class one or more octaves above the
// expected note (the classic overblow / YIN half-period error). Only checks the
// octave-up direction because overblowing a flute sounds the note an octave (or
// twelfth) higher, never lower.
inline bool isOctaveAbove(int detectedMidi, int expectedMidi) {
  if (detectedMidi <= expectedMidi) return false;
  int diff = detectedMidi - expectedMidi;
  return (diff % 12) == 0;  // +12, +24, ...
}

// True when the detected note is an exact match of the expected note and the
// cents deviation stays inside `toleranceCents`.
inline bool isNoteMatch(int detectedMidi, float cents, int expectedMidi, float toleranceCents) {
  if (detectedMidi != expectedMidi) return false;
  return fabsf(cents) <= toleranceCents;
}

}  // namespace PitchMath

#endif  // PITCH_MATH_H
