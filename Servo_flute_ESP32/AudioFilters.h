/***********************************************************************************************
 * AudioFilters - Chaine de filtrage du flux audio
 *
 * Fonctions pures d'etat, sans allocation ni dependance materielle : la chaine
 * se teste entierement sur hote.
 *
 *     retrait du continu  ->  passe-haut  ->  passe-bas (optionnel)
 *
 * OU LE FILTRAGE EST APPLIQUE, ET POURQUOI LA
 * -------------------------------------------
 * Le filtrage est applique sur le FLUX, dans drainI2S(), avant l'ecriture dans
 * l'anneau - pas frame par frame. Deux raisons, et la seconde est la plus
 * importante :
 *
 *  1. Un filtre a reponse impulsionnelle infinie a une memoire. Le reinitialiser
 *     a chaque frame produirait un transitoire d'etablissement au debut de
 *     CHAQUE frame, c'est-a-dire un artefact periodique a la cadence d'analyse.
 *
 *  2. Les frames se RECOUVRENT de 50 %. Filtrer frame par frame ferait passer
 *     chaque echantillon deux fois dans le filtre, avec des etats differents :
 *     la moitie recouverte de deux frames successives ne contiendrait meme plus
 *     les memes valeurs.
 *
 * L'ECRETAGE NE SE MESURE PAS ICI
 * -------------------------------
 * C'est le convertisseur qui sature, pas le signal filtre. Un echantillon au
 * rail peut tres bien repasser sous le seuil apres un passe-haut. La detection
 * d'ecretage se fait donc sur les echantillons BRUTS, avant cette chaine (voir
 * AudioLevel::countClipped et AudioAnalyzer::drainI2S).
 *
 * CE QUE LE FILTRAGE NE DOIT PAS CASSER
 * -------------------------------------
 * Les frequences de coupure par defaut encadrent LARGEMENT la plage de
 * detection de pitch (MIC_PITCH_MIN_HZ..MIC_PITCH_MAX_HZ) : le passe-haut est
 * bien en dessous de la note la plus grave, le passe-bas bien au-dessus de la
 * plus aigue. Une coupure qui empieterait sur cette plage fausserait le pitch
 * au lieu de nettoyer le bruit ; c'est verifie par un static_assert.
 ***********************************************************************************************/
#ifndef AUDIO_FILTERS_H
#define AUDIO_FILTERS_H

#include <stddef.h>
#include "settings.h"

// Cellule biquad, forme directe II transposee (bonne stabilite numerique en
// simple precision, un seul jeu d'etats par cellule).
class Biquad {
public:
  Biquad() { setPassthrough(); }

  void setPassthrough();
  // Butterworth d'ordre 2 (Q = 0,7071). `cutoffHz` doit etre dans ]0, Fe/2[ ;
  // hors de cette plage la cellule devient transparente plutot que instable.
  void setHighPass(float cutoffHz, float sampleRate);
  void setLowPass(float cutoffHz, float sampleRate);

  void reset() { _z1 = 0.0f; _z2 = 0.0f; }

  inline float process(float x) {
    const float y = _b0 * x + _z1;
    _z1 = _b1 * x - _a1 * y + _z2;
    _z2 = _b2 * x - _a2 * y;
    return y;
  }

  void processBlock(float* buf, size_t n) {
    for (size_t i = 0; i < n; i++) buf[i] = process(buf[i]);
  }

  bool isPassthrough() const { return _passthrough; }

private:
  float _b0 = 1.0f, _b1 = 0.0f, _b2 = 0.0f;
  float _a1 = 0.0f, _a2 = 0.0f;
  float _z1 = 0.0f, _z2 = 0.0f;
  bool _passthrough = true;
};

// Retrait du continu du premier ordre : y[n] = x[n] - x[n-1] + R * y[n-1].
// Tres peu couteux (3 operations par echantillon) et utile meme quand le
// passe-haut est desactive : le decalage de polarisation d'un INMP441 n'est pas
// du signal, et le laisser passer biaiserait le niveau comme le spectre.
class DcBlocker {
public:
  void configure(float pole) { _r = (pole > 0.0f && pole < 1.0f) ? pole : 0.0f; }
  void reset() { _x1 = 0.0f; _y1 = 0.0f; }

  inline float process(float x) {
    if (_r <= 0.0f) return x;
    const float y = x - _x1 + _r * _y1;
    _x1 = x;
    _y1 = y;
    return y;
  }

  void processBlock(float* buf, size_t n) {
    if (_r <= 0.0f) return;
    for (size_t i = 0; i < n; i++) buf[i] = process(buf[i]);
  }

  bool isActive() const { return _r > 0.0f; }

private:
  float _r = 0.0f;   // 0 = desactive
  float _x1 = 0.0f, _y1 = 0.0f;
};

class AudioFilterChain {
public:
  AudioFilterChain() { configureDefaults(); }

  // Configuration explicite. Une frequence de coupure nulle ou negative
  // DESACTIVE l'etage correspondant, elle ne le regle pas a zero.
  void configure(float sampleRate, float highPassHz, float lowPassHz, float dcPole);
  void configureDefaults() {
    configure((float)MIC_SAMPLE_RATE, MIC_FILTER_HP_HZ, MIC_FILTER_LP_HZ, MIC_FILTER_DC_POLE);
  }

  // Filtre un bloc EN PLACE. A appeler sur le flux continu, jamais frame par
  // frame (voir l'en-tete du fichier).
  void processBlock(float* buf, size_t n);

  // Vide la memoire des filtres. A appeler quand le flux est interrompu
  // (microphone reinitialise, acquisition relancee) : sans cela le premier bloc
  // du nouveau flux serait melange a la queue de l'ancien.
  void reset();

  bool highPassActive() const { return !_hp.isPassthrough(); }
  bool lowPassActive() const { return !_lp.isPassthrough(); }
  bool dcBlockerActive() const { return _dc.isActive(); }

private:
  DcBlocker _dc;
  Biquad _hp;
  Biquad _lp;
};

#endif  // AUDIO_FILTERS_H
