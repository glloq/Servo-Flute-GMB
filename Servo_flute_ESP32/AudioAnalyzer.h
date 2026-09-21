/***********************************************************************************************
 * AudioAnalyzer - I2S INMP441 microphone driver with real-time audio analysis
 *
 * Provides:
 * - Acquisition continue : le DMA est vide dans un anneau (AudioRingBuffer) et
 *   l'analyse n'extrait une frame que lorsque MIC_ANALYSIS_FRAME_SIZE
 *   echantillons sont REELLEMENT disponibles. Une lecture I2S partielle ne
 *   constitue plus une frame (voir AUDIO_ARCHITECTURE.md, defauts A0-1 / A0-2).
 * - Recouvrement configurable (MIC_ANALYSIS_HOP_SIZE) entre frames successives.
 * - Niveau complet : RMS, crete, dBFS, decalage continu, ratio d'ecretage.
 * - Compteurs d'acquisition exposes au diagnostic (lectures partielles,
 *   debordements, echantillons perdus).
 * - I2S DMA-based microphone input (INMP441 MEMS mic, 32-bit words, 32 kHz).
 *   Works on both the ESP-IDF 5.x "std" I2S driver and the legacy IDF 4.x driver
 *   (selected at compile time), so it builds on Arduino-ESP32 2.0.x and 3.0.x.
 * - Robust microphone presence detection (all-zero / stuck / saturated / ok).
 * - DC-offset removal + RMS level, plus pitch detection delegated to the
 *   hardware-free PitchDetector (YIN + Hann + confidence), so the pitch core is
 *   unit-tested natively.
 * - MIDI note + cents deviation, YIN confidence, pitch validity.
 * - Frame freshness (sequence + timestamp) so consumers never count a frame twice
 *   and can detect a frozen source.
 *
 * Implements IAudioSource so AutoCalibrator consumes measurements without an I2S
 * dependency.
 ***********************************************************************************************/
#ifndef AUDIO_ANALYZER_H
#define AUDIO_ANALYZER_H

#include <Arduino.h>
#include "settings.h"
#include "IAudioSource.h"
#include "PitchDetector.h"
#include "AudioRingBuffer.h"
#include "AudioLevel.h"
#include "SpectralAnalyzer.h"
#include "AcousticFeatures.h"
#include "AcousticQuality.h"
#include "AcousticTiming.h"
#include "AudioFilters.h"
#include "NoiseModel.h"

#if MIC_ENABLED

#include <esp_idf_version.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  #include <driver/i2s_std.h>
  #define MIC_I2S_STD_DRIVER 1
#else
  #include <driver/i2s.h>
  #define MIC_I2S_STD_DRIVER 0
#endif

// Detailed microphone status (mirrors PitchDetector::MicSignalClass plus a
// read-error state that only the driver can report).
enum MicStatus {
  MIC_STATUS_DETECTED = 0,
  MIC_STATUS_ALL_ZERO,
  MIC_STATUS_STUCK,
  MIC_STATUS_SATURATED,
  MIC_STATUS_READ_ERROR,
  MIC_STATUS_NOT_INIT
};

class AudioAnalyzer : public IAudioSource {
public:
  AudioAnalyzer();

  bool begin();
  void end();
  void update();

  // Re-probe / re-initialise the microphone without rebooting the ESP32.
  bool resetMicrophone();
  MicStatus getMicStatus() const { return _micStatus; }
  const char* getMicStatusString() const;

  // --- IAudioSource ---
  bool isMicDetected() const override { return _micDetected; }
  bool isSoundDetected() const override { return _soundDetected; }
  float getRMS() const override { return _rms; }
  float getPitchHz() const override { return _pitchHz; }
  int getPitchMidi() const override { return _pitchMidi; }
  float getPitchCents() const override { return _pitchCents; }
  float getPitchConfidence() const override { return _pitchConfidence; }
  bool isPitchValid() const override { return _pitchValid; }

  // --- Niveau et acquisition (PHASE 1) ---
  const FrameLevel& getLevel() const { return _level; }
  float getRmsDbFS() const { return _level.rmsDbFS; }
  float getPeakDbFS() const { return _level.peakDbFS; }
  bool isClipping() const { return _level.clippingDetected; }
  float getClippingRatio() const { return _level.clippingRatio; }
  const AudioCaptureStats& getCaptureStats() const { return _stats; }
  void resetCaptureStats() { _stats.reset(); }

  // --- Pitch enrichi (PHASE 2) ---
  const PitchResult& getPitchResult() const { return _lastPitch; }
  float getPitchStability() const { return _lastPitch.stability; }
  // Note visee : permet au detecteur de lever l'ambiguite d'octave de facon
  // deterministe (f0/2, f0, 2*f0, 3*f0). Pose par l'auto-calibration.
  // CHANGER la note visee, ou la retirer, vaut declaration de changement de
  // note : les deux appellent resetAcousticTracking() sur TRANSITION, sans
  // quoi la ligne de base de brillance et l'historique de pitch de la note
  // precedente serviraient a juger la suivante. Voir le corps dans le .cpp.
  void setExpectedMidiNote(int midi) override;
  void clearExpectedMidiNote() override;

  // --- Descripteurs acoustiques (PHASE 4) ---
  // Etat complet de la DERNIERE frame analysee. Les champs spectraux ne sont
  // renseignes qu'une frame sur MIC_SPECTRAL_DECIMATION : verifier
  // `spectralValid` avant de les lire.
  const AcousticFeatures& getFeatures() const { return _features; }
  float getSNR() const { return _features.snrDb; }
  const SpectralAnalyzer& getSpectral() const { return _spectral; }

  // --- Modele de bruit (PHASE 5) ---
  // Declare l'etat REEL de la source d'air. Le rapport signal/bruit est alors
  // calcule contre le profil de CET etat : comparer une note jouee pompe en
  // marche a un plancher mesure pompe arretee surestimerait sa qualite.
  void setAirSourceState(uint8_t airMode, uint8_t pumpPercent, uint8_t fanPercent);
  NoiseProfileId currentNoiseProfile() const { return _noiseProfileId; }

  // Capture du profil de bruit de l'etat courant. L'appelant est responsable de
  // mettre l'instrument dans cet etat ET de garantir qu'aucune note ne sonne.
  void beginNoiseCapture();
  bool endNoiseCapture();
  bool isCapturingNoise() const { return _noise.isCapturing(); }
  // Vrai lorsqu'une capture s'est terminee D'ELLE-MEME au plafond de duree,
  // sans que personne n'ait envoye de "stop". L'appelant peut alors arreter
  // l'analyseur plutot que de laisser tout le DSP tourner pour rien.
  bool noiseCaptureFinished() const { return _noiseCaptureFinished; }
  const NoiseModel& getNoiseModel() const { return _noise; }
  void resetNoiseModel() { _noise.reset(); }

  // --- Classification et note de qualite (PHASE 6) --------------------------
  // Verdicts de la DERNIERE frame analysee. Ils ne survivent pas a la mesure
  // qui les a produits : des qu'une frame n'est plus fraiche
  // (markMeasurementInvalid) ou que la note visee change
  // (resetAcousticTracking), ils redeviennent invalides au lieu de rester ceux
  // de la derniere frame reussie. Lire `classified` / `valid` AVANT le contenu.
  // RESERVE : "la note visee change" n'est pas "la note change". Un changement
  // de note joue sans note visee declaree - le cas de la lecture MIDI
  // ordinaire - n'invalide rien du tout ; voir resetAcousticTracking() plus
  // bas pour ce que cela coute et pour le hook qui manque.
  const AcousticClassification& getClassification() const { return _classification; }
  const QualityScore&           getQualityScore() const { return _quality; }
  // La respiration et le couac sont calcules PAR la classification, qui les
  // range dans son propre resultat : ces deux accesseurs pointent donc la
  // meme mesure, jamais une seconde mesure faite a part.
  const BreathinessResult&      getBreathiness() const { return _classification.breathiness; }
  const SqueakResult&           getSqueak() const { return _classification.squeak; }
  // Rend "unclassified" tant qu'aucun verdict n'a pu etre rendu :
  // AcousticClassification::state garde alors sa valeur par defaut
  // (ACOUSTIC_SILENCE), qui ne doit pas etre affichee comme un etat mesure.
  const char*                   getAcousticStateName() const;
  // Note changee / reprise apres pause. Remet a zero le detecteur de couac ET
  // l'historique de pitch, et invalide les verdicts de la note precedente. Ne
  // touche PAS a AcousticTiming, dont le cycle est pilote par les ordres
  // d'actionneur.
  //
  // QUI L'APPELLE, EXACTEMENT - la liste, pas une intention :
  //   - setActive(true) : reprise apres une pause de duree inconnue ;
  //   - setExpectedMidiNote() / clearExpectedMidiNote(), sur TRANSITION de la
  //     note visee. Seul AutoCalibrator declare une note visee.
  //
  // CE QUI MANQUE, ET IL FAUT LE LIRE AVANT DE SE FIER AUX VERDICTS : en
  // lecture MIDI ordinaire personne ne declare de note visee (_expectedMidi
  // vaut 0), donc AUCUN appel ci-dessus ne survient a un changement de note
  // joue. Un legato montant d'une octave fait alors comparer la premiere frame
  // de la note nouvelle a la brillance de l'ancienne : le detecteur de couac
  // publie ACOUSTIC_SQUEAK - troisieme dans l'ordre de priorite, donc il
  // masque tout ce qui suit - pendant quelques frames sur une note propre, et
  // `stability` tombe a zero AVEC `stabilityValid` vrai, l'historique
  // traversant les deux notes. Le signal manquant ne peut PAS etre fabrique
  // ici : le deviner depuis le pitch mesure ferait d'une observation une
  // cause. Il doit venir de la chaine d'actionneurs, qui seule sait quand
  // l'ordre part : NoteSequencer notifie deja la chronometrie aux deux bornes
  // de la note, et il manque a ces deux notifications un observateur audio.
  void                          resetAcousticTracking();

  // --- Chronometrie acoustique (PHASE 7) ------------------------------------
  // Alimentee par l'analyse a chaque frame ; les instants d'ORDRE lui sont
  // donnes par la chaine d'actionneurs (noteCommanded, airCommanded,
  // valveOpened, noteReleased). Le flux est a SENS UNIQUE : rien dans cette
  // classe ne lit ses verdicts pour decider quoi que ce soit.
  AcousticTiming&               timing() { return _timing; }
  const AcousticTiming&         timing() const { return _timing; }

  // Chaine de filtrage appliquee au flux (voir AudioFilters.h).
  const AudioFilterChain& getFilters() const { return _filters; }
  bool isActive() const override { return _active; }
  // Mise en pause / reprise de l'analyse. Ce n'est pas un simple drapeau :
  // update() sort des sa premiere ligne quand l'analyseur est inactif, donc
  // aucun plafond d'obsolescence ne s'appliquerait pendant la pause. Voir le
  // corps dans AudioAnalyzer.cpp.
  void setActive(bool active) override;
  uint32_t getFrameSequence() const override { return _frameSeq; }
  unsigned long getFrameTimestamp() const override { return _frameTimestamp; }

private:
  bool _active;
  bool _initialized;
  bool _micDetected;
  bool _soundDetected;
  MicStatus _micStatus;
  float _rms;
  float _pitchHz;
  int _pitchMidi;
  float _pitchCents;
  float _pitchConfidence;
  bool _pitchValid;
  uint32_t _frameSeq;
  unsigned long _frameTimestamp;

#if MIC_I2S_STD_DRIVER
  i2s_chan_handle_t _rxHandle;
#endif

  PitchDetector _pitch;
  PitchResult _lastPitch;
  SpectralAnalyzer _spectral;
  AcousticFeatures _features;
  AudioFilterChain _filters;
  // --- PHASE 6 : etat persistant de la classification -----------------------
  // Le detecteur de couac est le SEUL etat qui traverse les frames : il lui
  // faut un historique de brillance pour distinguer un accident bref d'un
  // defaut installe. Il appartient a l'analyseur, pas a une variable statique
  // cachee dans AcousticQuality.
  SqueakDetector _squeak;
  AcousticClassification _classification;
  QualityScore _quality;
  // --- PHASE 7 --------------------------------------------------------------
  AcousticTiming _timing;
  NoiseModel _noise;
  NoiseProfileId _noiseProfileId;
  bool _noiseCaptureFinished;
  // Ecretage compte sur les echantillons BRUTS, avant filtrage, depuis la
  // derniere frame analysee.
  uint32_t _rawSamplesSinceFrame;
  uint32_t _clippedSinceFrame;
  int _expectedMidi;          // 0 = aucune note visee declaree
  uint8_t _spectralCountdown; // decimation de l'analyse spectrale
  FrameLevel _level;
  AudioCaptureStats _stats;
  AudioRingBuffer _ring;
  // Tampon de transfert I2S -> anneau. Petit (1 ko) et reutilise : il remplace
  // l'ancien _rawBuffer[1024] + _analysisBuffer[1024] (8 ko), car on n'a plus
  // besoin de lire une frame entiere en une fois.
  int32_t _chunk[MIC_I2S_CHUNK_SAMPLES];
  float _chunkFloat[MIC_I2S_CHUNK_SAMPLES];
  // Frame d'analyse contigue extraite de l'anneau. Toujours COMPLETE.
  float _frame[MIC_ANALYSIS_FRAME_SIZE];
  unsigned long _lastDrain;

  bool installI2S();
  void uninstallI2S();
  bool detectMicrophone();       // reads a probe buffer and sets _micStatus
  // Vide le DMA dans l'anneau. Appelee beaucoup plus souvent que l'analyse.
  void drainI2S();
  // Analyse UNE frame complete deja extraite dans _frame.
  void analyzeFrame();
  // Descripteurs spectraux : Goertzel a chaque frame ou la fondamentale est
  // connue, FFT une frame sur MIC_SPECTRAL_DECIMATION.
  void analyzeSpectrum();
  // PHASE 6/7 : classification, note de qualite et chronometrie d'UNE frame.
  // Appelee APRES que la frame ait recu son numero et son horodatage - le
  // detecteur de couac compte les frames par leur SEQUENCE et la chronometrie
  // date les siennes par leur horodatage : les lui donner avant leur mise a
  // jour ferait travailler les deux sur l'identite de la frame PRECEDENTE.
  void analyzeAcoustics();
  void markMeasurementInvalid();
};

#endif // MIC_ENABLED
#endif // AUDIO_ANALYZER_H
