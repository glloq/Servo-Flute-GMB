/***********************************************************************************************
 * PitchDetector - Detection de pitch YIN, sans dependance materielle
 *
 * Extrait d'AudioAnalyzer pour que le coeur de detection se teste nativement sur
 * du PCM synthetique, sans I2S ni Arduino. Aucune allocation dynamique.
 *
 * POURQUOI IL N'Y A PLUS DE FENETRE (audit PHASE 0, defaut A0-3)
 * --------------------------------------------------------------
 * La version precedente appliquait une fenetre de Hann AVANT la fonction de
 * difference YIN. C'est une erreur de principe : YIN appartient a la famille de
 * l'autocorrelation, et il compare x[i] a x[i+tau]. Si le signal est multiplie
 * par une enveloppe, ces deux echantillons subissent des gains DIFFERENTS, donc
 *
 *     d(tau) = somme (w[i]*x[i] - w[i+tau]*x[i+tau])^2
 *
 * ne mesure plus la seule periodicite : il mesure aussi la pente de l'enveloppe,
 * qui croit avec tau. Le minimum de d(tau) est deplace, et l'interpolation
 * parabolique interpole une courbe biaisee. Mesure avant correction : un 440 Hz
 * PUR etait rendu a 435,7 Hz, soit -16,9 cents, avec une erreur pire-cas de
 * 23 cents alors qu'un pas de tau ne vaut que 23,6 cents a cette frequence -
 * autrement dit l'interpolation n'apportait rien.
 *
 * Le fenetrage reste utile, mais pour le chemin SPECTRAL (FFT / Goertzel), ou il
 * limite les fuites entre bins. Il n'a rien a faire dans un estimateur temporel.
 *
 * `detectWindowed()` conserve l'ancien comportement : il n'est PAS utilise en
 * production, il sert uniquement de reference a la comparaison A/B des tests,
 * pour que le gain soit demontre et non affirme.
 *
 * DC : inutile de retirer la composante continue avant YIN. Elle s'annule
 * d'elle-meme dans la difference : (x[i]-m) - (x[i+tau]-m) = x[i] - x[i+tau].
 * C'est ce qui permet a detect() de ne PAS modifier le tampon de l'appelant,
 * lequel reste disponible pour l'analyse spectrale de la meme frame.
 ***********************************************************************************************/
#ifndef PITCH_DETECTOR_H
#define PITCH_DETECTOR_H

#include <stdint.h>
#include <stddef.h>
#include "settings.h"

struct PitchResult {
  bool valid = false;        // confiance suffisante ET frequence dans la plage
  float hz = 0.0f;           // fondamentale detectee, 0 si aucune
  float confidence = 0.0f;   // periodicite YIN 0..1 (1 - aperiodicite)

  int midi = 0;              // note MIDI la plus proche, 0 si aucune
  float cents = 0.0f;        // ecart signe par rapport au temperament egal

  // Renseignes uniquement lorsqu'une note attendue a ete declaree
  // (setExpectedMidiNote). Sinon tous faux.
  bool expectedMatch = false;  // meme note que l'attendue, dans la tolerance
  bool octaveAbove = false;    // une ou plusieurs octaves AU-DESSUS de l'attendue
  bool octaveBelow = false;    // une ou plusieurs octaves EN DESSOUS

  // Stabilite 0..1 sur les dernieres trames analysees (1 = parfaitement stable).
  // Vaut 0 tant que l'historique n'est pas rempli.
  float stability = 0.0f;
};

// Classification d'une trame microphone brute (detection de presence robuste).
enum MicSignalClass {
  MIC_SIG_OK = 0,        // signal varie et dans la plage (micro present et sain)
  MIC_SIG_ALL_ZERO,      // tous les echantillons nuls (bus au repos / non cable)
  MIC_SIG_STUCK,         // non nul mais quasi constant (ligne bloquee / pas d'horloge)
  MIC_SIG_SATURATED      // ecrete en permanence
};

class PitchDetector {
public:
  PitchDetector() { resetTracking(); }

  // --- Detection ------------------------------------------------------------

  // Analyse PURE : ne modifie ni le tampon ni l'etat du detecteur. `stability`
  // n'est pas renseignee (elle demande un historique). Utilisee par les tests et
  // par detect().
  PitchResult analyse(const float* samples, size_t n) const;

  // Detection de production : analyse puis mise a jour de l'historique, ce qui
  // renseigne `stability`. Le tampon n'est PAS modifie.
  PitchResult detect(const float* samples, size_t n);

  // ANCIENNE methode : fenetre de Hann appliquee avant YIN, tampon modifie en
  // place. Conservee UNIQUEMENT comme reference de la comparaison A/B (voir
  // l'en-tete de ce fichier). Ne pas utiliser en production.
  PitchResult detectWindowed(float* samples, size_t n) const;

  // --- Note attendue (PHASE 2.2) --------------------------------------------
  // Quand la note visee est connue (calibration, note jouee), le detecteur
  // evalue EXPLICITEMENT les lags correspondant a f0/2, f0, 2*f0 et 3*f0 et
  // retient le meilleur. Cela leve l'ambiguite d'octave de facon deterministe,
  // au lieu de dependre du premier creux rencontre. Sans note attendue, le
  // comportement general est strictement inchange.
  void setExpectedMidiNote(int midi);
  void clearExpectedMidiNote() { _expectedMidi = 0; }
  int expectedMidiNote() const { return _expectedMidi; }
  bool hasExpectedNote() const { return _expectedMidi > 0; }

  // --- Suivi temporel -------------------------------------------------------
  // Vide l'historique de stabilite. A appeler sur changement de note, sur
  // silence, ou entre deux mesures sans rapport.
  void resetTracking();

  // --- Utilitaires purs -----------------------------------------------------

  // RMS des echantillons, composante continue retiree. Ne modifie pas le tampon.
  static float rms(const float* samples, size_t n);

  // Classe une trame I2S brute (INMP441 : 24 bits cales a gauche sur 32).
  static MicSignalClass classifyRaw(const int32_t* raw, size_t n);

private:
  mutable float _yinBuf[MIC_YIN_TAU_MAX + 2];

  int _expectedMidi;
  float _expectedHz;

  // Historique des derniers pitchs valides, en cents absolus (MIDI*100 + cents),
  // pour calculer la stabilite sans dependre de la note.
  float _history[MIC_PITCH_HISTORY];
  uint8_t _histCount;
  uint8_t _histIdx;

  // Coeur commun : remplit _yinBuf et choisit le lag. `applyWindow` n'existe que
  // pour la reference A/B.
  PitchResult runYin(const float* samples, size_t n) const;
  void annotate(PitchResult& r) const;   // midi, cents, relations d'octave
};

#endif  // PITCH_DETECTOR_H
