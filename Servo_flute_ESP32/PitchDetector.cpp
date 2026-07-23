#include "PitchDetector.h"

#include <math.h>
#include "PitchMath.h"

void PitchDetector::buildHannWindow() {
  // w[n] = 0.5 * (1 - cos(2*pi*n/(N-1))). Precomputed once (no per-frame trig).
  const float kTwoPi = 6.28318530717958647692f;
  const int N = MIC_BUFFER_SIZE;
  for (int n = 0; n < N; n++) {
    _hann[n] = 0.5f * (1.0f - cosf(kTwoPi * (float)n / (float)(N - 1)));
  }
}

float PitchDetector::rms(const float* samples, size_t n) {
  if (n == 0) return 0.0f;
  // DC-removed RMS: subtract the mean so a bias does not inflate the level.
  float mean = 0.0f;
  for (size_t i = 0; i < n; i++) mean += samples[i];
  mean /= (float)n;
  float sum = 0.0f;
  for (size_t i = 0; i < n; i++) {
    float c = samples[i] - mean;
    sum += c * c;
  }
  return sqrtf(sum / (float)n);
}

PitchResult PitchDetector::detect(float* samples, size_t n) const {
  PitchResult out{false, 0.0f, 0.0f};
  if (n > (size_t)MIC_BUFFER_SIZE) n = MIC_BUFFER_SIZE;
  if (n < 8) return out;

  // 1. Remove DC bias.
  float mean = 0.0f;
  for (size_t i = 0; i < n; i++) mean += samples[i];
  mean /= (float)n;
  for (size_t i = 0; i < n; i++) samples[i] -= mean;

  // 2. Hann window (limits edge/leakage errors).
  for (size_t i = 0; i < n; i++) samples[i] *= _hann[i];

  // 3. YIN.
  int tauMin = (int)(MIC_SAMPLE_RATE / MIC_PITCH_MAX_HZ);
  int tauMax = (int)(MIC_SAMPLE_RATE / MIC_PITCH_MIN_HZ);
  if (tauMin < 1) tauMin = 1;
  if (tauMax > MIC_YIN_TAU_MAX) tauMax = MIC_YIN_TAU_MAX;

  const int W = (int)n / 2;
  if (W < tauMax + 1) return out;
  if (tauMin >= tauMax) return out;

  _yinBuf[0] = 1.0f;
  float runningSum = 0.0f;
  for (int tau = 1; tau <= tauMax; tau++) {
    float sum = 0.0f;
    for (int i = 0; i < W; i++) {
      float delta = samples[i] - samples[i + tau];
      sum += delta * delta;
    }
    runningSum += sum;
    _yinBuf[tau] = (runningSum > 0.0f) ? (sum * (float)tau / runningSum) : 1.0f;
  }

  // Absolute-threshold step: the FIRST lag whose cumulative-mean-normalized
  // difference dips below the threshold is the fundamental period. Because we
  // take the first (smallest) qualifying lag and descend to its local minimum,
  // sub-multiple lags (2T, 3T, ...) are never chosen, so octave-DOWN errors do
  // not occur. (A 2*tau "octave guard" was removed: for any periodic signal
  // d(2T) is naturally deep, so such a guard mis-fires and forces octave-down
  // errors at high pitch.)
  int tauEst = -1;
  for (int tau = tauMin; tau < tauMax; tau++) {
    if (_yinBuf[tau] < MIC_YIN_THRESHOLD) {
      while (tau + 1 < tauMax && _yinBuf[tau + 1] < _yinBuf[tau]) tau++;
      tauEst = tau;
      break;
    }
  }
  if (tauEst < 0) return out;

  // Parabolic interpolation for sub-sample accuracy.
  float betterTau = (float)tauEst;
  if (tauEst > tauMin && tauEst < tauMax) {
    float s0 = _yinBuf[tauEst - 1];
    float s1 = _yinBuf[tauEst];
    float s2 = _yinBuf[tauEst + 1];
    float denom = 2.0f * (2.0f * s1 - s2 - s0);
    if (fabsf(denom) > 1e-6f) betterTau = (float)tauEst + (s0 - s2) / denom;
  }
  if (betterTau < 1.0f) return out;

  float aperiodicity = _yinBuf[tauEst];
  if (aperiodicity < 0.0f) aperiodicity = 0.0f;
  if (aperiodicity > 1.0f) aperiodicity = 1.0f;
  out.confidence = 1.0f - aperiodicity;

  float hz = (float)MIC_SAMPLE_RATE / betterTau;
  if (hz < MIC_PITCH_MIN_HZ || hz > MIC_PITCH_MAX_HZ) return out;
  out.hz = hz;
  out.valid = (out.confidence >= MIC_YIN_CONFIDENCE_MIN);
  return out;
}

MicSignalClass PitchDetector::classifyRaw(const int32_t* raw, size_t n) {
  if (n == 0) return MIC_SIG_ALL_ZERO;
  int nonZero = 0;
  int saturated = 0;
  int64_t minV = 0, maxV = 0;
  bool first = true;
  // INMP441 data is left-aligned 24-bit in a 32-bit word: use the upper 24 bits.
  const int32_t kSatThreshold = (int32_t)0x7F0000 << 8 >> 8;  // near +full-scale (24-bit)
  for (size_t i = 0; i < n; i++) {
    int32_t s = raw[i] >> 8;  // 24-bit sample
    if (s != 0) nonZero++;
    if (s > kSatThreshold || s < -kSatThreshold) saturated++;
    if (first) { minV = maxV = s; first = false; }
    else { if (s < minV) minV = s; if (s > maxV) maxV = s; }
  }
  if (nonZero == 0) return MIC_SIG_ALL_ZERO;
  // Permanently clipped: almost every sample at the rail.
  if (saturated > (int)(n * 9 / 10)) return MIC_SIG_SATURATED;
  // Stuck line: non-zero but essentially constant (no clock / bad wiring).
  int64_t span = maxV - minV;
  if (span < 256) return MIC_SIG_STUCK;
  return MIC_SIG_OK;
}
