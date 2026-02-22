#include "AudioAnalyzer.h"

#if MIC_ENABLED

#include <math.h>

AudioAnalyzer::AudioAnalyzer()
  : _active(false), _initialized(false), _micDetected(false),
    _soundDetected(false), _rms(0), _pitchHz(0), _pitchMidi(0),
    _pitchCents(0), _rxHandle(NULL), _validSamples(0), _lastUpdate(0) {
}

bool AudioAnalyzer::begin() {
  // --- New I2S standard driver (ESP-IDF 5.x) ---
  // Replaces legacy <driver/i2s.h> to avoid ADC driver conflict with analogRead()

  // 1. Create RX channel
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = MIC_DMA_BUF_COUNT;
  chan_cfg.dma_frame_num = MIC_DMA_BUF_LEN;

  esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &_rxHandle);
  if (err != ESP_OK) {
    if (DEBUG) {
      Serial.print("ERREUR: AudioAnalyzer - i2s_new_channel: ");
      Serial.println(err);
    }
    return false;
  }

  // 2. Configure standard mode (Philips I2S for INMP441)
  i2s_std_config_t std_cfg = {};

  // Clock config
  std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);

  // Slot config: 32-bit, mono left
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  // GPIO config
  std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.bclk = (gpio_num_t)MIC_PIN_BCLK;
  std_cfg.gpio_cfg.ws = (gpio_num_t)MIC_PIN_LRCLK;
  std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.din = (gpio_num_t)MIC_PIN_DIN;
  std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.ws_inv = false;

  err = i2s_channel_init_std_mode(_rxHandle, &std_cfg);
  if (err != ESP_OK) {
    if (DEBUG) {
      Serial.print("ERREUR: AudioAnalyzer - i2s_channel_init_std_mode: ");
      Serial.println(err);
    }
    i2s_del_channel(_rxHandle);
    _rxHandle = NULL;
    return false;
  }

  // 3. Enable the channel
  err = i2s_channel_enable(_rxHandle);
  if (err != ESP_OK) {
    if (DEBUG) {
      Serial.print("ERREUR: AudioAnalyzer - i2s_channel_enable: ");
      Serial.println(err);
    }
    i2s_del_channel(_rxHandle);
    _rxHandle = NULL;
    return false;
  }

  delay(100);  // Let I2S stabilize

  _initialized = true;
  _micDetected = detectMicrophone();

  if (DEBUG) {
    Serial.print("DEBUG: AudioAnalyzer - Mic detected: ");
    Serial.println(_micDetected ? "OUI" : "NON");
  }

  if (!_micDetected) {
    // Free resources if no mic
    i2s_channel_disable(_rxHandle);
    i2s_del_channel(_rxHandle);
    _rxHandle = NULL;
    _initialized = false;
  }

  return _micDetected;
}

void AudioAnalyzer::end() {
  if (_initialized && _rxHandle) {
    i2s_channel_disable(_rxHandle);
    i2s_del_channel(_rxHandle);
    _rxHandle = NULL;
    _initialized = false;
  }
  _active = false;
}

bool AudioAnalyzer::detectMicrophone() {
  // Read a buffer and check if we get any non-zero data
  size_t bytesRead = 0;
  int32_t testBuf[256];

  esp_err_t err = i2s_channel_read(_rxHandle, testBuf, sizeof(testBuf), &bytesRead, 500);
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
  esp_err_t err = i2s_channel_read(_rxHandle, _rawBuffer,
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
