#include "PitchDetector.h"

#include <math.h>
#include "PitchMath.h"
#include "AutoCalMath.h"

namespace {

// Bornes de lag derivees de la plage de detection. Calculees une fois par appel,
// elles ne dependent que de la configuration.
struct TauRange {
  int min;
  int max;
  bool valid;
};

inline TauRange tauRangeFor(size_t n) {
  TauRange t;
  t.min = (int)(MIC_SAMPLE_RATE / MIC_PITCH_MAX_HZ);
  t.max = (int)(MIC_SAMPLE_RATE / MIC_PITCH_MIN_HZ);
  if (t.min < 1) t.min = 1;
  if (t.max > MIC_YIN_TAU_MAX) t.max = MIC_YIN_TAU_MAX;
  const int W = (int)n / 2;
  t.valid = (t.min < t.max) && (W >= t.max + 1);
  return t;
}

// Difference YIN a un lag FRACTIONNAIRE. L'echantillon decale est obtenu par
// interpolation lineaire entre deux echantillons voisins.
inline float diffAtLag(const float* x, int W, float tau) {
  const int t0 = (int)tau;
  const float frac = tau - (float)t0;
  float sum = 0.0f;
  for (int i = 0; i < W; i++) {
    const float a = x[i + t0];
    const float b = x[i + t0 + 1];
    const float shifted = a + frac * (b - a);
    const float d = x[i] - shifted;
    sum += d * d;
  }
  return sum;
}

// Affine le lag par recherche ternaire sur d(tau) a pas fractionnaire.
//
// POURQUOI PAS L'INTERPOLATION PARABOLIQUE (audit PHASE 0, defaut A0-3)
// ---------------------------------------------------------------------
// La version precedente ajustait une parabole sur trois valeurs ENTIERES de
// tau. Or d(tau) est fortement asymetrique autour de son minimum, et le pas de
// tau devient grossier des que la note monte : a 1046 Hz (do6) un pas vaut 55
// cents. Exemple mesure sur un do6 pur, periode vraie 30,578 echantillons :
//
//     d(30) = 0,579   d(31) = 0,308   d(32) = 3,470
//
// Le minimum discret est 31, mais le vrai minimum est a 30,58 ; le point 32 est
// deja tres haut sur la remontee et tire la parabole vers la droite. Resultat :
// 31,42 au lieu de 30,58, soit -47 cents. L'erreur pire-cas atteignait 47 cents
// sur la plage do4-mi6, et retirer la fenetre de Hann n'y changeait presque
// rien (19,6 c de moyenne contre 20,9 c) : la fenetre n'etait PAS la cause.
//
// La recherche ternaire evalue d(tau) a des lags reellement fractionnaires, ce
// qui supprime la dependance au pas de tau. Mesure apres correction : 0,53 cent
// pire-cas, 0,18 cent de moyenne, pour environ +12 % de cout YIN.
inline float refineTau(const float* x, int W, int tauEst, int tauMin, int tauMax) {
  float lo = (float)tauEst - 1.0f;
  float hi = (float)tauEst + 1.0f;
  if (lo < (float)tauMin) lo = (float)tauMin;
  if (hi > (float)tauMax) hi = (float)tauMax;
  if (!(hi > lo)) return (float)tauEst;

  for (int it = 0; it < MIC_YIN_REFINE_ITERATIONS; it++) {
    const float third = (hi - lo) / 3.0f;
    const float m1 = lo + third;
    const float m2 = hi - third;
    if (diffAtLag(x, W, m1) <= diffAtLag(x, W, m2)) hi = m2;
    else lo = m1;
  }
  return 0.5f * (lo + hi);
}

// Descend jusqu'au minimum local a partir de `tau`.
inline int descendToLocalMin(const float* yin, int tau, int tauMax) {
  while (tau + 1 < tauMax && yin[tau + 1] < yin[tau]) tau++;
  return tau;
}

}  // namespace

// ------------------------------------------------------------------- etat --

void PitchDetector::resetTracking() {
  _expectedMidi = 0;
  _expectedHz = 0.0f;
  _histCount = 0;
  _histIdx = 0;
  for (uint8_t i = 0; i < MIC_PITCH_HISTORY; i++) _history[i] = 0.0f;
}

void PitchDetector::setExpectedMidiNote(int midi) {
  if (midi <= 0 || midi > 127) { clearExpectedMidiNote(); return; }
  _expectedMidi = midi;
  _expectedHz = PitchMath::midiToHz(midi);
}

// ------------------------------------------------------------- utilitaires --

float PitchDetector::rms(const float* samples, size_t n) {
  if (samples == nullptr || n == 0) return 0.0f;
  float mean = 0.0f;
  for (size_t i = 0; i < n; i++) mean += samples[i];
  mean /= (float)n;
  float sum = 0.0f;
  for (size_t i = 0; i < n; i++) {
    const float c = samples[i] - mean;
    sum += c * c;
  }
  return sqrtf(sum / (float)n);
}

MicSignalClass PitchDetector::classifyRaw(const int32_t* raw, size_t n) {
  if (raw == nullptr || n == 0) return MIC_SIG_ALL_ZERO;
  int nonZero = 0;
  int saturated = 0;
  int64_t minV = 0, maxV = 0;
  bool first = true;
  // INMP441 : echantillon 24 bits cale a gauche dans un mot de 32 bits.
  const int32_t kSatThreshold = 0x7F0000;
  for (size_t i = 0; i < n; i++) {
    const int32_t s = raw[i] >> 8;
    if (s != 0) nonZero++;
    if (s > kSatThreshold || s < -kSatThreshold) saturated++;
    if (first) { minV = maxV = s; first = false; }
    else { if (s < minV) minV = s; if (s > maxV) maxV = s; }
  }
  if (nonZero == 0) return MIC_SIG_ALL_ZERO;
  if (saturated > (int)(n * 9 / 10)) return MIC_SIG_SATURATED;
  if ((maxV - minV) < 256) return MIC_SIG_STUCK;
  return MIC_SIG_OK;
}

// ---------------------------------------------------------------- coeur YIN --

PitchResult PitchDetector::runYin(const float* samples, size_t n) const {
  PitchResult out;
  if (samples == nullptr) return out;
  if (n > (size_t)MIC_BUFFER_SIZE) n = MIC_BUFFER_SIZE;
  if (n < 8) return out;

  const TauRange tr = tauRangeFor(n);
  if (!tr.valid) return out;

  const int W = (int)n / 2;

  // Fonction de difference cumulee normalisee. Aucun pretraitement du signal :
  // la composante continue s'annule dans la difference, et toute fenetre
  // introduirait un biais dependant de tau (voir l'en-tete du fichier).
  _yinBuf[0] = 1.0f;
  float runningSum = 0.0f;
  for (int tau = 1; tau <= tr.max; tau++) {
    float sum = 0.0f;
    for (int i = 0; i < W; i++) {
      const float delta = samples[i] - samples[i + tau];
      sum += delta * delta;
    }
    runningSum += sum;
    _yinBuf[tau] = (runningSum > 0.0f) ? (sum * (float)tau / runningSum) : 1.0f;
  }

  int tauEst = -1;

  // --- Chemin "note attendue" (PHASE 2.2) ----------------------------------
  // Quand la note visee est connue, on evalue EXPLICITEMENT les lags des
  // rapports harmoniques plausibles et on retient le creux le plus profond.
  // Cela tranche l'ambiguite d'octave de maniere deterministe, au lieu de
  // dependre du premier creux rencontre en balayant tau.
  if (_expectedMidi > 0 && _expectedHz > 0.0f) {
    // Les rapports sont parcourus du PLUS AIGU au plus grave, donc par lag
    // CROISSANT, et le premier creux sous le seuil gagne.
    //
    // Prendre le creux le plus PROFOND serait faux : pour tout signal
    // periodique, d(2T) et d(3T) sont naturellement tres profonds. Un la4 joue
    // en overblow a 880 Hz presente un creux profond au lag de 440 Hz (deux
    // periodes exactement), et on conclurait a un la4 correct alors que
    // l'instrument sonne une octave trop haut. Le plus petit lag qualifiant est
    // le meme principe que le chemin general, et il est correct pour la meme
    // raison : un sous-multiple ne peut pas etre choisi a la place de la vraie
    // periode.
    const float kRatios[] = {3.0f, 2.0f, 1.0f, 0.5f};   // 3*f0, 2*f0, f0, f0/2
    for (float ratio : kRatios) {
      const float hz = _expectedHz * ratio;
      if (hz <= 0.0f) continue;
      const int tau = (int)lroundf((float)MIC_SAMPLE_RATE / hz);
      // Un candidat hors de la plage de lags n'est pas detectable : le tester
      // reviendrait a lire hors du tampon.
      if (tau <= tr.min || tau >= tr.max) continue;
      const int local = descendToLocalMin(_yinBuf, tau, tr.max);
      if (_yinBuf[local] < MIC_YIN_THRESHOLD) { tauEst = local; break; }
    }
  }

  // --- Chemin general (inchange) -------------------------------------------
  // Le PREMIER lag dont la difference normalisee passe sous le seuil est la
  // periode fondamentale. Prendre le plus petit lag qualifiant puis descendre
  // vers son minimum local evite de choisir un sous-multiple (2T, 3T...), donc
  // les erreurs d'octave vers le BAS.
  if (tauEst < 0) {
    for (int tau = tr.min; tau < tr.max; tau++) {
      if (_yinBuf[tau] < MIC_YIN_THRESHOLD) {
        tauEst = descendToLocalMin(_yinBuf, tau, tr.max);
        break;
      }
    }
  }
  if (tauEst < 0) return out;

  const float betterTau = refineTau(samples, W, tauEst, tr.min, tr.max);
  if (betterTau < 1.0f) return out;

  float aperiodicity = _yinBuf[tauEst];
  if (aperiodicity < 0.0f) aperiodicity = 0.0f;
  if (aperiodicity > 1.0f) aperiodicity = 1.0f;
  out.confidence = 1.0f - aperiodicity;

  const float hz = (float)MIC_SAMPLE_RATE / betterTau;
  if (hz < MIC_PITCH_MIN_HZ || hz > MIC_PITCH_MAX_HZ) return out;
  out.hz = hz;
  out.valid = (out.confidence >= MIC_YIN_CONFIDENCE_MIN);
  return out;
}

void PitchDetector::annotate(PitchResult& r) const {
  if (r.hz <= 0.0f) return;
  r.midi = PitchMath::hzToMidi(r.hz);
  r.cents = PitchMath::hzToCents(r.hz, r.midi);
  if (_expectedMidi <= 0) return;

  r.expectedMatch =
      PitchMath::isNoteMatch(r.midi, r.cents, _expectedMidi, MIC_EXPECTED_TOLERANCE_CENTS);
  const int diff = r.midi - _expectedMidi;
  if (diff > 0 && (diff % 12) == 0) r.octaveAbove = true;
  if (diff < 0 && ((-diff) % 12) == 0) r.octaveBelow = true;
}

PitchResult PitchDetector::analyse(const float* samples, size_t n) const {
  PitchResult r = runYin(samples, n);
  annotate(r);
  return r;
}

PitchResult PitchDetector::detect(const float* samples, size_t n) {
  PitchResult r = analyse(samples, n);

  if (!r.valid) {
    // Une mesure non fiable ne doit pas entrer dans l'historique : elle
    // ferait paraitre instable une note qui ne l'est pas, ou l'inverse.
    return r;
  }

  // Historique en cents ABSOLUS (note * 100 + ecart) : la stabilite se mesure
  // ainsi independamment de la note, et un saut d'octave apparait comme une
  // dispersion de 1200 cents.
  const float absCents = (float)r.midi * 100.0f + r.cents;
  _history[_histIdx] = absCents;
  _histIdx = (uint8_t)((_histIdx + 1) % MIC_PITCH_HISTORY);
  if (_histCount < MIC_PITCH_HISTORY) _histCount++;

  if (_histCount >= MIC_PITCH_HISTORY) {
    float lo = _history[0], hi = _history[0];
    for (uint8_t i = 1; i < _histCount; i++) {
      if (_history[i] < lo) lo = _history[i];
      if (_history[i] > hi) hi = _history[i];
    }
    r.stability = AutoCalMath::pitchStability(hi - lo, MIC_PITCH_STABILITY_REF_CENTS);
  }
  return r;
}

// -------------------------------------------------- reference A/B (legacy) --

PitchResult PitchDetector::detectWindowed(float* samples, size_t n) const {
  // ANCIEN comportement, conserve uniquement pour la comparaison A/B des tests :
  // retrait de la composante continue puis fenetre de Hann AVANT YIN. Le tampon
  // est modifie en place. Ne pas utiliser en production (defaut A0-3).
  PitchResult out;
  if (samples == nullptr) return out;
  if (n > (size_t)MIC_BUFFER_SIZE) n = MIC_BUFFER_SIZE;
  if (n < 8) return out;

  float mean = 0.0f;
  for (size_t i = 0; i < n; i++) mean += samples[i];
  mean /= (float)n;
  for (size_t i = 0; i < n; i++) samples[i] -= mean;

  const float kTwoPi = 6.28318530717958647692f;
  for (size_t i = 0; i < n; i++) {
    samples[i] *= 0.5f * (1.0f - cosf(kTwoPi * (float)i / (float)(n - 1)));
  }

  out = runYin(samples, n);
  annotate(out);
  return out;
}
