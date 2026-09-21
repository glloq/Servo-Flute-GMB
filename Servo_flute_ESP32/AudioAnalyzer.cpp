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
    _noiseProfileId(NOISE_AMBIENT), _noiseCaptureFinished(false),
    _rawSamplesSinceFrame(0), _clippedSinceFrame(0),
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
  // Un flux qui (re)demarre n'herite RIEN du precedent : ni les verdicts de la
  // derniere frame, ni l'historique de brillance du detecteur de couac, ni
  // l'historique de pitch. Meme raison que le `_filters.configureDefaults()`
  // plus bas - sans quoi le premier bloc du nouveau flux serait melange a la
  // queue de l'ancien - et place AVANT l'installation du pilote pour que les
  // chemins d'ECHEC ci-dessous soient couverts eux aussi : un demarrage rate
  // ne doit pas laisser publier l'etat acoustique d'avant.
  markMeasurementInvalid();
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
  // Plus aucune frame n'arrivera. update() sort desormais immediatement, donc
  // le plafond d'obsolescence ne s'appliquera JAMAIS : sans cette invalidation
  // explicite, un diagnostic ouvert apres l'arret afficherait indefiniment
  // l'etat acoustique et la note de qualite de la derniere frame analysee.
  markMeasurementInvalid();
}

bool AudioAnalyzer::resetMicrophone() {
  // Re-probe without rebooting: tear the driver down and bring it back up.
  if (_initialized) uninstallI2S();
  _initialized = false;
  _micDetected = false;
  _frameSeq = 0;
  _frameTimestamp = 0;
  // INVALIDATION EXPLICITE, et pas seulement par l'intermediaire de begin().
  // Deux raisons distinctes :
  //   - `_frameTimestamp = 0` ci-dessus DESARME le plafond d'obsolescence de
  //     update() (qui ne s'applique que si l'horodatage est non nul) : si le
  //     microphone ne revient pas, plus rien n'invaliderait jamais le dernier
  //     verdict, qui resterait publie comme s'il venait d'etre mesure ;
  //   - begin() peut echouer avant d'y arriver (installation I2S refusee).
  // De plus, `_frameSeq` repart de zero : l'historique du detecteur de couac
  // compte les frames par leur sequence et n'y survivrait pas proprement.
  markMeasurementInvalid();
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

  // PHASE 6/7, une fois par frame analysee et APRES son identite : le
  // detecteur de couac mesure des durees en frames a partir de
  // `frameSequence`, et la chronometrie date ses instants avec `timestamp`.
  analyzeAcoustics();
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

  // PHASE 6. Les verdicts de la derniere frame reussie datent d'au moins
  // MIC_FRAME_STALE_MS : les laisser en place ferait lire un etat acoustique
  // et une note de qualite perimes comme s'ils decrivaient l'instant present -
  // le meme defaut que lire une platitude spectrale vieille de 64 ms en la
  // croyant fraiche. Un verdict perime n'est pas "un peu moins vrai", il est
  // faux.
  _classification = AcousticClassification();
  _quality = QualityScore();
  // L'historique de couac se compte en FRAMES, et _frameSeq n'avance que sur
  // les frames ANALYSEES : apres un trou de flux, la sequence reste contigue
  // alors que le temps, lui, a saute. Le detecteur ne peut donc pas voir ce
  // trou tout seul ; c'est ici qu'on le lui dit.
  _squeak.reset();
  // AcousticTiming n'est PAS touchee : son cycle est pilote par les ordres
  // d'actionneur, et elle possede ses propres plafonds (kOnsetTimeoutMs,
  // kReleaseTimeoutMs...) pour conclure quand le son n'arrive jamais. La
  // remettre a zero ici effacerait une note en cours de chronometrage sur un
  // simple trou d'acquisition.
}

void AudioAnalyzer::resetAcousticTracking() {
  // Changement de note, ou arret. L'historique de brillance decrit la note
  // PRECEDENTE : comparer la nouvelle a celle-la inventerait un couac au
  // premier instant de chaque note. Meme chose pour l'historique de pitch,
  // dont l'etendue traverserait les deux notes et les declarerait instables.
  _squeak.reset();
  _pitch.resetTracking();
  // resetTracking() efface aussi la note visee : on la restaure, car changer
  // de note ne veut pas dire qu'on ne vise plus rien.
  if (_expectedMidi > 0) _pitch.setExpectedMidiNote(_expectedMidi);
  // Les verdicts portaient sur la note precedente.
  _classification = AcousticClassification();
  _quality = QualityScore();
  // AcousticTiming n'est deliberement PAS remise a zero : son cycle commence a
  // noteCommanded() et se termine a noteReleased(), tous deux emis par la
  // chaine d'actionneurs. L'effacer ici depuis le chemin d'ANALYSE perdrait la
  // note en cours de mesure.
}

const char* AudioAnalyzer::getAcousticStateName() const {
  // `classified` faux signifie qu'AUCUN verdict n'a pu etre rendu : `state`
  // garde alors sa valeur par defaut (ACOUSTIC_SILENCE), qui se lirait comme
  // une mesure - "il n'y a pas de son" - alors qu'elle ne dit rien.
  if (!_classification.classified) return "unclassified";
  return AcousticQuality::stateName(_classification.state);
}

void AudioAnalyzer::setActive(bool active) {
  // TRANSITIONS SEULEMENT. Tous les appelants posent cet etat sur EVENEMENT
  // (bascule du moniteur, debut et fin de calibration, capture de bruit), mais
  // plusieurs le reposent a une valeur qu'il a deja ; agir sur la valeur plutot
  // que sur le front effacerait l'historique de pitch a chaque passage.
  if (_active == active) return;

  if (!active) {
    // Pause : update() sort des sa premiere ligne, donc le plafond
    // d'obsolescence ne s'appliquera JAMAIS. Sans invalidation ici, l'etat
    // acoustique et la note de qualite de la derniere frame resteraient
    // publies indefiniment, comme s'ils decrivaient l'instant present.
    markMeasurementInvalid();
  } else {
    // Reprise : ce que le detecteur de couac et l'historique de pitch avaient
    // appris decrit un AUTRE moment de jeu, separe par une pause de duree
    // inconnue. On repart vierge plutot que de comparer les frames qui
    // arrivent a une reference d'avant la pause.
    resetAcousticTracking();
  }
  _active = active;
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
    // La capture peut s'etre terminee d'elle-meme au plafond (voir NoiseModel).
    // Le drapeau est releve ici pour que l'appelant puisse arreter l'analyseur
    // plutot que de laisser tout le DSP tourner pour rien.
    if (!_noise.isCapturing()) _noiseCaptureFinished = true;
  }

  // Rapport signal/bruit contre le profil de l'etat REEL de la source d'air.
  const SnrResult snr = _noise.snrDb(_level.rms, _noiseProfileId);
  _features.snrValid = snr.valid;
  _features.snrUsedFallback = snr.usedFallback;
  _features.snrDb = snr.valid ? snr.db : 0.0f;
  _features.noiseProfile = (uint8_t)_noiseProfileId;
}

/*----------------------------------------------------------------------------
 * PHASE 6 et 7 - classification, note de qualite, chronometrie
 *
 * UN CONTEXTE HONNETE, CHAMP PAR CHAMP
 * ------------------------------------
 * AcousticQuality ne devine rien de ce qu'AcousticFeatures ne porte pas : il
 * demande a l'appelant de le DIRE. Cette fonction est donc le seul endroit du
 * firmware qui sait, pour une frame donnee, ce qui a reellement ete mesure sur
 * elle - et le seul endroit ou l'on peut mentir sans que rien ne le detecte.
 * Chaque champ ci-dessous est pris a sa source, jamais reconstruit.
 *
 * COUT, par frame analysee (62,5 par seconde)
 * -------------------------------------------
 * huit Goertzel de 1024 points pour la respiration (~8 % de YIN), deux pour
 * l'overblow (~2 %), 2n operations pour la brillance (~2 %), le reste etant de
 * la comparaison de seuils. Ce sont des COMPTES D'OPERATIONS, pas des mesures
 * sur materiel : rien n'a jamais tourne sur un ESP32 dans ce projet.
 *--------------------------------------------------------------------------*/

void AudioAnalyzer::analyzeAcoustics() {
  AcousticContext ctx;
  ctx.expectedMidi = _expectedMidi;

  // FRAICHEUR DE LA FFT. `fftValid` est remis a faux en tete de chaque
  // fillSpectral() et n'est releve que lorsque computeSpectrum() a REELLEMENT
  // tourne sur cette frame-ci. analyzeSpectrum(), lui, ne lance la FFT qu'une
  // frame sur MIC_SPECTRAL_DECIMATION, soit toutes les 64 ms. Annoncer un
  // contexte frais a chaque frame ferait juger la respiration - et, faute de
  // PCM, la brillance - sur un centroide et une platitude vieux de trois
  // frames. C'est le drapeau du constructeur de descripteurs qui fait foi,
  // jamais une hypothese locale sur la decimation.
  ctx.fftFresh = _features.fftValid;

  // HISTORIQUE DE PITCH. `pitchStability` vaut 0 tant que l'historique n'est
  // pas rempli ET 0 pour une note franchement instable : les deux cas sont
  // indiscernables dans le champ lui-meme. `stabilityValid` vient de
  // PitchResult::stabilityValid, propage tel quel par fillPitch() ; sans lui,
  // chaque debut de note serait classe ACOUSTIC_UNSTABLE.
  ctx.stabilityMeasured = _features.stabilityValid;

  // PCM DE LA FRAME COURANTE. Indispensable, et pas seulement utile :
  // evaluateOverblow mesure la puissance a la note VISEE et a son octave,
  // alors que les harmoniques deja rangees dans _features sont ancrees sur la
  // frequence DETECTEE - c'est-a-dire sur l'octave elle-meme pendant un
  // overblow, ou le critere ne verrait donc plus rien. Sans ce pointeur,
  // OverblowResult::valid reste faux et l'etat ACOUSTIC_OVERBLOW est
  // inatteignable. _frame contient encore la frame que analyzeFrame() vient de
  // traiter : ni detect() ni les mesures spectrales ne le modifient.
  ctx.frame = _frame;
  ctx.frameSize = MIC_ANALYSIS_FRAME_SIZE;
  ctx.sampleRate = (float)MIC_SAMPLE_RATE;

  // classify() fait AVANCER la machine a couac a chaque frame - c'est son
  // historique qui distingue un accident bref d'un defaut installe - et range
  // dans son resultat la respiration qu'elle a calculee au passage.
  _classification = AcousticQuality::classify(_features, ctx, &_squeak);

  // La respiration est REPRISE de la classification, pas recalculee :
  // computeBreathiness est une fonction pure, la rappeler avec les memes
  // entrees rendrait exactement le meme resultat pour huit Goertzel de plus
  // par frame. getBreathiness() expose cette mesure-la.
  //
  // QUALITE D'ATTAQUE : la sentinelle, volontairement. La PHASE 7 mesure des
  // DUREES en millisecondes ; la note de qualite attend une valeur 0..1. Aucun
  // seuil de ce projet ne dit quelle duree d'attaque vaut 1, et en inventer un
  // ici ferait passer un reglage de gout pour une mesure. La composante reste
  // donc ABSENTE et QualityScore::weightUsed le dit - c'est exactement a quoi
  // sert ce champ.
  _quality = AcousticQuality::computeAcousticQuality(_features, ctx,
                                                     _classification.breathiness,
                                                     AQ_ATTACK_NOT_MEASURED);

  // PHASE 7. Flux a SENS UNIQUE : l'analyse alimente la chronometrie, et rien
  // ici ne lit ses verdicts pour decider quoi que ce soit. Les instants
  // d'ORDRE (noteCommanded, airCommanded, valveOpened, noteReleased) lui
  // viennent de la chaine d'actionneurs, jamais de l'analyse.
  _timing.update(AcousticTiming::fromFeatures(_features));
}

void AudioAnalyzer::setAirSourceState(uint8_t airMode, uint8_t pumpPercent,
                                      uint8_t fanPercent) {
  _noiseProfileId = NoiseModel::profileForState(airMode, pumpPercent, fanPercent);
}

void AudioAnalyzer::beginNoiseCapture() {
  _noiseCaptureFinished = false;
  _noise.beginCapture(_noiseProfileId);
}

bool AudioAnalyzer::endNoiseCapture() {
  // Une capture qui s'est deja terminee au plafond a range son profil : on le
  // rapporte comme un succes plutot que comme un echec trompeur.
  if (_noiseCaptureFinished && !_noise.isCapturing()) {
    _noiseCaptureFinished = false;
    return _noise.hasProfile(_noiseProfileId);
  }
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
