#include "AudioFilters.h"

#include <math.h>

namespace {
constexpr float kTwoPi = 6.28318530717958647692f;
// Q de Butterworth d'ordre 2 : reponse la plus plate possible dans la bande
// passante, sans surtension a la coupure.
constexpr float kButterworthQ = 0.70710678f;
}  // namespace

// Les coupures par defaut doivent encadrer LARGEMENT la plage de detection de
// pitch, sinon le filtrage fausserait la mesure au lieu de nettoyer le bruit.
static_assert(MIC_FILTER_HP_HZ <= 0.0f || MIC_FILTER_HP_HZ <= MIC_PITCH_MIN_HZ / 2.0f,
              "Le passe-haut doit rester au moins une octave sous MIC_PITCH_MIN_HZ");
static_assert(MIC_FILTER_LP_HZ <= 0.0f || MIC_FILTER_LP_HZ > MIC_PITCH_MAX_HZ * 1.2f,
              "Le passe-bas doit rester bien au-dessus de MIC_PITCH_MAX_HZ");

void Biquad::setPassthrough() {
  _b0 = 1.0f; _b1 = 0.0f; _b2 = 0.0f;
  _a1 = 0.0f; _a2 = 0.0f;
  _z1 = 0.0f; _z2 = 0.0f;
  _passthrough = true;
}

void Biquad::setHighPass(float cutoffHz, float sampleRate) {
  // Une coupure absurde rend la cellule TRANSPARENTE plutot qu'instable : mieux
  // vaut ne pas filtrer que produire des valeurs qui divergent.
  if (!(cutoffHz > 0.0f) || !(sampleRate > 0.0f) || cutoffHz >= sampleRate * 0.5f) {
    setPassthrough();
    return;
  }
  const float w0 = kTwoPi * cutoffHz / sampleRate;
  const float cw = cosf(w0);
  const float alpha = sinf(w0) / (2.0f * kButterworthQ);
  const float a0 = 1.0f + alpha;

  _b0 = ((1.0f + cw) * 0.5f) / a0;
  _b1 = (-(1.0f + cw)) / a0;
  _b2 = _b0;
  _a1 = (-2.0f * cw) / a0;
  _a2 = (1.0f - alpha) / a0;
  _z1 = 0.0f; _z2 = 0.0f;
  _passthrough = false;
}

void Biquad::setLowPass(float cutoffHz, float sampleRate) {
  if (!(cutoffHz > 0.0f) || !(sampleRate > 0.0f) || cutoffHz >= sampleRate * 0.5f) {
    setPassthrough();
    return;
  }
  const float w0 = kTwoPi * cutoffHz / sampleRate;
  const float cw = cosf(w0);
  const float alpha = sinf(w0) / (2.0f * kButterworthQ);
  const float a0 = 1.0f + alpha;

  _b0 = ((1.0f - cw) * 0.5f) / a0;
  _b1 = (1.0f - cw) / a0;
  _b2 = _b0;
  _a1 = (-2.0f * cw) / a0;
  _a2 = (1.0f - alpha) / a0;
  _z1 = 0.0f; _z2 = 0.0f;
  _passthrough = false;
}

void AudioFilterChain::configure(float sampleRate, float highPassHz, float lowPassHz,
                                 float dcPole) {
  _dc.configure(dcPole);
  if (highPassHz > 0.0f) _hp.setHighPass(highPassHz, sampleRate);
  else _hp.setPassthrough();
  if (lowPassHz > 0.0f) _lp.setLowPass(lowPassHz, sampleRate);
  else _lp.setPassthrough();
  reset();
}

void AudioFilterChain::processBlock(float* buf, size_t n) {
  if (buf == nullptr || n == 0) return;
  // Un etage transparent est saute entierement : pas de multiplication par 1
  // sur chaque echantillon, et l'etat reste vierge.
  _dc.processBlock(buf, n);
  if (!_hp.isPassthrough()) _hp.processBlock(buf, n);
  if (!_lp.isPassthrough()) _lp.processBlock(buf, n);
}

void AudioFilterChain::reset() {
  _dc.reset();
  _hp.reset();
  _lp.reset();
}
