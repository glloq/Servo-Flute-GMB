#include "AcousticFeatures.h"

#include <math.h>

namespace AcousticFeatureBuilder {

void fillLevel(AcousticFeatures& f, const FrameLevel& level) {
  f.rms = level.rms;
  f.rmsDbFS = level.rmsDbFS;
  f.peakDbFS = level.peakDbFS;
  f.clippingRatio = level.clippingRatio;
  f.clipping = level.clippingDetected;
}

void fillPitch(AcousticFeatures& f, const PitchResult& p, bool soundDetected) {
  // Le verdict du detecteur est PROPAGE, et non laisse a reconstituer par chaque
  // consommateur a partir de la confiance : deux criteres reimplementes finissent
  // toujours par diverger.
  f.pitchValid = p.valid;
  f.stabilityValid = p.stabilityValid;
  f.pitchHz = p.hz;
  f.pitchMidi = (int16_t)p.midi;
  f.cents = p.cents;
  f.pitchConfidence = p.confidence;
  f.pitchStability = p.stability;
  f.soundDetected = soundDetected;
  // Ces deux verdicts n'ont de sens que si le pitch est FIABLE. Les deriver
  // d'une mesure rejetee reviendrait a annoncer un overblow sur du bruit.
  f.expectedNoteDetected = p.valid && p.expectedMatch;
  f.overblowDetected = p.valid && p.octaveAbove;
}

void fillSpectral(AcousticFeatures& f, const float* frame, size_t n, float f0,
                  SpectralAnalyzer* spectral, bool runFft) {
  f.spectralValid = false;
  f.fftValid = false;
  f.fundamentalEnergy = 0.0f;
  f.h2Ratio = 0.0f;
  f.h3Ratio = 0.0f;
  f.harmonicToNoiseRatio = 0.0f;

  if (frame == nullptr || n == 0 || f0 <= 0.0f) {
    // Rien de mesurable : les descripteurs de FORME de spectre sont effaces
    // aussi. Les laisser porter la valeur d'une note precedente ferait decrire
    // cette note a une frame qui n'en contient pas.
    f.spectralCentroid = 0.0f;
    f.spectralFlatness = 0.0f;
    return;
  }

  const HarmonicEnergies h = SpectralAnalyzer::harmonics(frame, n, f0);
  if (!h.valid) return;

  f.fundamentalEnergy = h.fundamental;
  f.h2Ratio = h.h2Ratio;
  f.h3Ratio = h.h3Ratio;

  // Rapport harmonique / non-harmonique, en dB. APPROXIMATION assumee :
  // l'energie dite "non harmonique" est tout ce que les quatre raies mesurees
  // ne captent pas, ce qui inclut le souffle mais aussi les harmoniques de rang
  // superieur. Un HNR rigoureux demanderait le spectre complet et une
  // estimation de plancher de bruit ; ce n'est pas ce qui est fourni ici.
  const float total = SpectralAnalyzer::totalPower(frame, n);
  // Goertzel rend la puissance d'une raie complexe (A^2/4), totalPower la
  // puissance moyenne du signal reel (A^2/2) : le facteur 2 aligne les unites.
  const float harmonic = 2.0f * h.harmonicTotal;
  const float residual = (total > harmonic) ? (total - harmonic) : 0.0f;
  if (residual > 1e-12f && harmonic > 0.0f) {
    f.harmonicToNoiseRatio = 10.0f * log10f(harmonic / residual);
  } else if (harmonic > 0.0f) {
    // Aucun residu mesurable : signal purement harmonique. On borne plutot que
    // de rendre +inf.
    f.harmonicToNoiseRatio = MIC_HNR_MAX_DB;
  }
  if (f.harmonicToNoiseRatio > MIC_HNR_MAX_DB) f.harmonicToNoiseRatio = MIC_HNR_MAX_DB;
  if (f.harmonicToNoiseRatio < -MIC_HNR_MAX_DB) f.harmonicToNoiseRatio = -MIC_HNR_MAX_DB;

#if MIC_FFT_ENABLED
  // La FFT ne tourne qu'une frame sur MIC_SPECTRAL_DECIMATION. Quand elle ne
  // tourne pas, les deux champs gardent la DERNIERE valeur mesuree - elle reste
  // la meilleure information disponible - mais `fftValid` reste faux pour que
  // personne ne la prenne pour une mesure de cette frame.
  if (spectral != nullptr && runFft && spectral->computeSpectrum(frame, n)) {
    f.spectralCentroid = spectral->spectralCentroid();
    f.spectralFlatness = spectral->spectralFlatness();
    f.fftValid = true;
  }
#else
  (void)spectral;
  (void)runFft;
#endif

  f.spectralValid = true;
}

}  // namespace AcousticFeatureBuilder
