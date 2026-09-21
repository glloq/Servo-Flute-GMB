/***********************************************************************************************
 * SpectralAnalyzer - Analyse frequentielle legere (Goertzel + FFT optionnelle)
 *
 * Aucune dependance materielle : la classe se teste entierement sur hote avec
 * des signaux synthetiques. Aucune allocation dynamique.
 *
 * DEUX CHEMINS, DEUX ROLES
 * ------------------------
 * Goertzel repond a la question "combien d'energie y a-t-il EXACTEMENT a cette
 * frequence ?". Quand la note attendue est connue, c'est tout ce qu'il faut pour
 * mesurer f0, 2f0, 3f0, 4f0 : le cout est de n operations par cible, soit
 * ~4 % du cout de YIN pour quatre harmoniques. C'est le chemin par defaut.
 *
 * La FFT repond a "a quoi ressemble tout le spectre ?" : centre de gravite,
 * platitude, energie hors harmoniques. Elle coute bien plus cher et n'est pas
 * necessaire a chaque frame - d'ou MIC_SPECTRAL_DECIMATION.
 *
 * FENETRAGE
 * ---------
 * Ici, contrairement a YIN, la fenetre de Hann est LEGITIME et necessaire : elle
 * limite les fuites entre bins d'un signal non periodique sur la fenetre. C'est
 * la distinction que l'audit PHASE 0 avait manquee - le fenetrage appartient au
 * domaine frequentiel, pas a un estimateur temporel de periode.
 *
 * Goertzel est applique SANS fenetre : on mesure une puissance a une frequence
 * connue, pas un spectre, et la fenetre ne ferait qu'attenuer le resultat d'un
 * facteur dependant de l'alignement.
 *
 * POURQUOI PAS ESP-DSP
 * --------------------
 * ESP-DSP offrirait une FFT plus rapide, mais rendrait cette classe
 * incompilable sur hote, donc non testable. Une FFT radix-2 de 512 points coute
 * ~4 600 papillons, ce qui reste modeste face aux 92 160 operations de YIN. Si
 * une mesure sur materiel montre que la FFT domine, elle pourra etre remplacee
 * derriere cette meme interface, sans toucher aux appelants ni aux tests.
 ***********************************************************************************************/
#ifndef SPECTRAL_ANALYZER_H
#define SPECTRAL_ANALYZER_H

#include <stddef.h>
#include <stdint.h>
#include "settings.h"

// Energies harmoniques mesurees par Goertzel autour d'une fondamentale connue.
struct HarmonicEnergies {
  bool valid = false;         // f0 exploitable et harmoniques sous Nyquist
  float fundamental = 0.0f;   // puissance a f0
  float h2 = 0.0f;
  float h3 = 0.0f;
  float h4 = 0.0f;
  float harmonicTotal = 0.0f; // f0 + h2 + h3 + h4
  // Rapports a la fondamentale. 0 si la fondamentale est nulle.
  float h2Ratio = 0.0f;
  float h3Ratio = 0.0f;
  float h4Ratio = 0.0f;
  // Combien d'harmoniques sont reellement tombees sous Nyquist (1 a 4). Une
  // note aigue n'a pas quatre harmoniques mesurables a 32 kHz.
  uint8_t measured = 0;
};

class SpectralAnalyzer {
public:
  SpectralAnalyzer() { buildWindow(); }

  // --- Goertzel (chemin par defaut) ----------------------------------------

  // Puissance du signal a `targetHz`. Fonction PURE : ne modifie rien.
  // Retourne 0 si la cible est hors de ]0, Nyquist[.
  static float goertzelPower(const float* x, size_t n, float targetHz,
                             float sampleRate = (float)MIC_SAMPLE_RATE);

  // Energies de f0, 2f0, 3f0, 4f0. Les harmoniques au-dessus de Nyquist sont
  // ignorees et `measured` dit combien ont reellement ete mesurees.
  static HarmonicEnergies harmonics(const float* x, size_t n, float f0,
                                    float sampleRate = (float)MIC_SAMPLE_RATE);

  // Energie totale de la frame (somme des carres, continu retire). Sert de
  // denominateur aux rapports harmonique / bruit.
  static float totalPower(const float* x, size_t n);

#if MIC_FFT_ENABLED
  // --- FFT (chemin spectral complet) ---------------------------------------

  // Calcule le spectre d'amplitude des MIC_FFT_SIZE premiers echantillons
  // (fenetre de Hann appliquee). Retourne false si n < MIC_FFT_SIZE.
  bool computeSpectrum(const float* x, size_t n);

  bool hasSpectrum() const { return _spectrumValid; }
  size_t binCount() const { return MIC_FFT_SIZE / 2 + 1; }
  const float* magnitudes() const { return _mag; }
  static float binToHz(size_t bin, float sampleRate = (float)MIC_SAMPLE_RATE) {
    return (float)bin * sampleRate / (float)MIC_FFT_SIZE;
  }

  // Centre de gravite spectral (Hz) : ou se situe "en moyenne" l'energie. Un
  // son souffle le fait monter, une note pleine le garde bas.
  float spectralCentroid(float sampleRate = (float)MIC_SAMPLE_RATE) const;

  // Platitude spectrale 0..1 (moyenne geometrique / moyenne arithmetique).
  // Proche de 1 = bruit large bande, proche de 0 = spectre a raies.
  float spectralFlatness() const;

  // Energie dans une bande [loHz, hiHz].
  float bandEnergy(float loHz, float hiHz,
                   float sampleRate = (float)MIC_SAMPLE_RATE) const;
#endif  // MIC_FFT_ENABLED

private:
#if MIC_FFT_ENABLED
  float _re[MIC_FFT_SIZE];
  float _im[MIC_FFT_SIZE];
  float _mag[MIC_FFT_SIZE / 2 + 1];
  // Demi-table de Hann : w[i] = w[N-1-i], donc N/2 valeurs suffisent.
  float _window[MIC_FFT_SIZE / 2];
  bool _spectrumValid = false;

  void buildWindow();
  void fftInPlace();
  float windowAt(size_t i) const {
    return (i < MIC_FFT_SIZE / 2) ? _window[i] : _window[MIC_FFT_SIZE - 1 - i];
  }
#else
  void buildWindow() {}
#endif
};

#endif  // SPECTRAL_ANALYZER_H
