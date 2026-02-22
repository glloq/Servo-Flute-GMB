/***********************************************************************************************
 * AudioAnalyzer - I2S INMP441 microphone driver with real-time audio analysis
 *
 * Provides:
 * - I2S DMA-based microphone input (INMP441 MEMS mic)
 * - Automatic microphone detection (silence = not connected)
 * - RMS level computation
 * - Pitch detection via simplified YIN algorithm
 * - MIDI note + cents deviation output
 *
 * Call begin() at startup - returns true if mic detected.
 * Call update() from main loop when active.
 ***********************************************************************************************/
#ifndef AUDIO_ANALYZER_H
#define AUDIO_ANALYZER_H

#include <Arduino.h>
#include "settings.h"

#if MIC_ENABLED

class AudioAnalyzer {
public:
  AudioAnalyzer();

  // Initialize I2S and probe for microphone. Returns true if mic detected.
  bool begin();

  // Release I2S resources
  void end();

  // Read I2S DMA buffer and run analysis. Call from loop().
  void update();

  bool isMicDetected() const { return _micDetected; }
  bool isSoundDetected() const { return _soundDetected; }
  float getRMS() const { return _rms; }
  float getPitchHz() const { return _pitchHz; }
  int getPitchMidi() const { return _pitchMidi; }
  float getPitchCents() const { return _pitchCents; }
  bool isActive() const { return _active; }

  void setActive(bool active) { _active = active; }

private:
  bool _active;
  bool _initialized;
  bool _micDetected;
  bool _soundDetected;
  float _rms;
  float _pitchHz;
  int _pitchMidi;
  float _pitchCents;

  int32_t _rawBuffer[MIC_BUFFER_SIZE];
  float _analysisBuffer[MIC_BUFFER_SIZE];
  size_t _validSamples;

  unsigned long _lastUpdate;

  bool detectMicrophone();
  void readI2S();
  void analyzeBuffer();
  float computeRMS();
  float computePitchYIN();

  static int hzToMidi(float hz);
  static float hzToCents(float hz, int midi);
};

#endif // MIC_ENABLED
#endif // AUDIO_ANALYZER_H
