#include "AudioAnalyzer.h"

#if MIC_ENABLED

#include <math.h>
#include "PitchMath.h"

AudioAnalyzer::AudioAnalyzer()
  : _active(false), _initialized(false), _micDetected(false),
    _soundDetected(false), _micStatus(MIC_STATUS_NOT_INIT), _rms(0),
    _pitchHz(0), _pitchMidi(0), _pitchCents(0), _pitchConfidence(0), _pitchValid(false),
    _frameSeq(0), _frameTimestamp(0),
#if MIC_I2S_STD_DRIVER
    _rxHandle(NULL),
#endif
    _validSamples(0), _lastUpdate(0) {
}

// ------------------------------------------------------------- I2S lifecycle --

bool AudioAnalyzer::installI2S() {
#if MIC_I2S_STD_DRIVER
  // ESP-IDF 5.x "std" I2S driver (avoids the legacy ADC driver conflict).
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = MIC_DMA_BUF_COUNT;
  chan_cfg.dma_frame_num = MIC_DMA_BUF_LEN;
  if (i2s_new_channel(&chan_cfg, NULL, &_rxHandle) != ESP_OK) return false;

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.bclk = (gpio_num_t)MIC_PIN_BCLK;
  std_cfg.gpio_cfg.ws = (gpio_num_t)MIC_PIN_LRCLK;
  std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.din = (gpio_num_t)MIC_PIN_DIN;
  std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.ws_inv = false;

  if (i2s_channel_init_std_mode(_rxHandle, &std_cfg) != ESP_OK) {
    i2s_del_channel(_rxHandle); _rxHandle = NULL; return false;
  }
  if (i2s_channel_enable(_rxHandle) != ESP_OK) {
    i2s_del_channel(_rxHandle); _rxHandle = NULL; return false;
  }
  return true;
#else
  // Legacy ESP-IDF 4.x I2S driver (Arduino-ESP32 2.0.x). INMP441 is an external
  // I2S mic (not internal-ADC mode), so this does not touch the ADC driver.
  i2s_config_t i2s_config = {};
  i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  i2s_config.sample_rate = MIC_SAMPLE_RATE;
  i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_config.intr_alloc_flags = 0;
  i2s_config.dma_buf_count = MIC_DMA_BUF_COUNT;
  i2s_config.dma_buf_len = MIC_DMA_BUF_LEN;
  i2s_config.use_apll = false;
  i2s_config.tx_desc_auto_clear = false;
  i2s_config.fixed_mclk = 0;

  if (i2s_driver_install(MIC_I2S_PORT, &i2s_config, 0, NULL) != ESP_OK) return false;

  i2s_pin_config_t pin_config = {};
  pin_config.bck_io_num = MIC_PIN_BCLK;
  pin_config.ws_io_num = MIC_PIN_LRCLK;
  pin_config.data_out_num = I2S_PIN_NO_CHANGE;
  pin_config.data_in_num = MIC_PIN_DIN;
  if (i2s_set_pin(MIC_I2S_PORT, &pin_config) != ESP_OK) {
    i2s_driver_uninstall(MIC_I2S_PORT); return false;
  }
  return true;
#endif
}

void AudioAnalyzer::uninstallI2S() {
#if MIC_I2S_STD_DRIVER
  if (_rxHandle) {
    i2s_channel_disable(_rxHandle);
    i2s_del_channel(_rxHandle);
    _rxHandle = NULL;
  }
#else
  i2s_driver_uninstall(MIC_I2S_PORT);
#endif
}

bool AudioAnalyzer::begin() {
  if (!installI2S()) {
    if (DEBUG) Serial.println("ERREUR: AudioAnalyzer - I2S install failed");
    _initialized = false;
    _micStatus = MIC_STATUS_READ_ERROR;
    return false;
  }
  delay(100);  // let I2S stabilise (startup only, not in the state machine)
  _initialized = true;
  _micDetected = detectMicrophone();

  if (DEBUG) {
    Serial.print("DEBUG: AudioAnalyzer - Mic status: ");
    Serial.println(getMicStatusString());
  }

  if (!_micDetected) {
    uninstallI2S();
    _initialized = false;
  }
  return _micDetected;
}

void AudioAnalyzer::end() {
  if (_initialized) {
    uninstallI2S();
    _initialized = false;
  }
  _active = false;
}

bool AudioAnalyzer::resetMicrophone() {
  // Re-probe without rebooting: tear the driver down and bring it back up.
  if (_initialized) uninstallI2S();
  _initialized = false;
  _micDetected = false;
  _frameSeq = 0;
  _frameTimestamp = 0;
  bool ok = begin();
  return ok;
}

bool AudioAnalyzer::detectMicrophone() {
  size_t bytesRead = 0;
  int32_t testBuf[256];
#if MIC_I2S_STD_DRIVER
  esp_err_t err = i2s_channel_read(_rxHandle, testBuf, sizeof(testBuf), &bytesRead, 500);
#else
  esp_err_t err = i2s_read(MIC_I2S_PORT, testBuf, sizeof(testBuf), &bytesRead, 500 / portTICK_PERIOD_MS);
#endif
  if (err != ESP_OK || bytesRead == 0) {
    _micStatus = MIC_STATUS_READ_ERROR;
    return false;
  }
  size_t samples = bytesRead / sizeof(int32_t);
  // Robust classification (non-zero + variance + not stuck + not saturated).
  MicSignalClass cls = PitchDetector::classifyRaw(testBuf, samples);
  switch (cls) {
    case MIC_SIG_OK:        _micStatus = MIC_STATUS_DETECTED;  return true;
    case MIC_SIG_ALL_ZERO:  _micStatus = MIC_STATUS_ALL_ZERO;  return false;
    case MIC_SIG_STUCK:     _micStatus = MIC_STATUS_STUCK;     return false;
    case MIC_SIG_SATURATED: _micStatus = MIC_STATUS_SATURATED; return false;
  }
  return false;
}

const char* AudioAnalyzer::getMicStatusString() const {
  switch (_micStatus) {
    case MIC_STATUS_DETECTED:  return "detected";
    case MIC_STATUS_ALL_ZERO:  return "all_zero";
    case MIC_STATUS_STUCK:     return "stuck";
    case MIC_STATUS_SATURATED: return "saturated";
    case MIC_STATUS_READ_ERROR:return "read_error";
    case MIC_STATUS_NOT_INIT:  return "not_init";
  }
  return "?";
}

// --------------------------------------------------------------- processing --

void AudioAnalyzer::update() {
  if (!_initialized || !_active) return;

  unsigned long now = millis();
  if (now - _lastUpdate < AUTOCAL_FRAME_SAMPLE_MS) return;  // throttle analysis rate
  _lastUpdate = now;

  readI2S();
  if (_validSamples > 0) {
    analyzeBuffer();
    _frameSeq++;
    _frameTimestamp = now;
  } else if (_frameTimestamp != 0 && (now - _frameTimestamp) > MIC_FRAME_STALE_MS) {
    // No fresh I2S data for too long: never let stale pitch data look valid.
    _pitchValid = false;
    _pitchHz = 0;
    _pitchMidi = 0;
    _pitchConfidence = 0;
    _soundDetected = false;
  }
}

void AudioAnalyzer::readI2S() {
  size_t bytesRead = 0;
#if MIC_I2S_STD_DRIVER
  esp_err_t err = i2s_channel_read(_rxHandle, _rawBuffer,
                                   MIC_BUFFER_SIZE * sizeof(int32_t), &bytesRead, 0);
#else
  esp_err_t err = i2s_read(MIC_I2S_PORT, _rawBuffer,
                           MIC_BUFFER_SIZE * sizeof(int32_t), &bytesRead, 0);
#endif
  if (err != ESP_OK || bytesRead == 0) {
    _validSamples = 0;
    return;
  }
  _validSamples = bytesRead / sizeof(int32_t);

  // Normalize 24-bit-in-32-bit samples to -1..+1.
  const float scale = 1.0f / 2147483648.0f;
  for (size_t i = 0; i < _validSamples; i++) {
    _analysisBuffer[i] = (float)_rawBuffer[i] * scale;
  }
}

void AudioAnalyzer::analyzeBuffer() {
  // RMS on the DC-removed samples (PitchDetector::rms does not modify the buffer).
  _rms = PitchDetector::rms(_analysisBuffer, _validSamples);
  _soundDetected = (_rms > MIC_RMS_THRESHOLD);

  _pitchHz = 0; _pitchMidi = 0; _pitchCents = 0; _pitchConfidence = 0; _pitchValid = false;

  if (_rms > MIC_RMS_ABSOLUTE_MIN) {
    // detect() centres + windows _analysisBuffer in place (not reused afterwards).
    PitchResult pr = _pitch.detect(_analysisBuffer, _validSamples);
    if (pr.hz > 0.0f) {
      _pitchHz = pr.hz;
      _pitchConfidence = pr.confidence;
      _pitchMidi = PitchMath::hzToMidi(pr.hz);
      _pitchCents = PitchMath::hzToCents(pr.hz, _pitchMidi);
      _pitchValid = pr.valid;
    }
  }
}

#endif // MIC_ENABLED
