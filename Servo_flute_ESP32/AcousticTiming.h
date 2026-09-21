/***********************************************************************************************
 * AcousticTiming - Analyse temporelle de l'attaque, du relachement et de la latence
 *
 * PHASE 7. Machine a etats PURE : aucune allocation, aucune dependance Arduino
 * ni I2S. Elle consomme une suite de frames deja analysees (niveau + pitch) et
 * des horodatages d'evenements fournis par l'appelant, et produit des mesures
 * temporelles. Elle se teste donc integralement sur hote en lui injectant des
 * frames synthetiques a des instants controles.
 *
 * CE QU'ELLE MESURE, ET CE QU'ELLE NE DEVINE PAS
 * ----------------------------------------------
 * Chaque mesure porte son propre drapeau de validite. Une mesure impossible -
 * le son n'est jamais apparu, l'ordre n'a jamais ete donne, la note a ete
 * coupee avant que son niveau s'etablisse - rend `valid = false`, JAMAIS un zero
 * ni une valeur plausible. Un zero se confondrait avec une latence nulle, ce qui
 * est precisement la valeur la plus trompeuse possible ici.
 *
 * RESOLUTION TEMPORELLE - a lire avant d'interpreter un chiffre
 * ------------------------------------------------------------
 * Une frame d'analyse arrive toutes les MIC_ANALYSIS_HOP_SIZE / MIC_SAMPLE_RATE
 * secondes, soit 16 ms (hop 512 a 32 kHz). Deux consequences, distinctes :
 *
 *   1. QUANTIFICATION. Sans traitement, tout instant detecte serait cale sur la
 *      grille des frames, donc quantifie a 16 ms. Ce module interpole le
 *      franchissement de seuil entre les deux frames qui l'encadrent, dans le
 *      domaine des dB (celui ou le critere est exprime).
 *
 *   2. ETALEMENT DE FENETRE. Le niveau d'une frame est une moyenne sur
 *      MIC_ANALYSIS_FRAME_SIZE echantillons, soit 32 ms, qui se TERMINENT a son
 *      horodatage. Une transition brusque n'apparait donc dans le RMS que
 *      progressivement, sur toute la duree de la fenetre.
 *
 * CE QUI A ETE MESURE (signal synthetique, voir test_timing.cpp)
 * -------------------------------------------------------------
 * Ces chiffres viennent du balayage de l'instant physique d'apparition sur une
 * periode de frame entiere. Ce ne sont pas des objectifs, ce sont des mesures.
 *
 *   soundOnset, avec interpolation ..... biais moyen -1 ms, erreur |e| <= 5 ms
 *   soundOnset, sans interpolation ..... biais moyen +9 ms, etendue 14 ms
 *
 * L'attaque est donc datee a mieux qu'un tiers de periode de frame, et l'apport
 * de l'interpolation est chiffre, pas affirme (setEnvelopeInterpolation(false)
 * rend la reference A/B). Le seuil d'attaque etant place bas sur la montee, et
 * l'interpolation remontant dans la frame precedente, l'etalement de fenetre ne
 * produit PAS ici le retard systematique qu'on pourrait attendre.
 *
 * Il en produit un, en revanche, sur les instants dont le critere exige que la
 * fenetre ait entierement franchi la transition :
 *
 *   levelEstablished ... +43 ms sur une attaque INSTANTANEE (~ une fenetre plus
 *                        une frame). attackTime ne peut donc pas valoir zero,
 *                        meme pour un echelon parfait, et c'est dit.
 *   soundRelease ....... +18 a +25 ms selon la pente d'extinction.
 *
 * Ces retards sont des BIAIS, connus et bornes, pas du bruit. Ils s'annulent en
 * partie dans les mesures qui soustraient deux instants detectes de la meme
 * facon (attackTime, pitchStabilizationTime) et pas du tout dans celles qui
 * comparent un instant detecte a un ordre horodate par le logiciel
 * (releaseTime, qui surestime donc d'environ 20 ms).
 *
 * DEBORDEMENT DE millis()
 * -----------------------
 * Les instants sont recus en `unsigned long` (le type de millis()) et convertis
 * a l'entree en uint32_t. Ce n'est pas un detail : `unsigned long` fait 32 bits
 * sur ESP32 et 64 bits sur l'hote, donc sans cette conversion un test sur hote
 * n'exercerait jamais le vrai repliement. Toutes les comparaisons passent
 * ensuite par (int32_t)(a - b), seule forme correcte de part et d'autre.
 ***********************************************************************************************/
#ifndef ACOUSTIC_TIMING_H
#define ACOUSTIC_TIMING_H

#include <stddef.h>
#include <stdint.h>
#include "settings.h"
#include "AudioLevel.h"

struct AcousticFeatures;   // adaptateur seulement : l'en-tete reste leger

/*==============================================================================
 * SEUILS D'INSTRUMENT - bloc unique, delibere, HORS settings.h
 *
 * Ces valeurs ne decrivent ni le materiel ni le dimensionnement memoire : elles
 * decrivent ce que cet instrument-ci considere comme une attaque, un niveau
 * etabli, une note eteinte. Elles se regleront a l'oreille et au banc, note par
 * note, quand une vraie flute sera branchee. Les mettre dans settings.h les
 * melangerait aux constantes de plateforme, qui elles ne se touchent pas.
 *
 * AUCUNE n'a ete validee contre un microphone reel. Ce sont des points de
 * DEPART justifies, pas des mesures.
 *============================================================================*/
namespace AcousticTimingCfg {

// --- Cadence d'analyse -------------------------------------------------------

// Periode entre deux frames, en ms. DERIVEE du materiel, jamais ecrite en dur :
// hop 512 a 32 kHz = 16 ms. C'est le pas de la grille sur laquelle tombent
// toutes les detections avant interpolation.
constexpr uint32_t kFramePeriodMs =
    (1000u * (uint32_t)MIC_ANALYSIS_HOP_SIZE) / (uint32_t)MIC_SAMPLE_RATE;
static_assert(kFramePeriodMs > 0, "hop trop petit : la periode de frame s'annule");

// Duree couverte par UNE frame (32 ms). Sert uniquement a documenter et a
// borner l'interpolation ; c'est la source du biais d'etalement decrit en tete.
constexpr uint32_t kFrameSpanMs =
    (1000u * (uint32_t)MIC_ANALYSIS_FRAME_SIZE) / (uint32_t)MIC_SAMPLE_RATE;

// --- 7.1 Detection d'attaque (onset) ----------------------------------------

// Montee de niveau, EN dB au-dessus du plancher mesure juste avant l'ordre, a
// partir de laquelle on declare que quelque chose sonne. Le critere est
// exprime en dB et non en lineaire parce que c'est le seul domaine ou il garde
// le meme sens quel que soit le gain du microphone : +12 dB, c'est un facteur 4
// en amplitude, que le plancher soit a -70 ou a -40 dBFS.
// 12 dB : bien au-dessus des quelques dB de fluctuation d'une pompe ou d'un
// ventilateur en regime, bien en dessous des 30 a 40 dB que couvre l'attaque
// complete d'une note - donc franchi tot dans la montee.
constexpr float kOnsetRiseDb = 12.0f;

// Plancher ABSOLU en dessous duquel une montee, si grande soit-elle, n'est pas
// un son. Sans lui, passer d'un silence numerique (-120 dBFS) a -100 dBFS
// serait une montee de 20 dB parfaitement inaudible. La valeur est DERIVEE de
// MIC_RMS_ABSOLUTE_MIN, que le firmware definit deja comme "en dessous, on
// n'appelle jamais cela du son" (0,002 -> environ -54 dBFS) : une seule
// definition de ce qu'est du son, pas deux.
inline float onsetFloorDbFS() { return AudioLevel::toDbFS(MIC_RMS_ABSOLUTE_MIN); }

// Frames consecutives au-dessus du seuil exigees pour CONFIRMER l'attaque.
// Une frame isolee peut etre un claquement de solenoide (SOLENOID_ACTIVATION_TIME_MS
// vaut 50 ms, largement de quoi remplir une frame). Deux frames, c'est 32 ms de
// montee soutenue. La confirmation ne coute RIEN en precision : l'instant
// retenu est celui du PREMIER franchissement, pas celui de la confirmation.
constexpr uint8_t kOnsetConfirmFrames = 2;

// Frames conservees pour estimer le plancher d'avant-note. 8 frames = 128 ms,
// la meme profondeur que MIC_PITCH_HISTORY : assez pour moyenner l'ondulation
// de la machinerie, assez court pour suivre un changement de regime d'air.
constexpr uint8_t kBaselineFrames = 8;

// --- Niveau etabli (fin de l'attaque) ---------------------------------------

// Tolerance du critere de plateau : le niveau est dit ETABLI quand son
// amplitude crete-a-crete reste sous cette valeur sur kAttackStableFrames
// frames consecutives. 1,5 dB est au-dessus de l'ondulation frame a frame d'un
// son stable mesure sur 32 ms, et sous les ~3 dB d'un ecart audible.
constexpr float kAttackSettleDb = 1.5f;

// Largeur du plateau exige : 4 frames, qui couvrent 3 intervalles, soit 48 ms.
// Consequence assumee et non contournable : une attaque qui monte plus
// LENTEMENT que kAttackSettleDb / ((kAttackStableFrames - 1) * kFramePeriodMs),
// soit 31 dB/s, est indiscernable d'un plateau et sera declaree etablie trop
// tot. Mesure sur une montee lineaire de 200 ms : l'attaque est rendue a
// 184 ms, soit environ 50 ms trop court, parce que le critere se declenche
// pendant l'approche asymptotique du palier. Une attaque de flute est bien plus
// rapide que cela, mais la limite existe et doit etre lue avec le chiffre.
constexpr uint8_t kAttackStableFrames = 4;

// --- Stabilisation du pitch --------------------------------------------------

// Ecart maximal, en cents, entre les pitchs d'une fenetre pour la dire stable.
// 25 cents est un quart de ton : plus serre que MIC_EXPECTED_TOLERANCE_CENTS
// (35), qui juge la JUSTESSE, alors qu'ici on juge la TENUE.
constexpr float kPitchStableCents = 25.0f;

// Frames consecutives dans cette fourchette. 4 frames = 64 ms. C'est le
// plancher incompressible de pitchStabilizationTime, et il est explicite.
// (Ce critere est calcule ici a partir des pitchs recus, et NON repris de
// PitchResult::stability, qui exige MIC_PITCH_HISTORY = 8 frames = 128 ms avant
// de valoir autre chose que zero : le plancher serait deux fois plus haut et
// bien moins lisible.)
constexpr uint8_t kPitchStableFrames = 4;

// --- 7.2 Detection de relachement -------------------------------------------

// Chute, en dB sous le niveau de reference de la note, a partir de laquelle le
// son est considere comme disparu. 20 dB est un facteur 10 en amplitude : pour
// un auditeur, la note n'est plus la. Le critere est relatif au niveau de la
// note elle-meme, donc independant du gain et du plancher de bruit.
constexpr float kReleaseFallDb = 20.0f;

// HYSTERESIS. Un relachement en cours n'est ANNULE que si le niveau remonte au
// moins de cette valeur AU-DESSUS du seuil de chute. Sans elle, un signal qui
// ondule de quelques dixieme de dB autour du seuil annulerait et relancerait le
// comptage sans fin, et le relachement ne serait jamais declare. 3 dB, soit un
// facteur 2 en puissance : une remontee franche, pas une ondulation.
constexpr float kReleaseHysteresisDb = 3.0f;

// Frames consecutives sous le seuil exigees pour confirmer. Comme pour
// l'attaque, l'instant retenu est celui du PREMIER franchissement.
constexpr uint8_t kReleaseConfirmFrames = 2;

// --- Plafonds : aucune mesure ne reste "en cours" indefiniment ---------------

// Attente maximale de l'apparition du son apres l'ordre. Le chemin mecanique
// complet (SERVO_TO_SOLENOID_DELAY_MS = 105, SOLENOID_ACTIVATION_TIME_MS = 50,
// AUTOCAL_AIR_SETTLE_MS = 120) tient dans ~300 ms ; 1500 ms laisse un facteur 5
// et, au-dela, la conclusion n'est plus "c'est lent" mais "la note n'a pas
// sonne".
constexpr uint32_t kOnsetTimeoutMs = 1500;

// Attente maximale de l'etablissement du niveau apres l'apparition. Au-dela, le
// niveau n'a pas de palier : attackTime reste INVALIDE, mais la note continue
// d'etre suivie - son relachement, lui, reste mesurable.
constexpr uint32_t kAttackTimeoutMs = 1000;

// Attente maximale de la stabilisation du pitch apres l'apparition. Au-dela,
// pitchStabilizationTime reste INVALIDE : un pitch qui n'a pas tenu 64 ms en
// deux secondes ne s'est pas stabilise.
constexpr uint32_t kPitchTimeoutMs = 2000;

// Attente maximale de la disparition du son apres l'ordre d'arret. C'est le
// plafond le plus important du lot : une valve bloquee laisse la flute sonner
// indefiniment, et sans ce plafond la mesure resterait "en cours" pour toujours.
constexpr uint32_t kReleaseTimeoutMs = 2000;

// Duree maximale d'un suivi de note. N'existe PAS pour juger la note - une note
// tenue longtemps est legitime - mais pour borner la machine a etats : un
// Note Off perdu (coupure BLE, client parti) ne doit pas la laisser armee
// jusqu'au redemarrage. 60 s depasse tres largement toute note jouable.
constexpr uint32_t kNoteMaxDurationMs = 60000;

// Ecart maximal entre deux frames au-dela duquel on n'interpole PAS le
// franchissement. 3 periodes de frame : au-dela, des frames ont ete perdues et
// la forme de l'enveloppe entre les deux est inconnue - l'interpoler
// reviendrait a l'inventer. On retombe alors sur l'instant de la frame qui a
// franchi, donc sur la quantification a 16 ms, ce qui est honnete.
constexpr uint32_t kMaxInterpolationGapMs = 3u * kFramePeriodMs;

}  // namespace AcousticTimingCfg

/*==============================================================================
 * Types publics
 *============================================================================*/

// Une mesure de duree. `valid` faux signifie NON MESUREE : `ms` n'a alors
// aucune signification et ne doit pas etre lu. C'est deliberement un type a
// part, pour qu'aucun appelant ne puisse lire une duree sans avoir vu son
// drapeau.
struct TimingMeasure {
  bool valid = false;
  uint32_t ms = 0;
};

// Etat de la machine. Sert au diagnostic et aux tests ; les resultats se lisent
// dans NoteTiming.
enum TimingState : uint8_t {
  TIMING_IDLE = 0,       // aucune note suivie
  TIMING_WAIT_ONSET,     // ordre donne, le son n'est pas encore apparu
  TIMING_ATTACK,         // son apparu, niveau pas encore etabli
  TIMING_SUSTAIN,        // niveau etabli
  TIMING_RELEASING       // ordre d'arret donne, le son n'a pas encore disparu
};

// Comment le suivi d'une note s'est TERMINE. Decrit le chemin parcouru, pas la
// validite des mesures : chaque mesure porte son propre drapeau. Un
// TIMING_COMPLETE avec un attackTime invalide se lit tres bien - "la note a
// sonne puis s'est tue, mais son niveau n'a jamais fait de palier".
enum TimingOutcome : uint8_t {
  TIMING_OUTCOME_NONE = 0,   // rien n'a encore ete suivi
  TIMING_IN_PROGRESS,        // suivi en cours
  TIMING_COMPLETE,           // apparue, etablie, puis disparue apres l'ordre
  TIMING_NO_SOUND,           // jamais apparue : plafond atteint ou coupee avant
  TIMING_CUT_SHORT,          // coupee avant que le niveau soit etabli
  TIMING_TIMEOUT,            // ABANDON explicite : un plafond a ete atteint
  TIMING_ABORTED             // remplacee par une nouvelle note, ou reset()
};

// Releve complet d'une note. Les horodatages sont en ms (millis() replie sur
// 32 bits) et ne valent que si leur drapeau `has*` est vrai.
struct NoteTiming {
  TimingOutcome outcome = TIMING_OUTCOME_NONE;

  // --- Horodatages d'ORDRE : fournis par l'appelant, jamais devines ---------
  bool hasNoteCommand = false;      uint32_t noteCommandTimestamp = 0;
  bool hasAirCommand = false;       uint32_t airCommandTimestamp = 0;
  bool hasValveOpen = false;        uint32_t valveOpenTimestamp = 0;
  bool hasReleaseCommand = false;   uint32_t releaseCommandTimestamp = 0;

  // --- Horodatages MESURES sur le signal -----------------------------------
  bool hasSoundOnset = false;       uint32_t soundOnsetTimestamp = 0;
  bool hasLevelEstablished = false; uint32_t levelEstablishedTimestamp = 0;
  bool hasPitchDetected = false;    uint32_t pitchDetectedTimestamp = 0;
  bool hasPitchStable = false;      uint32_t pitchStableTimestamp = 0;
  bool hasSoundRelease = false;     uint32_t soundReleaseTimestamp = 0;

  // --- Mesures --------------------------------------------------------------
  TimingMeasure commandToSoundLatency;    // ordre MIDI -> son audible
  TimingMeasure airToSoundLatency;        // consigne d'air -> son audible
  TimingMeasure attackTime;               // apparition -> niveau etabli
  TimingMeasure pitchStabilizationTime;   // apparition -> pitch stable
  TimingMeasure releaseTime;              // ordre d'arret -> disparition

  // --- Contexte de la mesure, pour pouvoir la relire ------------------------
  // `baselineValid` faux signifie qu'aucune frame n'avait ete vue avant
  // l'ordre : le critere relatif etait alors impossible et seul le plancher
  // absolu a servi. C'est un REPLI EXPLICITE, comme celui de NoiseModel.
  bool baselineValid = false;
  float baselineDbFS = MIC_DBFS_FLOOR;      // plancher d'avant-note
  float onsetThresholdDbFS = MIC_DBFS_FLOOR;// seuil reellement applique
  float referenceDbFS = MIC_DBFS_FLOOR;     // niveau de reference du relachement

  void reset() { *this = NoteTiming(); }
};

// Une frame deja analysee, reduite a ce dont l'analyse temporelle a besoin.
// Volontairement minimale : la machine a etats ne depend d'aucune structure de
// la chaine audio, ce qui permet de la piloter frame par frame dans les tests.
struct TimingFrame {
  // Horodatage de la frame. Meme convention qu'AcousticFeatures::timestamp :
  // c'est l'instant de l'ANALYSE, donc la FIN de la fenetre de 32 ms - d'ou le
  // biais d'etalement decrit en tete de fichier.
  uint32_t timestampMs = 0;

  float rmsDbFS = MIC_DBFS_FLOOR;   // niveau de la frame, en dBFS

  bool  pitchValid = false;         // le pitch de cette frame est-il fiable
  float pitchHz = 0.0f;             // n'a de sens que si pitchValid
};

/*==============================================================================
 * La machine a etats
 *============================================================================*/
class AcousticTiming {
public:
  AcousticTiming() { reset(); }

  // Efface tout, y compris le dernier releve. Une note en cours est PERDUE, pas
  // rangee : reset() est une remise a zero, pas une fin de note.
  void reset();

  // --- Evenements d'ORDRE ---------------------------------------------------
  // Tous prennent l'instant en `unsigned long`, le type de millis(). Un ordre
  // recu dans un etat ou il n'a pas de sens est REFUSE et compte, jamais
  // silencieusement applique.

  // Ordre MIDI parti. Demarre un suivi. Si un suivi etait en cours, il est
  // clos en TIMING_ABORTED : une note en remplace une autre, elle ne la
  // prolonge pas.
  void noteCommanded(unsigned long nowMs);

  // Consigne d'air posee. Acceptee uniquement entre l'ordre MIDI et
  // l'apparition du son, et jamais anterieure a l'ordre MIDI.
  bool airCommanded(unsigned long nowMs);

  // Valve ouverte. Memes conditions. Aucune des cinq mesures demandees ne
  // consomme cet instant ; il est releve parce qu'il documente le chemin
  // mecanique et qu'il n'a de sens que mesure ici.
  bool valveOpened(unsigned long nowMs);

  // Ordre d'arret : Note Off, ou fermeture de valve - le premier des deux qui
  // commande reellement l'extinction. C'est l'origine de releaseTime.
  bool noteReleased(unsigned long nowMs);

  // --- Flux d'analyse -------------------------------------------------------

  // Injecte UNE frame. Rend false si la frame est refusee (temps qui recule).
  bool update(const TimingFrame& f);

  // Adaptateur depuis la chaine audio. `pitchValid` y est reconstruit selon le
  // meme critere que PitchResult::valid, qu'AcousticFeatures n'expose pas.
  static TimingFrame fromFeatures(const AcousticFeatures& f);

  // --- Resultats ------------------------------------------------------------
  TimingState state() const { return _state; }
  bool active() const { return _state != TIMING_IDLE; }

  // Releve de la note EN COURS. Vide entre deux notes.
  const NoteTiming& current() const { return _note; }

  // Dernier releve TERMINE. `hasLast()` faux tant qu'aucune note n'est close.
  const NoteTiming& last() const { return _last; }
  bool hasLast() const { return _hasLast; }

  // --- Diagnostic -----------------------------------------------------------
  uint16_t rejectedFrames() const { return _rejectedFrames; }
  uint16_t rejectedEvents() const { return _rejectedEvents; }

  // Interpolation du franchissement de seuil entre deux frames. Active par
  // defaut. La desactiver ramene les instants sur la grille des frames : c'est
  // la REFERENCE A/B qui permet de mesurer le gain au lieu de l'affirmer.
  void setEnvelopeInterpolation(bool on) { _interpolate = on; }
  bool envelopeInterpolation() const { return _interpolate; }

private:
  // --- Etat courant ---------------------------------------------------------
  TimingState _state;
  NoteTiming _note;
  NoteTiming _last;
  bool _hasLast;
  bool _interpolate;

  uint16_t _rejectedFrames;
  uint16_t _rejectedEvents;

  // Derniere frame vue : sert de point bas de l'interpolation et de garde
  // contre un temps qui recule.
  bool _havePrev;
  uint32_t _prevTs;
  float _prevDb;

  // Plancher d'avant-note.
  float _baseline[AcousticTimingCfg::kBaselineFrames];
  uint8_t _baselineCount;
  uint8_t _baselineIdx;

  // Confirmation d'attaque / de relachement.
  uint8_t _confirmCount;
  uint32_t _candidateTs;      // premier franchissement, deja interpole

  // Fenetre glissante du critere de plateau.
  float _plateauDb[AcousticTimingCfg::kAttackStableFrames];
  uint32_t _plateauTs[AcousticTimingCfg::kAttackStableFrames];
  uint8_t _plateauCount;
  uint8_t _plateauIdx;

  // Fenetre glissante du critere de stabilite de pitch.
  float _pitchHz[AcousticTimingCfg::kPitchStableFrames];
  uint32_t _pitchTs[AcousticTimingCfg::kPitchStableFrames];
  uint8_t _pitchCount;
  uint8_t _pitchIdx;

  // Suivi de niveau pendant la note.
  float _maxSinceOnset;
  float _sustainMeanDb;
  uint16_t _sustainFrames;
  bool _cutShort;            // l'arret est arrive avant le palier
  float _releaseThresholdDb; // seuil de chute, fige a l'ordre d'arret

  // --- Interne --------------------------------------------------------------
  void clearNoteTracking();
  void closeNote(TimingOutcome outcome);
  void pushBaseline(float db);
  float baselineMean() const;
  uint32_t crossingInstant(uint32_t curTs, float curDb, float thresholdDb) const;
  void trackPitch(const TimingFrame& f);
  bool plateauReached(uint32_t& atTs);
};

#endif  // ACOUSTIC_TIMING_H
