/***********************************************************************************************
 * audio_signals.h - Generateurs de signaux PCM synthetiques, reproductibles
 *
 * Partages par tous les tests audio natifs. Aucune dependance Arduino / I2S : ces
 * generateurs produisent des tableaux de float normalises -1..+1, exactement le
 * format que la chaine d'analyse recoit apres conversion des mots I2S 32 bits.
 *
 * REPRODUCTIBILITE : le bruit utilise un LCG a graine explicite, jamais rand().
 * Deux executions produisent donc des echantillons identiques, sur n'importe
 * quelle machine. Un test qui echoue echoue toujours de la meme facon.
 *
 * ATTENTION : un signal synthetique ne valide PAS le comportement acoustique
 * reel. Il valide le traitement du signal. La distinction est maintenue dans la
 * documentation (voir AUDIO_ARCHITECTURE.md, section "Niveaux de validation").
 ***********************************************************************************************/
#ifndef TEST_AUDIO_SIGNALS_H
#define TEST_AUDIO_SIGNALS_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace audiosig {

constexpr float kPi = 3.14159265358979323846f;

// Generateur pseudo-aleatoire deterministe (LCG 32 bits). Une graine identique
// donne toujours la meme sequence : les tests de bruit sont reproductibles.
struct Lcg {
  uint32_t state;
  explicit Lcg(uint32_t seed = 12345u) : state(seed) {}
  // Uniforme dans [-1, +1].
  float bipolar() {
    state = state * 1103515245u + 12345u;
    return ((float)((state >> 16) & 0x7FFF) / 16383.5f) - 1.0f;
  }
};

// Description d'un signal de test. Tous les champs sont optionnels : un champ a
// zero ne contribue pas.
struct ToneSpec {
  float sampleRate = 32000.0f;
  float f0 = 0.0f;          // fondamentale (Hz), 0 = pas de fondamentale
  float amp = 0.0f;         // amplitude de la fondamentale
  float h2 = 0.0f;          // amplitude ABSOLUE de l'harmonique 2
  float h3 = 0.0f;          // amplitude ABSOLUE de l'harmonique 3
  float h4 = 0.0f;          // amplitude ABSOLUE de l'harmonique 4
  float dc = 0.0f;          // decalage continu
  float noise = 0.0f;       // amplitude du bruit blanc
  float clipAt = 0.0f;      // > 0 : ecretage symetrique a cette valeur
  float phase = 0.0f;       // phase initiale (radians)
  uint32_t seed = 12345u;   // graine du bruit
};

// Remplit `buf` avec le signal decrit. `startSample` permet de generer des
// blocs contigus : deux appels consecutifs avec startSample = 0 puis n donnent
// un signal continu, sans discontinuite de phase.
inline void fill(float* buf, size_t n, const ToneSpec& s, size_t startSample = 0) {
  Lcg rng(s.seed + (uint32_t)startSample);
  for (size_t i = 0; i < n; i++) {
    const float t = (float)(startSample + i) / s.sampleRate;
    float v = 0.0f;
    if (s.f0 > 0.0f) {
      const float w = 2.0f * kPi * s.f0 * t + s.phase;
      v += s.amp * sinf(w);
      if (s.h2 != 0.0f) v += s.h2 * sinf(2.0f * w);
      if (s.h3 != 0.0f) v += s.h3 * sinf(3.0f * w);
      if (s.h4 != 0.0f) v += s.h4 * sinf(4.0f * w);
    }
    v += s.dc;
    if (s.noise > 0.0f) v += s.noise * rng.bipolar();
    if (s.clipAt > 0.0f) {
      if (v > s.clipAt) v = s.clipAt;
      if (v < -s.clipAt) v = -s.clipAt;
    }
    buf[i] = v;
  }
}

// Raccourcis lisibles pour les cas les plus frequents.

inline void silence(float* buf, size_t n) {
  for (size_t i = 0; i < n; i++) buf[i] = 0.0f;
}

inline void dcOnly(float* buf, size_t n, float level) {
  for (size_t i = 0; i < n; i++) buf[i] = level;
}

inline void whiteNoise(float* buf, size_t n, float amp, uint32_t seed = 12345u) {
  Lcg rng(seed);
  for (size_t i = 0; i < n; i++) buf[i] = amp * rng.bipolar();
}

inline void pureTone(float* buf, size_t n, float f0, float amp,
                     float sampleRate = 32000.0f, size_t startSample = 0) {
  ToneSpec s; s.sampleRate = sampleRate; s.f0 = f0; s.amp = amp;
  fill(buf, n, s, startSample);
}

// Note "flute" plausible : fondamentale dominante, harmoniques decroissantes,
// un peu de bruit de souffle. Ce n'est PAS un modele valide acoustiquement,
// seulement un signal harmonique realiste pour exercer le DSP.
inline void fluteLike(float* buf, size_t n, float f0, float amp, float breath,
                      float sampleRate = 32000.0f, size_t startSample = 0) {
  ToneSpec s;
  s.sampleRate = sampleRate;
  s.f0 = f0;
  s.amp = amp;
  s.h2 = 0.30f * amp;
  s.h3 = 0.12f * amp;
  s.h4 = 0.05f * amp;
  s.noise = breath;
  fill(buf, n, s, startSample);
}

// Mot I2S brut tel que le produit un INMP441 : echantillon 24 bits cale a
// gauche dans un mot de 32 bits. Utilise pour tester la classification du
// signal brut et la conversion.
inline int32_t toI2sWord(float normalized) {
  if (normalized > 1.0f) normalized = 1.0f;
  if (normalized < -1.0f) normalized = -1.0f;
  int32_t sample24 = (int32_t)(normalized * 8388607.0f);   // +-(2^23 - 1)
  return sample24 << 8;
}

inline void fillI2s(int32_t* raw, size_t n, const ToneSpec& s, size_t startSample = 0) {
  for (size_t i = 0; i < n; i++) {
    float v;
    fill(&v, 1, s, startSample + i);
    raw[i] = toI2sWord(v);
  }
}

}  // namespace audiosig

#endif  // TEST_AUDIO_SIGNALS_H
