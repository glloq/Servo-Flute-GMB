/***********************************************************************************************
 * AudioAnalyzer - I2S INMP441 microphone driver with real-time audio analysis
 *
 * Provides:
 * - I2S DMA-based microphone input (INMP441 MEMS mic, 32-bit words, 32 kHz)
 * - Automatic microphone detection (silence = not connected)
 * - DC-offset removal + RMS level computation
 * - Pitch detection via a simplified YIN algorithm with:
 *     - Hann windowing to limit edge errors,
 *     - parabolic interpolation for sub-sample accuracy,
 *     - a confidence value (1 - aperiodicity),
 *     - basic octave-error correction.
 * - MIDI note + cents deviation output.
 *
 * Implements IAudioSource so the AutoCalibrator can consume measurements without a
 * direct dependency on the I2S driver.
 *
 * Call begin() at startup - returns true if mic detected.
 * Call update() from main loop when active.
 ***********************************************************************************************/
#ifndef AUDIO_ANALYZER_H
#define AUDIO_ANALYZER_H

#include <Arduino.h>
#include "settings.h"
#include "IAudioSource.h"

#if MIC_ENABLED

#include <driver/i2s_std.h>

class AudioAnalyzer : public IAudioSource {
public:
  AudioAnalyzer();

  // Initialize I2S and probe for microphone. Returns true if mic detected.
  bool begin();

  // Release I2S resources
  void end();

  // Read I2S DMA buffer and run analysis. Call from loop().
  void update();

  // --- IAudioSource ---
  bool isMicDetected() const override { return _micDetected; }
  bool isSoundDetected() const override { return _soundDetected; }
  float getRMS() const override { return _rms; }
  float getPitchHz() const override { return _pitchHz; }
  int getPitchMidi() const override { return _pitchMidi; }
  float getPitchCents() const override { return _pitchCents; }
  float getPitchConfidence() const override { return _pitchConfidence; }
  bool isPitchValid() const override { return _pitchValid; }
  bool isActive() const override { return _active; }
  void setActive(bool active) override { _active = active; }

private:
  bool _active;
  bool _initialized;
  bool _micDetected;
  bool _soundDetected;
  float _rms;
  float _pitchHz;
  int _pitchMidi;
  float _pitchCents;
  float _pitchConfidence;   // 0..1, higher = more periodic / trustworthy
  bool _pitchValid;         // confidence >= MIC_YIN_CONFIDENCE_MIN and pitch in range

  i2s_chan_handle_t _rxHandle;

  int32_t _rawBuffer[MIC_BUFFER_SIZE];
  float _analysisBuffer[MIC_BUFFER_SIZE];
  float _hannWindow[MIC_BUFFER_SIZE];       // precomputed once in begin()
  float _yinBuf[MIC_YIN_TAU_MAX + 2];       // YIN difference buffer (off the stack)
  size_t _validSamples;

  unsigned long _lastUpdate;

  void buildHannWindow();
  bool detectMicrophone();
  void readI2S();
  void analyzeBuffer();
  float computeRMS();            // RMS of the DC-removed _analysisBuffer
  void removeDcOffset();         // subtract the mean in place
  void applyHannWindow();        // multiply _analysisBuffer by the Hann window
  bool computePitchYIN(float& outHz, float& outConfidence);
};

#endif // MIC_ENABLED
#endif // AUDIO_ANALYZER_H
