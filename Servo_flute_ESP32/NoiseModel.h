/***********************************************************************************************
 * NoiseModel - Profils de bruit par etat de fonctionnement de l'instrument
 *
 * Sans dependance materielle : la classe se teste entierement sur hote.
 *
 * POURQUOI PLUSIEURS PROFILS
 * --------------------------
 * L'auto-calibration mesurait jusqu'ici UN plancher de bruit par note, valve
 * fermee et air au repos. C'est insuffisant, pour une raison simple : la
 * machinerie de la flute FAIT PARTIE du bruit, et son niveau depend du point de
 * fonctionnement. Une pompe a 90 % ne fait pas le meme bruit qu'a l'arret, ni le
 * meme spectre. Un rapport signal/bruit calcule contre un plancher mesure pompe
 * arretee surestime donc la qualite de toute note jouee pompe en marche - c'est
 * a dire de toutes les notes.
 *
 * Chaque profil decrit donc UN etat : ambiance seule, pompe au ralenti, pompe a
 * mi-regime, pompe a fond, et les equivalents ventilateur.
 *
 * CE QUI EST STOCKE, ET CE QUI NE L'EST PAS
 * -----------------------------------------
 * Aucun PCM n'est conserve. Un profil ne retient que des STATISTIQUES : niveau
 * moyen, energie par bande, platitude spectrale, pic dominant. Une seconde
 * d'audio brut couterait 128 ko ; un profil complet en coute environ 48 octets.
 *
 * CE QUI N'EST PAS MESURE N'EST PAS INVENTE
 * -----------------------------------------
 * Un profil non capture reste `valid = false`, et snrDb() le dit au lieu de
 * rendre un rapport calcule contre un plancher imaginaire. Le repli vers le
 * profil d'ambiance est EXPLICITE et signale par la valeur de retour.
 *
 * PERSISTANCE
 * -----------
 * Les profils vivent en RAM et sont perdus au redemarrage. Les rendre
 * persistants demande le versionnement du format de configuration, qui est du
 * ressort d'une phase ulterieure ; le faire ici creerait un format a migrer
 * deux fois.
 ***********************************************************************************************/
#ifndef NOISE_MODEL_H
#define NOISE_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include "settings.h"
#include "SpectralAnalyzer.h"

// Etats de fonctionnement distingues. L'ordre est celui d'un bruit croissant.
enum NoiseProfileId : uint8_t {
  NOISE_AMBIENT = 0,     // instrument silencieux : bruit de la piece et du micro
  NOISE_PUMP_IDLE,
  NOISE_PUMP_MEDIUM,
  NOISE_PUMP_HIGH,
  NOISE_FAN_IDLE,
  NOISE_FAN_MEDIUM,
  NOISE_FAN_HIGH,
  NOISE_PROFILE_COUNT
};

// Statistiques d'un etat. Aucun echantillon, uniquement des agregats.
struct NoiseProfile {
  bool valid = false;
  uint16_t frames = 0;               // frames accumulees dans la moyenne
  float rms = 0.0f;                  // niveau lineaire moyen
  float rmsDbFS = MIC_DBFS_FLOOR;
  float flatness = 0.0f;             // platitude spectrale moyenne 0..1
  float bands[MIC_NOISE_BANDS] = {}; // energie moyenne par bande
  float peakHz = 0.0f;               // frequence du bin dominant
  float peakMag = 0.0f;
};

// Resultat d'une demande de rapport signal/bruit. `usedFallback` dit qu'aucun
// profil n'existait pour l'etat demande et que l'ambiance a servi de repli ;
// `valid` faux signifie qu'aucun profil exploitable n'existait du tout.
struct SnrResult {
  bool valid = false;
  bool usedFallback = false;
  float db = 0.0f;
  NoiseProfileId source = NOISE_AMBIENT;
};

class NoiseModel {
public:
  NoiseModel() { reset(); }

  // Efface tous les profils.
  void reset();

  // --- Capture --------------------------------------------------------------
  // Demarre l'accumulation dans `id`. Le profil precedent de cet etat est
  // ecrase : on recalibre volontairement, on ne moyenne pas avec une mesure
  // faite dans d'autres conditions.
  void beginCapture(NoiseProfileId id);
  bool isCapturing() const { return _capturing; }
  NoiseProfileId capturingId() const { return _captureId; }

  // Accumule UNE frame. `spectral` peut etre nul : seules les statistiques de
  // niveau sont alors renseignees, les bandes restent a zero.
  void accumulate(const float* frame, size_t n, const SpectralAnalyzer* spectral);

  // Termine la capture. Le profil n'est declare valide que si au moins
  // MIC_NOISE_MIN_FRAMES frames ont ete accumulees : une mesure faite sur deux
  // frames ne decrit rien.
  bool endCapture();
  void abortCapture() { _capturing = false; }

  // --- Consultation ---------------------------------------------------------
  const NoiseProfile& profile(NoiseProfileId id) const;
  bool hasProfile(NoiseProfileId id) const;
  uint8_t capturedCount() const;

  // Rapport signal/bruit d'un niveau mesure contre le profil de l'etat donne.
  SnrResult snrDb(float signalRms, NoiseProfileId id) const;

  // --- Selection de l'etat --------------------------------------------------
  // Traduit l'etat REEL de l'instrument en identifiant de profil. C'est le
  // point qui permet de comparer une note a son propre bruit de fond, et non a
  // celui d'un autre regime.
  static NoiseProfileId profileForState(uint8_t airMode, uint8_t pumpPercent,
                                        uint8_t fanPercent);

  // Bornes de la bande `index` (0..MIC_NOISE_BANDS-1), en Hz.
  static float bandLowHz(uint8_t index);
  static float bandHighHz(uint8_t index);
  static const char* profileName(NoiseProfileId id);

private:
  NoiseProfile _profiles[NOISE_PROFILE_COUNT];
  NoiseProfile _accum;              // profil en cours de construction
  NoiseProfileId _captureId = NOISE_AMBIENT;
  bool _capturing = false;
};

#endif  // NOISE_MODEL_H
