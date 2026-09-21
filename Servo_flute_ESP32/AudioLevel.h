/***********************************************************************************************
 * AudioLevel - Niveau, crete et ecretage d'une frame audio
 *
 * Fonctions PURES, sans etat ni dependance materielle : elles se testent
 * entierement sur hote avec des signaux synthetiques.
 *
 * UNITES - point important
 * ------------------------
 * `rmsDbFS` et `peakDbFS` sont exprimes en dBFS : decibels relatifs a la pleine
 * echelle NUMERIQUE, ou 0 dBFS correspond a un echantillon de valeur 1.0. Ce
 * n'est PAS un dB SPL. Aucune conversion en niveau de pression acoustique n'est
 * possible sans l'etalonnage du microphone, qui n'existe pas ici. Tout affichage
 * doit donc dire "dBFS" et jamais "dB".
 *
 * ECRETAGE
 * --------
 * L'audit PHASE 0 (defaut A0-4) a montre que rien ne detectait un ecretage
 * pendant le jeu : seule une saturation PERMANENTE etait reperee au demarrage,
 * sur une frame de sondage. Or un ecretage transitoire suffit a fausser le
 * spectre (harmoniques parasites) et donc la detection d'overblow.
 *
 * On distingue donc deux choses :
 *   - `clippingRatio` : la proportion d'echantillons au rail sur CETTE frame ;
 *   - `clippingDetected` : ce ratio depasse le seuil d'alerte.
 * Un ecretage bref sur quelques echantillons n'est pas un microphone sature en
 * permanence, et les deux ne doivent pas etre confondus.
 ***********************************************************************************************/
#ifndef AUDIO_LEVEL_H
#define AUDIO_LEVEL_H

#include <math.h>
#include <stddef.h>
#include "settings.h"

struct FrameLevel {
  float rms = 0.0f;              // RMS lineaire, composante continue retiree
  float rmsDbFS = MIC_DBFS_FLOOR;
  float peak = 0.0f;             // |echantillon| maximal, continu retire
  float peakDbFS = MIC_DBFS_FLOOR;
  float dcOffset = 0.0f;         // moyenne de la frame (utile au diagnostic)
  float clippingRatio = 0.0f;    // 0..1 : proportion d'echantillons au rail
  bool  clippingDetected = false;
};

namespace AudioLevel {

// Conversion lineaire -> dBFS, plancher a MIC_DBFS_FLOOR pour ne jamais rendre
// -inf ni NaN (un silence numerique parfait donnerait log10(0)).
inline float toDbFS(float linear) {
  if (!(linear > 0.0f)) return MIC_DBFS_FLOOR;   // capture aussi NaN
  const float db = 20.0f * log10f(linear);
  return (db < MIC_DBFS_FLOOR) ? MIC_DBFS_FLOOR : db;
}

inline float fromDbFS(float dbfs) {
  return powf(10.0f, dbfs / 20.0f);
}

// Analyse de niveau complete en UN seul parcours apres le calcul de moyenne.
// `clipThreshold` est le niveau absolu au-dela duquel un echantillon est
// considere au rail ; `clipRatioWarn` la proportion a partir de laquelle on
// declare l'ecretage.
inline FrameLevel compute(const float* frame, size_t n,
                          float clipThreshold = MIC_CLIP_THRESHOLD,
                          float clipRatioWarn = MIC_CLIP_RATIO_WARN) {
  FrameLevel out;
  if (frame == nullptr || n == 0) return out;

  // La composante continue est retiree : un decalage de polarisation du
  // microphone ne doit jamais etre compte comme du niveau sonore.
  float mean = 0.0f;
  for (size_t i = 0; i < n; i++) mean += frame[i];
  mean /= (float)n;
  out.dcOffset = mean;

  float sumSq = 0.0f;
  float peak = 0.0f;
  size_t clipped = 0;
  for (size_t i = 0; i < n; i++) {
    const float c = frame[i] - mean;
    sumSq += c * c;
    const float a = fabsf(c);
    if (a > peak) peak = a;
    // L'ecretage se juge sur l'echantillon BRUT : c'est le convertisseur qui
    // sature, pas le signal centre.
    if (fabsf(frame[i]) >= clipThreshold) clipped++;
  }

  out.rms = sqrtf(sumSq / (float)n);
  out.peak = peak;
  out.rmsDbFS = toDbFS(out.rms);
  out.peakDbFS = toDbFS(out.peak);
  out.clippingRatio = (float)clipped / (float)n;
  out.clippingDetected = (out.clippingRatio > clipRatioWarn);
  return out;
}

}  // namespace AudioLevel

#endif  // AUDIO_LEVEL_H
