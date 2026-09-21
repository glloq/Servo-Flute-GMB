#include "NoiseModel.h"

#include <math.h>
#include "AudioLevel.h"

namespace {

// Bornes des bandes d'analyse, en Hz. Espacement approximativement logarithmique
// sur la plage utile de l'instrument : les bandes basses isolent le bruit
// mecanique lent (ventilateur, vibration du chassis), les hautes le souffle.
const float kBandEdges[MIC_NOISE_BANDS + 1] = {
    100.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f};

static_assert(MIC_NOISE_BANDS == 6, "kBandEdges doit suivre MIC_NOISE_BANDS");

// Moyenne incrementale : m_n = m_{n-1} + (x - m_{n-1}) / n. Evite d'accumuler
// une somme qui deborderait, et donne une moyenne exacte a chaque etape.
inline void accumulateMean(float& mean, float sample, uint16_t count) {
  mean += (sample - mean) / (float)count;
}

}  // namespace

void NoiseModel::reset() {
  for (uint8_t i = 0; i < NOISE_PROFILE_COUNT; i++) _profiles[i] = NoiseProfile();
  _accum = NoiseProfile();
  _capturing = false;
  _captureId = NOISE_AMBIENT;
}

void NoiseModel::beginCapture(NoiseProfileId id) {
  if (id >= NOISE_PROFILE_COUNT) return;
  _captureId = id;
  _accum = NoiseProfile();
  _capturing = true;
}

void NoiseModel::accumulate(const float* frame, size_t n, const SpectralAnalyzer* spectral) {
  if (!_capturing || frame == nullptr || n == 0) return;
  if (_accum.frames >= MIC_NOISE_MAX_FRAMES) return;   // borne la duree d'une capture

  _accum.frames++;

  const FrameLevel level = AudioLevel::compute(frame, n);
  accumulateMean(_accum.rms, level.rms, _accum.frames);

#if MIC_FFT_ENABLED
  // Les energies par bande demandent un spectre. Sans FFT, elles restent a zero
  // et le profil ne porte qu'un niveau : c'est moins riche, mais ce n'est pas
  // faux.
  if (spectral != nullptr && spectral->hasSpectrum()) {
    for (uint8_t b = 0; b < MIC_NOISE_BANDS; b++) {
      const float e = spectral->bandEnergy(kBandEdges[b], kBandEdges[b + 1]);
      accumulateMean(_accum.bands[b], e, _accum.frames);
    }
    accumulateMean(_accum.flatness, spectral->spectralFlatness(), _accum.frames);

    // Pic dominant : utile pour reconnaitre une raie mecanique (harmonique de
    // rotation d'un ventilateur, par exemple) plutot qu'un bruit large bande.
    const float* mag = spectral->magnitudes();
    size_t peak = 1;
    for (size_t k = 2; k < spectral->binCount(); k++) {
      if (mag[k] > mag[peak]) peak = k;
    }
    accumulateMean(_accum.peakMag, mag[peak], _accum.frames);
    accumulateMean(_accum.peakHz, SpectralAnalyzer::binToHz(peak), _accum.frames);
  }
#else
  (void)spectral;
#endif
}

bool NoiseModel::endCapture() {
  if (!_capturing) return false;
  _capturing = false;

  // Une capture trop courte ne decrit rien : elle est rejetee plutot que
  // rangee comme un profil de confiance douteuse.
  if (_accum.frames < MIC_NOISE_MIN_FRAMES) return false;

  _accum.rmsDbFS = AudioLevel::toDbFS(_accum.rms);
  _accum.valid = true;
  _profiles[_captureId] = _accum;
  return true;
}

const NoiseProfile& NoiseModel::profile(NoiseProfileId id) const {
  static const NoiseProfile kEmpty;
  if (id >= NOISE_PROFILE_COUNT) return kEmpty;
  return _profiles[id];
}

bool NoiseModel::hasProfile(NoiseProfileId id) const {
  return id < NOISE_PROFILE_COUNT && _profiles[id].valid;
}

uint8_t NoiseModel::capturedCount() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < NOISE_PROFILE_COUNT; i++) {
    if (_profiles[i].valid) n++;
  }
  return n;
}

SnrResult NoiseModel::snrDb(float signalRms, NoiseProfileId id) const {
  SnrResult out;
  if (!(signalRms > 0.0f) || id >= NOISE_PROFILE_COUNT) return out;

  NoiseProfileId source = id;
  if (!_profiles[source].valid) {
    // Repli EXPLICITE vers l'ambiance : le rapport reste calculable, mais
    // l'appelant sait qu'il compare a un autre etat que celui demande, donc
    // qu'il surestime probablement la qualite.
    if (!_profiles[NOISE_AMBIENT].valid) return out;   // rien de mesure : on le dit
    source = NOISE_AMBIENT;
    out.usedFallback = true;
  }

  const float noiseRms = _profiles[source].rms;
  if (!(noiseRms > 0.0f)) {
    // Un plancher mesure a exactement zero est un artefact de signal
    // synthetique, pas une mesure physique. On rend le maximum borne plutot
    // qu'une division par zero.
    out.valid = true;
    out.source = source;
    out.db = MIC_SNR_MAX_DB;
    return out;
  }

  float db = 20.0f * log10f(signalRms / noiseRms);
  if (db < 0.0f) db = 0.0f;                      // le signal est sous le bruit
  if (db > MIC_SNR_MAX_DB) db = MIC_SNR_MAX_DB;
  out.valid = true;
  out.db = db;
  out.source = source;
  return out;
}

NoiseProfileId NoiseModel::profileForState(uint8_t airMode, uint8_t pumpPercent,
                                           uint8_t fanPercent) {
  // Seuls les modes a source d'air ACTIVE produisent un bruit de machinerie
  // significatif. Les modes passifs se comparent a l'ambiance.
  if (airMode == AIR_MODE_FAN_SERVO) {
    if (fanPercent > MIC_NOISE_HIGH_PERCENT) return NOISE_FAN_HIGH;
    if (fanPercent > MIC_NOISE_IDLE_PERCENT) return NOISE_FAN_MEDIUM;
    if (fanPercent > 0) return NOISE_FAN_IDLE;
    return NOISE_AMBIENT;
  }
  if (airMode == AIR_MODE_PUMP_VALVE || airMode == AIR_MODE_PUMP_RESERVOIR) {
    if (pumpPercent > MIC_NOISE_HIGH_PERCENT) return NOISE_PUMP_HIGH;
    if (pumpPercent > MIC_NOISE_IDLE_PERCENT) return NOISE_PUMP_MEDIUM;
    if (pumpPercent > 0) return NOISE_PUMP_IDLE;
    return NOISE_AMBIENT;
  }
  return NOISE_AMBIENT;
}

float NoiseModel::bandLowHz(uint8_t index) {
  return (index < MIC_NOISE_BANDS) ? kBandEdges[index] : 0.0f;
}

float NoiseModel::bandHighHz(uint8_t index) {
  return (index < MIC_NOISE_BANDS) ? kBandEdges[index + 1] : 0.0f;
}

const char* NoiseModel::profileName(NoiseProfileId id) {
  switch (id) {
    case NOISE_AMBIENT:     return "ambient";
    case NOISE_PUMP_IDLE:   return "pump_idle";
    case NOISE_PUMP_MEDIUM: return "pump_medium";
    case NOISE_PUMP_HIGH:   return "pump_high";
    case NOISE_FAN_IDLE:    return "fan_idle";
    case NOISE_FAN_MEDIUM:  return "fan_medium";
    case NOISE_FAN_HIGH:    return "fan_high";
    default:                return "?";
  }
}
