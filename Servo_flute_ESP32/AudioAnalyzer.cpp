#include "AudioAnalyzer.h"

#if MIC_ENABLED

#include <driver/i2s.h>
#include <math.h>

AudioAnalyzer::AudioAnalyzer()
  : _active(false), _initialized(false), _micDetected(false),
    _soundDetected(false), _rms(0), _pitchHz(0), _pitchMidi(0),
    _pitchCents(0), _validSamples(0), _lastUpdate(0) {
}

bool AudioAnalyzer::begin() {
  // Configure I2S for INMP441 (RX only, 32-bit, left channel)
  i2s_config_t i2s_config = {};
  i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  i2s_config.sample_rate = MIC_SAMPLE_RATE;
  i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2s_config.dma_buf_count = MIC_DMA_BUF_COUNT;
  i2s_config.dma_buf_len = MIC_DMA_BUF_LEN;
  i2s_config.use_apll = false;
  i2s_config.tx_desc_auto_clear = false;
  i2s_config.fixed_mclk = 0;

  i2s_pin_config_t pin_config = {};
  pin_config.bck_io_num = MIC_PIN_BCLK;
  pin_config.ws_io_num = MIC_PIN_LRCLK;
  pin_config.data_out_num = I2S_PIN_NO_CHANGE;
  pin_config.data_in_num = MIC_PIN_DIN;

  esp_err_t err = i2s_driver_install(MIC_I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    if (DEBUG) {
      Serial.print("ERREUR: AudioAnalyzer - i2s_driver_install: ");
      Serial.println(err);
    }
    return false;
  }

  err = i2s_set_pin(MIC_I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    if (DEBUG) {
      Serial.print("ERREUR: AudioAnalyzer - i2s_set_pin: ");
      Serial.println(err);
    }
    i2s_driver_uninstall(MIC_I2S_PORT);
    return false;
  }

  i2s_zero_dma_buffer(MIC_I2S_PORT);
  delay(100);  // Let I2S stabilize

  _initialized = true;
  _micDetected = detectMicrophone();

  if (DEBUG) {
    Serial.print("DEBUG: AudioAnalyzer - Mic detected: ");
    Serial.println(_micDetected ? "OUI" : "NON");
  }

  if (!_micDetected) {
    // Free resources if no mic
    i2s_driver_uninstall(MIC_I2S_PORT);
    _initialized = false;
  }

  return _micDetected;
}

void AudioAnalyzer::end() {
  if (_initialized) {
    i2s_driver_uninstall(MIC_I2S_PORT);
    _initialized = false;
  }
  _active = false;
}

bool AudioAnalyzer::detectMicrophone() {
  // Read a buffer and check if we get any non-zero data
  size_t bytesRead = 0;
  int32_t testBuf[256];

  esp_err_t err = i2s_read(MIC_I2S_PORT, testBuf, sizeof(testBuf), &bytesRead, 500);
  if (err != ESP_OK || bytesRead == 0) return false;

  size_t samples = bytesRead / sizeof(int32_t);
  int nonZero = 0;
  int64_t sum = 0;

  for (size_t i = 0; i < samples; i++) {
    if (testBuf[i] != 0) nonZero++;
    sum += abs(testBuf[i] >> 8);  // Check upper 24 bits
  }

  // If more than 10% of samples are non-zero, mic is present
  // A disconnected I2S bus reads all zeros
  return (nonZero > (int)(samples / 10));
}

void AudioAnalyzer::update() {
  if (!_initialized || !_active) return;

  unsigned long now = millis();
  if (now - _lastUpdate < 40) return;  // ~25 Hz analysis rate
  _lastUpdate = now;

  readI2S();
  if (_validSamples > 0) {
    analyzeBuffer();
  }
}

void AudioAnalyzer::readI2S() {
  size_t bytesRead = 0;
  esp_err_t err = i2s_read(MIC_I2S_PORT, _rawBuffer,
                            MIC_BUFFER_SIZE * sizeof(int32_t),
                            &bytesRead, 0);  // Non-blocking

  if (err != ESP_OK || bytesRead == 0) {
    _validSamples = 0;
    return;
  }

  _validSamples = bytesRead / sizeof(int32_t);

  // Convert to normalized float (-1.0 to 1.0)
  // INMP441 outputs 24-bit data left-aligned in 32-bit word
  const float scale = 1.0f / 2147483648.0f;
  for (size_t i = 0; i < _validSamples; i++) {
    _analysisBuffer[i] = (float)_rawBuffer[i] * scale;
  }
}

void AudioAnalyzer::analyzeBuffer() {
  _rms = computeRMS();
  _soundDetected = (_rms > MIC_RMS_THRESHOLD);

  if (_soundDetected) {
    _pitchHz = computePitchYIN();
    if (_pitchHz > 0) {
      _pitchMidi = hzToMidi(_pitchHz);
      _pitchCents = hzToCents(_pitchHz, _pitchMidi);
    } else {
      _pitchMidi = 0;
      _pitchCents = 0;
    }
  } else {
    _pitchHz = 0;
    _pitchMidi = 0;
    _pitchCents = 0;
  }
}

float AudioAnalyzer::computeRMS() {
  float sum = 0;
  for (size_t i = 0; i < _validSamples; i++) {
    sum += _analysisBuffer[i] * _analysisBuffer[i];
  }
  return sqrtf(sum / _validSamples);
}

float AudioAnalyzer::computePitchYIN() {
  // Simplified YIN algorithm for pitch detection
  const int tauMin = (int)(MIC_SAMPLE_RATE / MIC_PITCH_MAX_HZ);   // ~4
  int tauMax = (int)(MIC_SAMPLE_RATE / MIC_PITCH_MIN_HZ);         // ~80
  const int W = (int)_validSamples / 2;  // Window size

  // Safety clamp: yinBuf is stack-allocated, limit to avoid overflow
  const int TAU_LIMIT = 256;
  if (tauMax > TAU_LIMIT) tauMax = TAU_LIMIT;

  if (W < tauMax + 1) return 0;

  // Steps 1-2: Difference function + cumulative mean normalization
  float runningSum = 0;
  float bestTau = 0;
  bool found = false;

  // Pre-allocate on stack (max TAU_LIMIT + 2)
  float yinBuf[TAU_LIMIT + 2];
  yinBuf[0] = 1.0f;

  for (int tau = 1; tau <= tauMax; tau++) {
    float sum = 0;
    for (int i = 0; i < W; i++) {
      float delta = _analysisBuffer[i] - _analysisBuffer[i + tau];
      sum += delta * delta;
    }
    runningSum += sum;
    yinBuf[tau] = (runningSum > 0) ? (sum * tau / runningSum) : 1.0f;
  }

  // Step 3: Absolute threshold - find first dip below threshold
  for (int tau = tauMin; tau <= tauMax; tau++) {
    if (yinBuf[tau] < MIC_YIN_THRESHOLD) {
      // Step 4: Parabolic interpolation for sub-sample accuracy
      bestTau = (float)tau;
      if (tau > tauMin && tau < tauMax) {
        float s0 = yinBuf[tau - 1];
        float s1 = yinBuf[tau];
        float s2 = yinBuf[tau + 1];
        float denom = 2.0f * (2.0f * s1 - s2 - s0);
        if (fabsf(denom) > 1e-6f) {
          bestTau = tau + (s0 - s2) / denom;
        }
      }
      found = true;
      break;
    }
  }

  if (!found || bestTau < 1.0f) return 0;
  return (float)MIC_SAMPLE_RATE / bestTau;
}

int AudioAnalyzer::hzToMidi(float hz) {
  if (hz <= 0) return 0;
  // MIDI note = 69 + 12 * log2(hz / 440)
  return (int)roundf(69.0f + 12.0f * log2f(hz / 440.0f));
}

float AudioAnalyzer::hzToCents(float hz, int midi) {
  if (hz <= 0 || midi <= 0) return 0;
  // Expected frequency for MIDI note
  float expected = 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f);
  // Cents = 1200 * log2(hz / expected)
  return 1200.0f * log2f(hz / expected);
}

#endif // MIC_ENABLED
