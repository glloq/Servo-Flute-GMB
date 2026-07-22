#include "AudioAnalyzer.h"

#if MIC_ENABLED

#include <math.h>
#include "PitchMath.h"

AudioAnalyzer::AudioAnalyzer()
  : _active(false), _initialized(false), _micDetected(false),
    _soundDetected(false), _rms(0), _pitchHz(0), _pitchMidi(0),
    _pitchCents(0), _pitchConfidence(0), _pitchValid(false),
    _rxHandle(NULL), _validSamples(0), _lastUpdate(0) {
  buildHannWindow();
}

void AudioAnalyzer::buildHannWindow() {
  // Hann window: w[n] = 0.5 * (1 - cos(2*pi*n/(N-1))).
  // Precomputed once so update() performs no per-frame trig or allocation.
  const int N = MIC_BUFFER_SIZE;
  for (int n = 0; n < N; n++) {
    _hannWindow[n] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)n / (float)(N - 1)));
  }
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
  if (now - _lastUpdate < AUTOCAL_FRAME_SAMPLE_MS) return;  // throttle analysis rate
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

void AudioAnalyzer::removeDcOffset() {
  // The INMP441 carries a small DC bias; it must be removed before RMS and pitch
  // detection so the level and the difference function are not skewed.
  if (_validSamples == 0) return;
  float mean = 0.0f;
  for (size_t i = 0; i < _validSamples; i++) mean += _analysisBuffer[i];
  mean /= (float)_validSamples;
  for (size_t i = 0; i < _validSamples; i++) _analysisBuffer[i] -= mean;
}

float AudioAnalyzer::computeRMS() {
  float sum = 0;
  for (size_t i = 0; i < _validSamples; i++) {
    sum += _analysisBuffer[i] * _analysisBuffer[i];
  }
  return sqrtf(sum / (float)_validSamples);
}

void AudioAnalyzer::applyHannWindow() {
  // Applied only for pitch detection (after RMS) to limit spectral/edge leakage.
  for (size_t i = 0; i < _validSamples; i++) {
    _analysisBuffer[i] *= _hannWindow[i];
  }
}

void AudioAnalyzer::analyzeBuffer() {
  // 1. Remove DC bias so both RMS and pitch see a zero-mean signal.
  removeDcOffset();

  // 2. RMS on the DC-removed (un-windowed) buffer.
  _rms = computeRMS();
  _soundDetected = (_rms > MIC_RMS_THRESHOLD);

  // 3. Pitch detection: Hann-window the buffer, then run YIN.
  _pitchHz = 0;
  _pitchMidi = 0;
  _pitchCents = 0;
  _pitchConfidence = 0;
  _pitchValid = false;

  if (_rms > MIC_RMS_ABSOLUTE_MIN) {
    applyHannWindow();
    float hz = 0.0f, conf = 0.0f;
    bool ok = computePitchYIN(hz, conf);
    if (ok && hz > 0.0f) {
      _pitchHz = hz;
      _pitchConfidence = conf;
      _pitchMidi = PitchMath::hzToMidi(hz);
      _pitchCents = PitchMath::hzToCents(hz, _pitchMidi);
      _pitchValid = (conf >= MIC_YIN_CONFIDENCE_MIN);
    }
  }
}

bool AudioAnalyzer::computePitchYIN(float& outHz, float& outConfidence) {
  outHz = 0.0f;
  outConfidence = 0.0f;

  // Lag bounds from the configured pitch range, clamped to the buffer size.
  int tauMin = (int)(MIC_SAMPLE_RATE / MIC_PITCH_MAX_HZ);
  int tauMax = (int)(MIC_SAMPLE_RATE / MIC_PITCH_MIN_HZ);
  if (tauMin < 1) tauMin = 1;
  if (tauMax > MIC_YIN_TAU_MAX) tauMax = MIC_YIN_TAU_MAX;

  const int W = (int)_validSamples / 2;  // Window (integration) size
  if (W < tauMax + 1) return false;      // not enough samples for the largest lag
  if (tauMin >= tauMax) return false;

  // Steps 1-2: difference function + cumulative mean normalization.
  _yinBuf[0] = 1.0f;
  float runningSum = 0.0f;
  for (int tau = 1; tau <= tauMax; tau++) {
    float sum = 0.0f;
    for (int i = 0; i < W; i++) {
      float delta = _analysisBuffer[i] - _analysisBuffer[i + tau];
      sum += delta * delta;
    }
    runningSum += sum;
    _yinBuf[tau] = (runningSum > 0.0f) ? (sum * (float)tau / runningSum) : 1.0f;
  }

  // Step 3: absolute threshold - first dip below MIC_YIN_THRESHOLD.
  int tauEst = -1;
  for (int tau = tauMin; tau < tauMax; tau++) {
    if (_yinBuf[tau] < MIC_YIN_THRESHOLD) {
      // Walk down to the local minimum of this dip.
      while (tau + 1 < tauMax && _yinBuf[tau + 1] < _yinBuf[tau]) tau++;
      tauEst = tau;
      break;
    }
  }
  if (tauEst < 0) return false;

  // Step 3b: conservative octave-error guard. The classic YIN failure is locking
  // onto a half period (an octave too high) when a subharmonic dip crosses the
  // threshold before the true period. We only correct to twice the lag (the octave
  // below) when that dip is *dramatically* deeper - a genuine half-period artifact.
  // A loose ratio here would instead force octave-DOWN errors on harmonic-rich
  // tones (where d(2*tau) is naturally comparable to d(tau)), so the ratio is kept
  // strict. Octave-up detections that slip through are still rejected at the
  // calibration level by exact-MIDI matching (treated as overblow).
  int tauDouble = tauEst * 2;
  if (tauDouble <= tauMax) {
    int lo = tauDouble - 1, hi = tauDouble + 1;
    if (lo < tauMin) lo = tauMin;
    if (hi > tauMax) hi = tauMax;
    int bestD = lo;
    for (int t = lo + 1; t <= hi; t++) if (_yinBuf[t] < _yinBuf[bestD]) bestD = t;
    if (_yinBuf[bestD] < MIC_YIN_OCTAVE_RATIO * _yinBuf[tauEst]) {
      tauEst = bestD;  // clearly more periodic an octave below -> genuine half-period error
    }
  }

  // Step 4: parabolic interpolation for sub-sample accuracy.
  float betterTau = (float)tauEst;
  if (tauEst > tauMin && tauEst < tauMax) {
    float s0 = _yinBuf[tauEst - 1];
    float s1 = _yinBuf[tauEst];
    float s2 = _yinBuf[tauEst + 1];
    float denom = 2.0f * (2.0f * s1 - s2 - s0);
    if (fabsf(denom) > 1e-6f) {
      betterTau = (float)tauEst + (s0 - s2) / denom;
    }
  }
  if (betterTau < 1.0f) return false;

  // Confidence = periodicity = 1 - aperiodicity at the estimated lag.
  float aperiodicity = _yinBuf[tauEst];
  if (aperiodicity < 0.0f) aperiodicity = 0.0f;
  if (aperiodicity > 1.0f) aperiodicity = 1.0f;
  outConfidence = 1.0f - aperiodicity;

  float hz = (float)MIC_SAMPLE_RATE / betterTau;
  if (hz < MIC_PITCH_MIN_HZ || hz > MIC_PITCH_MAX_HZ) return false;
  outHz = hz;
  return true;
}

#endif // MIC_ENABLED
