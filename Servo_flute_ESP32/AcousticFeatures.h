/***********************************************************************************************
 * AcousticFeatures - Descripteurs acoustiques d'UNE frame d'analyse
 *
 * Structure de donnees pure, sans methode virtuelle ni allocation : c'est le
 * point de rendez-vous entre la chaine de mesure (acquisition, pitch, spectre)
 * et tous ses consommateurs (auto-calibration, diagnostics, moniteur web).
 *
 * TOUT EST MESURE, RIEN N'EST DEVINE
 * ----------------------------------
 * Un champ vaut sa valeur par defaut tant que la mesure correspondante n'a pas
 * ete faite. En particulier les descripteurs spectraux ne sont renseignes qu'une
 * frame sur MIC_SPECTRAL_DECIMATION, et `spectralValid` dit laquelle. Un
 * consommateur qui lit `spectralCentroid` sans regarder `spectralValid` lirait
 * la valeur d'une frame anterieure : c'est a lui de verifier.
 *
 * UNITES
 * ------
 *   rms, peak            lineaire, 0..1, continu retire
 *   rmsDbFS, peakDbFS    dBFS - pleine echelle NUMERIQUE, JAMAIS du dB SPL
 *   pitchHz              Hz
 *   cents                ecart signe au temperament egal
 *   confidence,          0..1
 *   stability,
 *   flatness
 *   h2Ratio, h3Ratio     rapports de PUISSANCE a la fondamentale (pas d'amplitude)
 *   harmonicToNoiseRatio dB
 *   spectralCentroid     Hz
 *
 * TAILLE
 * ------
 * Les champs sont ordonnes et dimensionnes pour rester compacts : cette
 * structure peut etre copiee a chaque frame (62,5 fois par seconde) et, plus
 * tard, historisee par note.
 ***********************************************************************************************/
#ifndef ACOUSTIC_FEATURES_H
#define ACOUSTIC_FEATURES_H

#include <stddef.h>
#include <stdint.h>
#include "settings.h"
#include "AudioLevel.h"
#include "PitchDetector.h"
#include "SpectralAnalyzer.h"

struct AcousticFeatures {
  // --- Identite de la frame ------------------------------------------------
  uint32_t frameSequence = 0;     // incremente une fois par frame analysee
  uint32_t timestamp = 0;         // millis() au moment de l'analyse

  // --- Niveau (PHASE 1) ----------------------------------------------------
  float rms = 0.0f;
  float rmsDbFS = MIC_DBFS_FLOOR;
  float peakDbFS = MIC_DBFS_FLOOR;
  float clippingRatio = 0.0f;
  bool clipping = false;

  // --- Pitch (PHASE 2) -----------------------------------------------------
  float pitchHz = 0.0f;
  int16_t pitchMidi = 0;
  float cents = 0.0f;
  float pitchConfidence = 0.0f;
  float pitchStability = 0.0f;

  // --- Spectre (PHASE 3) ---------------------------------------------------
  // Renseignes une frame sur MIC_SPECTRAL_DECIMATION. TOUJOURS verifier ce
  // drapeau avant de lire les champs qui suivent.
  bool spectralValid = false;
  float fundamentalEnergy = 0.0f;
  float h2Ratio = 0.0f;
  float h3Ratio = 0.0f;
  float harmonicToNoiseRatio = 0.0f;   // dB
  float spectralCentroid = 0.0f;       // Hz
  float spectralFlatness = 0.0f;       // 0..1

  // --- Rapport signal / bruit (PHASE 5) ------------------------------------
  // Mesure contre le profil de bruit de l'etat REEL de la source d'air, et non
  // contre un plancher global. `snrValid` faux signifie qu'aucun profil
  // exploitable n'a encore ete capture ; `snrUsedFallback` que le profil de
  // l'etat demande manquait et que l'ambiance a servi de repli - le rapport
  // surestime alors probablement la qualite.
  bool snrValid = false;
  bool snrUsedFallback = false;
  float snrDb = 0.0f;
  uint8_t noiseProfile = 0;            // NoiseProfileId de l'etat courant

  // --- Verdicts elementaires ------------------------------------------------
  // Deliberement limites a ce qui se deduit directement des mesures ci-dessus.
  // La classification acoustique complete (souffle, couac, note faible...)
  // appartient a une phase ulterieure et ne doit pas etre devinee ici.
  bool soundDetected = false;
  bool expectedNoteDetected = false;
  bool overblowDetected = false;

  void reset() { *this = AcousticFeatures(); }
};

/***********************************************************************************************
 * Assemblage des descripteurs - fonctions PURES
 *
 * Volontairement separees d'AudioAnalyzer, qui depend de l'I2S et ne compile
 * donc pas sur hote. L'assemblage lui-meme, c'est-a-dire toute la logique qui
 * decide QUELS champs sont renseignes et COMMENT ils sont derives, se teste
 * ainsi integralement avec des signaux synthetiques.
 ***********************************************************************************************/
namespace AcousticFeatureBuilder {

// Champs de niveau (PHASE 1).
void fillLevel(AcousticFeatures& f, const FrameLevel& level);

// Champs de pitch (PHASE 2) et verdicts elementaires qui en decoulent.
void fillPitch(AcousticFeatures& f, const PitchResult& p, bool soundDetected);

// Champs spectraux (PHASE 3). `f0` doit etre une fondamentale FIABLE : sans
// elle, les champs sont remis a zero et `spectralValid` reste faux, plutot que
// de rendre une mesure derivee d'une frequence inventee.
// `spectral` peut etre nul : seules les mesures Goertzel sont alors faites.
void fillSpectral(AcousticFeatures& f, const float* frame, size_t n, float f0,
                  SpectralAnalyzer* spectral = nullptr, bool runFft = false);

}  // namespace AcousticFeatureBuilder

#endif  // ACOUSTIC_FEATURES_H
