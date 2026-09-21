#include "SpectralAnalyzer.h"

#include <math.h>

namespace {
constexpr float kTwoPi = 6.28318530717958647692f;

// Moyenne d'une frame. Le continu est retire partout : un decalage de
// polarisation du microphone n'est pas du signal.
inline float meanOf(const float* x, size_t n) {
  float m = 0.0f;
  for (size_t i = 0; i < n; i++) m += x[i];
  return m / (float)n;
}
}  // namespace

// ------------------------------------------------------------------ Goertzel --

float SpectralAnalyzer::goertzelPower(const float* x, size_t n, float targetHz,
                                      float sampleRate) {
  if (x == nullptr || n < 2 || sampleRate <= 0.0f) return 0.0f;
  // Au-dessus de Nyquist la mesure n'a aucun sens : elle rendrait l'energie
  // d'une frequence repliee, donc d'une autre frequence.
  if (targetHz <= 0.0f || targetHz >= sampleRate * 0.5f) return 0.0f;

  const float omega = kTwoPi * targetHz / sampleRate;
  const float coeff = 2.0f * cosf(omega);
  const float mean = meanOf(x, n);

  float s1 = 0.0f, s2 = 0.0f;
  for (size_t i = 0; i < n; i++) {
    const float s0 = (x[i] - mean) + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  // Puissance normalisee par la longueur pour que le resultat ne depende pas
  // de la taille de frame.
  const float power = (s1 * s1 + s2 * s2 - coeff * s1 * s2) / ((float)n * (float)n);
  return power > 0.0f ? power : 0.0f;
}

float SpectralAnalyzer::totalPower(const float* x, size_t n) {
  if (x == nullptr || n == 0) return 0.0f;
  const float mean = meanOf(x, n);
  float sum = 0.0f;
  for (size_t i = 0; i < n; i++) {
    const float c = x[i] - mean;
    sum += c * c;
  }
  return sum / (float)n;
}

HarmonicEnergies SpectralAnalyzer::harmonics(const float* x, size_t n, float f0,
                                             float sampleRate) {
  HarmonicEnergies out;
  if (x == nullptr || n < 2 || f0 <= 0.0f || sampleRate <= 0.0f) return out;
  const float nyquist = sampleRate * 0.5f;
  if (f0 >= nyquist) return out;

  float* slots[4] = {&out.fundamental, &out.h2, &out.h3, &out.h4};
  for (int k = 0; k < 4; k++) {
    const float hz = f0 * (float)(k + 1);
    // Une note aigue n'a tout simplement pas quatre harmoniques sous Nyquist.
    // On s'arrete plutot que de mesurer une frequence repliee.
    if (hz >= nyquist) break;
    *slots[k] = goertzelPower(x, n, hz, sampleRate);
    out.measured = (uint8_t)(k + 1);
  }

  out.harmonicTotal = out.fundamental + out.h2 + out.h3 + out.h4;
  if (out.fundamental > 0.0f) {
    out.h2Ratio = out.h2 / out.fundamental;
    out.h3Ratio = out.h3 / out.fundamental;
    out.h4Ratio = out.h4 / out.fundamental;
  }
  out.valid = (out.measured > 0);
  return out;
}

// ----------------------------------------------------------------------- FFT --

#if MIC_FFT_ENABLED

static_assert((MIC_FFT_SIZE & (MIC_FFT_SIZE - 1)) == 0,
              "MIC_FFT_SIZE doit etre une puissance de deux (FFT radix-2)");
static_assert(MIC_FFT_SIZE >= 64, "MIC_FFT_SIZE trop petite pour etre utile");
static_assert(MIC_FFT_SIZE <= MIC_ANALYSIS_FRAME_SIZE,
              "MIC_FFT_SIZE ne peut pas depasser la taille de frame analysee");

void SpectralAnalyzer::buildWindow() {
  // Hann, demi-table : w[i] = w[N-1-i].
  const int N = MIC_FFT_SIZE;
  for (int i = 0; i < N / 2; i++) {
    _window[i] = 0.5f * (1.0f - cosf(kTwoPi * (float)i / (float)(N - 1)));
  }
  _spectrumValid = false;
}

void SpectralAnalyzer::fftInPlace() {
  const int N = MIC_FFT_SIZE;

  // 1. Permutation par inversion de bits, calculee a la volee (pas de table).
  for (int i = 1, j = 0; i < N; i++) {
    int bit = N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = _re[i]; _re[i] = _re[j]; _re[j] = t;
      t = _im[i]; _im[i] = _im[j]; _im[j] = t;
    }
  }

  // 2. Papillons Cooley-Tukey. Les facteurs de rotation sont produits par la
  //    recurrence stable de Numerical Recipes : deux appels trigonometriques
  //    par etage (18 au total pour 512 points) au lieu de N/2*log2(N) = 2304.
  for (int len = 2; len <= N; len <<= 1) {
    const float theta = -kTwoPi / (float)len;
    const float wpr = -2.0f * sinf(0.5f * theta) * sinf(0.5f * theta);
    const float wpi = sinf(theta);
    for (int i = 0; i < N; i += len) {
      float wr = 1.0f, wi = 0.0f;
      for (int k = 0; k < len / 2; k++) {
        const int a = i + k;
        const int b = a + len / 2;
        const float tr = _re[b] * wr - _im[b] * wi;
        const float ti = _re[b] * wi + _im[b] * wr;
        _re[b] = _re[a] - tr;
        _im[b] = _im[a] - ti;
        _re[a] += tr;
        _im[a] += ti;
        const float wrTmp = wr;
        wr += wr * wpr - wi * wpi;
        wi += wi * wpr + wrTmp * wpi;
      }
    }
  }
}

bool SpectralAnalyzer::computeSpectrum(const float* x, size_t n) {
  _spectrumValid = false;
  if (x == nullptr || n < (size_t)MIC_FFT_SIZE) return false;

  // Continu retire puis fenetre de Hann. Ici la fenetre est LEGITIME : elle
  // limite les fuites entre bins (voir l'en-tete du fichier).
  const float mean = meanOf(x, MIC_FFT_SIZE);
  for (int i = 0; i < MIC_FFT_SIZE; i++) {
    _re[i] = (x[i] - mean) * windowAt((size_t)i);
    _im[i] = 0.0f;
  }

  fftInPlace();

  // Amplitudes, normalisees par la taille pour ne pas dependre de MIC_FFT_SIZE.
  const float norm = 1.0f / (float)MIC_FFT_SIZE;
  const size_t bins = binCount();
  for (size_t k = 0; k < bins; k++) {
    _mag[k] = sqrtf(_re[k] * _re[k] + _im[k] * _im[k]) * norm;
  }
  _spectrumValid = true;
  return true;
}

float SpectralAnalyzer::spectralCentroid(float sampleRate) const {
  if (!_spectrumValid) return 0.0f;
  const size_t bins = binCount();
  float weighted = 0.0f, total = 0.0f;
  // Le bin 0 (continu) est exclu : il n'est pas du son et fausserait le centre.
  for (size_t k = 1; k < bins; k++) {
    weighted += binToHz(k, sampleRate) * _mag[k];
    total += _mag[k];
  }
  return (total > 1e-12f) ? (weighted / total) : 0.0f;
}

float SpectralAnalyzer::spectralFlatness() const {
  if (!_spectrumValid) return 0.0f;
  const size_t bins = binCount();
  // Moyenne geometrique calculee par somme de logarithmes : le produit direct
  // de 256 amplitudes sous-deborderait immediatement en simple precision.
  // Le plancher evite log(0) sur un bin vide.
  const float kFloor = 1e-12f;
  double logSum = 0.0;
  double arithSum = 0.0;
  size_t count = 0;
  for (size_t k = 1; k < bins; k++) {
    const float m = (_mag[k] > kFloor) ? _mag[k] : kFloor;
    logSum += log((double)m);
    arithSum += (double)m;
    count++;
  }
  if (count == 0 || arithSum <= 0.0) return 0.0f;
  const double geo = exp(logSum / (double)count);
  const double arith = arithSum / (double)count;
  const double flat = geo / arith;
  if (flat < 0.0) return 0.0f;
  if (flat > 1.0) return 1.0f;
  return (float)flat;
}

float SpectralAnalyzer::bandEnergy(float loHz, float hiHz, float sampleRate) const {
  if (!_spectrumValid || hiHz <= loHz) return 0.0f;
  const size_t bins = binCount();
  float sum = 0.0f;
  for (size_t k = 1; k < bins; k++) {
    const float hz = binToHz(k, sampleRate);
    if (hz >= loHz && hz <= hiHz) sum += _mag[k] * _mag[k];
  }
  return sum;
}

#endif  // MIC_FFT_ENABLED
