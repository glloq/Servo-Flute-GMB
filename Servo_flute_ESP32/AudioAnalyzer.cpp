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
    _noiseProfileId(NOISE_AMBIENT), _rawSamplesSinceFrame(0), _clippedSinceFrame(0),
    _expectedMidi(0), _spectralCountdown(0),
    _lastDrain(0) {
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
  _ring.reset();
  _stats.reset();
  _level = FrameLevel();
  // La memoire des filtres doit repartir vierge : sans cela le premier bloc du
  // nouveau flux serait melange a la queue de l'ancien.
  _filters.configureDefaults();
  _rawSamplesSinceFrame = 0;
  _clippedSinceFrame = 0;
  _lastDrain = 0;
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

  const unsigned long now = millis();

  // 1. VIDER LE DMA, souvent. Le DMA ne contient que
  //    MIC_DMA_BUF_COUNT * MIC_DMA_BUF_LEN echantillons (32 ms ici). L'ancienne
  //    version ne le lisait que toutes les 40 ms : 8 ms etaient ecrases a chaque
  //    cycle (defaut A0-2). On vide desormais toutes les MIC_DRAIN_INTERVAL_MS,
  //    ce qui laisse une marge de 4x, et vers un anneau qui conserve le passe
  //    recent meme si loop() prend du retard.
  if (now - _lastDrain >= MIC_DRAIN_INTERVAL_MS) {
    _lastDrain = now;
    drainI2S();
  }

  // 2. N'ANALYSER QU'UNE FRAME COMPLETE, et UNE SEULE par passage.
  //    Aucun minuteur ici, volontairement : le debit s'auto-regule sur le hop.
  //    Chaque analyse avance la lecture de MIC_ANALYSIS_HOP_SIZE echantillons,
  //    donc la cadence d'equilibre vaut exactement hop / Fe (16 ms a 32 kHz avec
  //    un hop de 512). Un minuteur plus LENT que cela ferait deborder l'anneau en
  //    permanence - la production etant fixee par le materiel - et un minuteur
  //    plus rapide ne servirait a rien puisque la frame ne serait pas prete.
  //    Une seule frame par passage borne le temps passe dans update().
  if (!_ring.readFrame(_frame, MIC_ANALYSIS_FRAME_SIZE, MIC_ANALYSIS_HOP_SIZE, &_stats)) {
    // Pas assez d'echantillons : c'est normal entre deux frames. On ne signale
    // l'obsolescence que si plus rien n'arrive depuis longtemps.
    if (_frameTimestamp != 0 && (now - _frameTimestamp) > MIC_FRAME_STALE_MS) {
      markMeasurementInvalid();
    }
    return;
  }

  analyzeFrame();
  _frameSeq++;
  _frameTimestamp = now;
  _stats.lastFrameTimestamp = now;
  // Renseigne APRES l'increment : l'identite de la frame ne depend pas de
  // l'ordre des appels a l'interieur d'analyzeFrame().
  _features.frameSequence = _frameSeq;
  _features.timestamp = (uint32_t)now;
}

void AudioAnalyzer::markMeasurementInvalid() {
  // Aucune donnee fraiche depuis trop longtemps : ne jamais laisser une mesure
  // ancienne passer pour valide.
  _pitchValid = false;
  _pitchHz = 0;
  _pitchMidi = 0;
  _pitchConfidence = 0;
  _soundDetected = false;
  _lastPitch = PitchResult();
  _pitch.resetTracking();
  // resetTracking() efface aussi la note visee : on la restaure, car une source
  // momentanement muette ne signifie pas que la calibration a change de note.
  if (_expectedMidi > 0) _pitch.setExpectedMidiNote(_expectedMidi);
  // Une capture de bruit en cours devient sans objet : elle accumulerait des
  // frames qui ne viennent plus du microphone.
  _noise.abortCapture();
  _features.reset();
}

void AudioAnalyzer::setExpectedMidiNote(int midi) {
  _expectedMidi = (midi > 0 && midi <= 127) ? midi : 0;
  _pitch.setExpectedMidiNote(midi);
}

void AudioAnalyzer::clearExpectedMidiNote() {
  _expectedMidi = 0;
  _pitch.clearExpectedMidiNote();
}

void AudioAnalyzer::drainI2S() {
  // Boucle bornee : on vide au plus la profondeur du DMA en un passage, pour ne
  // jamais monopoliser loop() si la source produit plus vite que prevu.
  const int kMaxChunks =
      (MIC_DMA_BUF_COUNT * MIC_DMA_BUF_LEN + MIC_I2S_CHUNK_SAMPLES - 1) / MIC_I2S_CHUNK_SAMPLES + 1;
  const float kScale = 1.0f / 2147483648.0f;   // 24 bits cales a gauche dans 32

  for (int c = 0; c < kMaxChunks; c++) {
    size_t bytesRead = 0;
#if MIC_I2S_STD_DRIVER
    esp_err_t err = i2s_channel_read(_rxHandle, _chunk,
                                     MIC_I2S_CHUNK_SAMPLES * sizeof(int32_t), &bytesRead, 0);
#else
    esp_err_t err = i2s_read(MIC_I2S_PORT, _chunk,
                             MIC_I2S_CHUNK_SAMPLES * sizeof(int32_t), &bytesRead, 0);
#endif
    if (err != ESP_OK) {
      _stats.readErrors++;
      return;
    }
    if (bytesRead == 0) return;   // DMA vide : normal, on a tout pris

    const size_t samples = bytesRead / sizeof(int32_t);
    // Une lecture plus courte que demandee est COMPTEE mais n'est plus un
    // probleme : les echantillons vont dans l'anneau, et la frame sera
    // assemblee quand il y en aura assez.
    if (samples < MIC_I2S_CHUNK_SAMPLES) _stats.partialReads++;

    for (size_t i = 0; i < samples; i++) _chunkFloat[i] = (float)_chunk[i] * kScale;

    // Ecretage mesure ICI, sur le signal BRUT : c'est le convertisseur qui
    // sature. Apres le passe-haut un echantillon au rail peut repasser sous le
    // seuil, et l'ecretage deviendrait invisible exactement quand il compte.
    _clippedSinceFrame += (uint32_t)AudioLevel::countClipped(_chunkFloat, samples);
    _rawSamplesSinceFrame += (uint32_t)samples;

    // Filtrage du FLUX, avant l'anneau. Voir AudioFilters.h : filtrer frame par
    // frame ferait passer chaque echantillon deux fois dans le filtre, les
    // frames se recouvrant de 50 %.
    _filters.processBlock(_chunkFloat, samples);

    _ring.write(_chunkFloat, samples, &_stats);

    if (samples < MIC_I2S_CHUNK_SAMPLES) return;   // DMA epuise
  }
}

void AudioAnalyzer::analyzeFrame() {
  // Niveau complet : RMS lineaire (compatibilite IAudioSource), mais aussi
  // crete, dBFS, decalage continu et ecretage.
  _level = AudioLevel::compute(_frame, MIC_ANALYSIS_FRAME_SIZE);
  _rms = _level.rms;
  _soundDetected = (_rms > MIC_RMS_THRESHOLD);

  _pitchHz = 0; _pitchMidi = 0; _pitchCents = 0; _pitchConfidence = 0; _pitchValid = false;

  if (_rms > MIC_RMS_ABSOLUTE_MIN) {
    // detect() ne modifie PAS _frame : la meme frame reste disponible pour
    // l'analyse spectrale, sans recopie.
    _lastPitch = _pitch.detect(_frame, MIC_ANALYSIS_FRAME_SIZE);
    if (_lastPitch.hz > 0.0f) {
      _pitchHz = _lastPitch.hz;
      _pitchConfidence = _lastPitch.confidence;
      _pitchMidi = _lastPitch.midi;
      _pitchCents = _lastPitch.cents;
      _pitchValid = _lastPitch.valid;
    }
  } else {
    // Sous le plancher de niveau il n'y a rien a suivre : on vide l'historique
    // pour qu'une note ulterieure ne herite pas de la stabilite d'une autre.
    _lastPitch = PitchResult();
    _pitch.resetTracking();
    if (_expectedMidi > 0) _pitch.setExpectedMidiNote(_expectedMidi);
  }

  analyzeSpectrum();

  // --- Assemblage des descripteurs (PHASE 4) --------------------------------
  // L'assemblage lui-meme vit dans AcousticFeatureBuilder, qui est pur et donc
  // testable sur hote ; cette classe, elle, depend de l'I2S.
  AcousticFeatureBuilder::fillLevel(_features, _level);

  // L'ecretage vient du chemin BRUT, pas de la frame filtree. Le ratio porte
  // sur tous les echantillons recus depuis la frame precedente, donc un peu
  // plus que la frame elle-meme : c'est volontaire, cela couvre aussi ce qui
  // tombe entre deux frames.
  if (_rawSamplesSinceFrame > 0) {
    _features.clippingRatio = (float)_clippedSinceFrame / (float)_rawSamplesSinceFrame;
    _features.clipping = (_features.clippingRatio > MIC_CLIP_RATIO_WARN);
    _level.clippingRatio = _features.clippingRatio;
    _level.clippingDetected = _features.clipping;
  }
  _rawSamplesSinceFrame = 0;
  _clippedSinceFrame = 0;

  AcousticFeatureBuilder::fillPitch(_features, _lastPitch, _soundDetected);

  // Accumulation d'un profil de bruit : uniquement pendant une capture
  // explicite, et l'appelant garantit qu'aucune note ne sonne.
  if (_noise.isCapturing()) {
#if MIC_FFT_ENABLED
    // Le spectre est RECALCULE ici, et ce n'est pas une precaution inutile.
    // analyzeSpectrum() ne calcule rien sans fondamentale fiable - or pendant
    // une capture de bruit il n'y a par definition pas de note. Reutiliser
    // _spectral tel quel ferait accumuler, frame apres frame, le spectre laisse
    // par la DERNIERE note jouee : le profil decrirait cette note et non le
    // bruit. Mieux vaut payer une FFT par frame pendant les quelques dixiemes
    // de seconde d'une capture explicite.
    const bool fresh = _spectral.computeSpectrum(_frame, MIC_ANALYSIS_FRAME_SIZE);
    _noise.accumulate(_frame, MIC_ANALYSIS_FRAME_SIZE, fresh ? &_spectral : nullptr);
#else
    _noise.accumulate(_frame, MIC_ANALYSIS_FRAME_SIZE, nullptr);
#endif
  }

  // Rapport signal/bruit contre le profil de l'etat REEL de la source d'air.
  const SnrResult snr = _noise.snrDb(_level.rms, _noiseProfileId);
  _features.snrValid = snr.valid;
  _features.snrUsedFallback = snr.usedFallback;
  _features.snrDb = snr.valid ? snr.db : 0.0f;
  _features.noiseProfile = (uint8_t)_noiseProfileId;
}

void AudioAnalyzer::setAirSourceState(uint8_t airMode, uint8_t pumpPercent,
                                      uint8_t fanPercent) {
  _noiseProfileId = NoiseModel::profileForState(airMode, pumpPercent, fanPercent);
}

void AudioAnalyzer::beginNoiseCapture() {
  _noise.beginCapture(_noiseProfileId);
}

bool AudioAnalyzer::endNoiseCapture() {
  return _noise.endCapture();
}

void AudioAnalyzer::analyzeSpectrum() {
  if (!_lastPitch.valid || _lastPitch.hz <= 0.0f) {
    // Sans fondamentale fiable il n'y a pas d'harmoniques a mesurer.
    AcousticFeatureBuilder::fillSpectral(_features, nullptr, 0, 0.0f);
    _spectralCountdown = 0;   // repartir a neuf des qu'une note revient
    return;
  }

  // Goertzel a CHAQUE frame (il coute environ 4 % de YIN pour quatre
  // harmoniques), FFT une frame sur MIC_SPECTRAL_DECIMATION : le timbre evolue
  // bien plus lentement que le pitch et la FFT coute bien plus cher.
  bool runFft = false;
#if MIC_FFT_ENABLED
  if (_spectralCountdown == 0) {
    runFft = true;
    _spectralCountdown = MIC_SPECTRAL_DECIMATION;
  }
  _spectralCountdown--;
#endif

  AcousticFeatureBuilder::fillSpectral(_features, _frame, MIC_ANALYSIS_FRAME_SIZE,
                                       _lastPitch.hz, &_spectral, runFft);
}

#endif // MIC_ENABLED
