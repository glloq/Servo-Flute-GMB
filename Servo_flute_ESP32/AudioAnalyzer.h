/***********************************************************************************************
 * AudioAnalyzer - I2S INMP441 microphone driver with real-time audio analysis
 *
 * Provides:
 * - I2S DMA-based microphone input (INMP441 MEMS mic, 32-bit words, 32 kHz).
 *   Works on both the ESP-IDF 5.x "std" I2S driver and the legacy IDF 4.x driver
 *   (selected at compile time), so it builds on Arduino-ESP32 2.0.x and 3.0.x.
 * - Robust microphone presence detection (all-zero / stuck / saturated / ok).
 * - DC-offset removal + RMS level, plus pitch detection delegated to the
 *   hardware-free PitchDetector (YIN + Hann + confidence), so the pitch core is
 *   unit-tested natively.
 * - MIDI note + cents deviation, YIN confidence, pitch validity.
 * - Frame freshness (sequence + timestamp) so consumers never count a frame twice
 *   and can detect a frozen source.
 *
 * Implements IAudioSource so AutoCalibrator consumes measurements without an I2S
 * dependency.
 ***********************************************************************************************/
#ifndef AUDIO_ANALYZER_H
#define AUDIO_ANALYZER_H

#include <Arduino.h>
#include "settings.h"
#include "IAudioSource.h"
#include "PitchDetector.h"

#if MIC_ENABLED

#include <esp_idf_version.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  #include <driver/i2s_std.h>
  #define MIC_I2S_STD_DRIVER 1
#else
  #include <driver/i2s.h>
  #define MIC_I2S_STD_DRIVER 0
#endif

// Detailed microphone status (mirrors PitchDetector::MicSignalClass plus a
// read-error state that only the driver can report).
enum MicStatus {
  MIC_STATUS_DETECTED = 0,
  MIC_STATUS_ALL_ZERO,
  MIC_STATUS_STUCK,
  MIC_STATUS_SATURATED,
  MIC_STATUS_READ_ERROR,
  MIC_STATUS_NOT_INIT
};

class AudioAnalyzer : public IAudioSource {
public:
  AudioAnalyzer();

  bool begin();
  void end();
  void update();

  // Re-probe / re-initialise the microphone without rebooting the ESP32.
  bool resetMicrophone();
  MicStatus getMicStatus() const { return _micStatus; }
  const char* getMicStatusString() const;

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
  uint32_t getFrameSequence() const override { return _frameSeq; }
  unsigned long getFrameTimestamp() const override { return _frameTimestamp; }

private:
  bool _active;
  bool _initialized;
  bool _micDetected;
  bool _soundDetected;
  MicStatus _micStatus;
  float _rms;
  float _pitchHz;
  int _pitchMidi;
  float _pitchCents;
  float _pitchConfidence;
  bool _pitchValid;
  uint32_t _frameSeq;
  unsigned long _frameTimestamp;

#if MIC_I2S_STD_DRIVER
  i2s_chan_handle_t _rxHandle;
#endif

  PitchDetector _pitch;
  int32_t _rawBuffer[MIC_BUFFER_SIZE];
  float _analysisBuffer[MIC_BUFFER_SIZE];
  size_t _validSamples;
  unsigned long _lastUpdate;

  bool installI2S();
  void uninstallI2S();
  bool detectMicrophone();       // reads a probe buffer and sets _micStatus
  void readI2S();
  void analyzeBuffer();
};

#endif // MIC_ENABLED
#endif // AUDIO_ANALYZER_H
