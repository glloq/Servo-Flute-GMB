/***********************************************************************************************
 * AcousticQuality - Classification acoustique DETERMINISTE (PHASE 6)
 *
 * Fonctions PURES : aucun etat global, aucune allocation dynamique, aucune
 * dependance Arduino / I2S. Tout se compile et se teste sur hote.
 *
 * CE QUE FAIT CE FICHIER
 * ----------------------
 * Il transforme les MESURES d'une frame (AcousticFeatures : niveau, pitch,
 * spectre, rapport signal/bruit) en VERDICTS lisibles : respiration, overblow,
 * couac, etat acoustique, note de qualite. Rien n'est appris, rien n'est
 * entraine : chaque verdict est une combinaison explicite de seuils nommes,
 * qu'un humain peut relire, contester et regler.
 *
 * CE QUI N'EST PAS MESURE N'EST PAS INVENTE
 * -----------------------------------------
 * C'est la regle centrale du projet, et elle coute ici plus cher qu'ailleurs :
 * une classification a toujours envie de rendre un etat plausible. Donc chaque
 * resultat porte un champ de validite, et chaque verdict dit de QUOI il a ete
 * prive. Un score de qualite calcule sans le SNR n'est pas le meme nombre qu'un
 * score calcule avec, et il le dit (`weightUsed`).
 *
 * CE QUE L'APPELANT DOIT FOURNIR, ET POURQUOI (AcousticContext)
 * -------------------------------------------------------------
 * AcousticFeatures ne porte pas tout ce qu'il faut pour classer honnetement.
 * Trois manques sont contournes par un contexte explicite plutot que par une
 * supposition :
 *
 *   - `fftFresh` : AcousticFeatures::spectralValid signale que les mesures
 *     Goertzel de CETTE frame sont bonnes, mais les champs issus de la FFT
 *     (centroide, platitude) ne sont recalcules qu'une frame sur
 *     MIC_SPECTRAL_DECIMATION et ne sont PAS remis a zero entre-temps. Un
 *     consommateur qui lit `spectralFlatness` en se fiant a `spectralValid`
 *     lit donc une valeur vieille de 64 ms au maximum. L'appelant, lui, sait
 *     s'il a lance la FFT sur cette frame : il le dit.
 *
 *   - `stabilityMeasured` : PitchResult::stability vaut 0 tant que
 *     l'historique n'est pas rempli, et vaut aussi 0 quand la note est
 *     completement instable. Les deux cas sont indiscernables dans le champ
 *     lui-meme. Classer "instable" sur cette ambiguite etiquetterait chaque
 *     debut de note comme un defaut.
 *
 *   - `frame` / `frameSize` : deux mesures de cette phase (energie entre les
 *     partiels, brillance) demandent le PCM de la frame. Sans lui elles ne sont
 *     pas approximees : elles sont declarees absentes.
 *
 * COUT
 * ----
 * La mesure inter-partiels ajoute 8 Goertzel de n points, soit environ 8 % du
 * cout de YIN ; la brillance ajoute 2n operations, soit environ 2 %. Le reste
 * est de la comparaison de seuils. Comme partout dans cette chaine, ce sont des
 * comptes d'operations, PAS des mesures sur materiel.
 ***********************************************************************************************/
#ifndef ACOUSTIC_QUALITY_H
#define ACOUSTIC_QUALITY_H

#include <stddef.h>
#include <stdint.h>
#include "settings.h"
#include "AcousticFeatures.h"

/*==============================================================================
 *  SEUILS D'INSTRUMENT - bloc unique, volontairement HORS de settings.h
 *==============================================================================
 *
 * POURQUOI PAS settings.h : settings.h decrit le MATERIEL et le firmware -
 * broches, frequence d'echantillonnage, tailles de tampon, bornes de detection.
 * Ce qui suit decrit un INSTRUMENT : a partir de quel souffle une flute
 * particuliere sonne "aeree", a quel ecart on declare une fausse note, combien
 * de temps dure un couac. Deux flutes, ou la meme avec un microphone place
 * autrement, n'ont pas les memes valeurs. Les melanger reviendrait a faire
 * croire qu'un reglage de gout est une constante physique.
 *
 * Les valeurs de depart ci-dessous ont ete relevees sur les signaux
 * synthetiques de tests/test_native/audio_signals.h (une note "flute" a 440 Hz
 * d'amplitude 0,4 a laquelle on ajoute du bruit large bande). Ce sont des
 * points de depart MESURES sur ces signaux-la, pas des valeurs validees sur un
 * instrument reel - aucun microphone n'a jamais ete branche sur ce projet.
 *============================================================================*/

// --- Presence sonore --------------------------------------------------------

// Plancher de presence. On REUTILISE deliberement la constante du firmware :
// deux planchers de silence differents dans la meme chaine - un pour decider
// qu'il y a du son, un pour le classer - finiraient forcement par se
// contredire sur les niveaux intermediaires.
constexpr float AQ_SILENCE_RMS = MIC_RMS_ABSOLUTE_MIN;

// En dessous de ce rapport signal/bruit, la note est noyee dans le bruit de sa
// propre machinerie : les mesures spectrales portent alors autant sur la pompe
// que sur la note. 12 dB = un facteur 4 en amplitude ; AutoCalMath considere
// 24 dB comme un excellent SNR, la moitie marque donc le "tout juste utilisable".
constexpr float AQ_WEAK_SNR_DB = 12.0f;

// Repli quand AUCUN profil de bruit n'a ete capture : le niveau absolu est un
// critere STRICTEMENT plus faible (il ignore la piece et la machinerie), mais
// il reste une mesure. -40 dBFS = 1 % de la pleine echelle numerique.
constexpr float AQ_WEAK_LEVEL_DBFS = -40.0f;

// --- Pitch ------------------------------------------------------------------

// Confiance YIN minimale pour qu'un pitch serve de base a un verdict. On
// reprend le seuil d'acceptation du detecteur lui-meme : un classificateur qui
// accepterait un pitch que PitchDetector a rejete inventerait une note.
constexpr float AQ_PITCH_MIN_CONFIDENCE = MIC_YIN_CONFIDENCE_MIN;

// Ecart a la note visee au-dela duquel on parle de fausse note. Un demi-ton
// vaut 100 cents : a 50 cents la frequence est a egale distance de deux notes
// et l'arrondi MIDI bascule, donc au-dela on ne joue litteralement plus la
// meme note.
constexpr float AQ_WRONG_NOTE_CENTS = 50.0f;

// Stabilite en dessous de laquelle le pitch est juge instable. Avec
// MIC_PITCH_STABILITY_REF_CENTS = 50, stabilite = 1 - etendue/50 : 0,60
// correspond donc exactement a 20 cents d'etendue sur les MIC_PITCH_HISTORY
// dernieres frames (128 ms). Un cinquieme de demi-ton de flottement est
// audible ; moins ne l'est pas.
constexpr float AQ_UNSTABLE_STABILITY = 0.60f;

// --- Respiration (breathiness) ----------------------------------------------

// Bornes de la composante "rapport harmonique / bruit". 20 dB : mesure sur une
// note flute propre (HNR releve entre 22 et 25 dB sans souffle ajoute). 0 dB :
// l'energie harmonique et le residu sont a egalite, il n'y a plus de timbre.
constexpr float AQ_BREATH_HNR_TONE_DB  = 20.0f;
constexpr float AQ_BREATH_HNR_NOISE_DB = 0.0f;

// Bornes de la composante "platitude spectrale". Releve sur les memes signaux :
// une note sans souffle donne moins de 0,001 ; un souffle a peine audible
// (bruit 0,01 pour une note d'amplitude 0,4) donne deja 0,12, parce que la
// platitude est une moyenne geometrique sur TOUS les bins et qu'un plancher de
// bruit, meme faible, remplit les bins vides. Elle est donc tres sensible a la
// PRESENCE de bruit et peu discriminante sur son NIVEAU : 0,10 a 0,70.
constexpr float AQ_BREATH_FLATNESS_TONE  = 0.10f;
constexpr float AQ_BREATH_FLATNESS_NOISE = 0.70f;

// Bornes de la composante "energie entre les partiels", en dB (creux / raies).
// Relevee sur la meme serie : -50 dB sans souffle, -28 dB pour un souffle deja
// franc, +1 dB pour du bruit blanc pur. -40 dB = les creux portent un
// dix-milliemes de l'energie des raies, c'est un spectre de raies propre ;
// -10 dB = ils en portent un dixieme, le spectre s'est rempli.
constexpr float AQ_BREATH_INTER_TONE_DB  = -40.0f;
constexpr float AQ_BREATH_INTER_NOISE_DB = -10.0f;

// Plancher de la mesure inter-partiels : un creux mesure a exactement zero est
// un artefact de signal synthetique, pas une mesure. On borne plutot que de
// rendre -inf.
constexpr float AQ_BREATH_INTER_FLOOR_DB = -60.0f;

// Nombre de partiels sondes (f0..4f0) et de creux (1,5f0..4,5f0). Aligne sur
// SpectralAnalyzer::harmonics, qui mesure lui aussi quatre raies : au-dela, a
// 32 kHz, une note aigue n'a plus de partiel sous Nyquist.
constexpr int AQ_BREATH_PARTIALS = 4;

// Poids des trois composantes. Le HNR pese le plus : il est mesure a CHAQUE
// frame et directement sur la fondamentale detectee. L'inter-partiels vient
// ensuite : aussi direct, mais dependant de l'ancre fournie. La platitude pese
// le moins : elle n'est rafraichie qu'une frame sur MIC_SPECTRAL_DECIMATION et
// le bruit de machinerie de l'instrument la fait monter sans qu'aucun souffle
// ne soit en cause.
constexpr float AQ_BREATH_W_HNR      = 0.50f;
constexpr float AQ_BREATH_W_INTER    = 0.30f;
constexpr float AQ_BREATH_W_FLATNESS = 0.20f;

// Au-dela, l'etat acoustique est declare "souffle". 0,55 correspond, sur les
// signaux de reference, a un HNR d'environ 9 dB accompagne d'un spectre
// visiblement rempli : l'energie harmonique domine encore, mais de peu.
constexpr float AQ_BREATHY_MAX = 0.55f;

// --- Overblow ---------------------------------------------------------------

// Rapport de PUISSANCE entre la raie a 2*f_visee et la raie a f_visee au-dela
// duquel on considere que l'energie a bascule sur l'octave. Une note flute
// normale a un H2 d'amplitude 0,3 fois la fondamentale, soit 0,09 en puissance :
// le seuil 4,0 est deux ordres de grandeur au-dessus, et tres en dessous d'un
// vrai overblow ou la fondamentale visee s'efface presque completement.
constexpr float AQ_OVERBLOW_OCTAVE_RATIO = 4.0f;

// Confiance minimale, DELIBEREMENT plus stricte que AQ_PITCH_MIN_CONFIDENCE :
// annoncer un overblow a tort conduit le regulateur a couper de l'air sur une
// note correcte, ce qui casse la note. Le faux positif coute plus cher ici que
// le faux negatif.
constexpr float AQ_OVERBLOW_MIN_CONFIDENCE = 0.85f;

// Tolerance autour de l'octave exacte, en cents. Meme raison qu'au-dessus :
// au-dela de 50 cents on n'est plus sur la note d'octave.
constexpr float AQ_OVERBLOW_CENTS = 50.0f;

// Rapport de puissance au-dela duquel la dominance de l'octave n'est plus
// mesurable : 1000 valent 30 dB, et une fondamentale 30 dB sous son octave est
// deja sous le plancher de n'importe quel microphone reel. On borne plutot que
// de rendre l'infini quand la raie visee est mesuree a exactement zero, ce qui
// n'arrive que sur un signal synthetique.
constexpr float AQ_OVERBLOW_RATIO_MAX = 1000.0f;

// --- Couac (squeak) ---------------------------------------------------------

// Profondeur de la ligne de base de brillance, en frames. 8 frames = 128 ms a
// 62,5 frames/s : assez pour moyenner le timbre tenu, assez court pour suivre
// un changement de note.
constexpr int AQ_SQUEAK_BASELINE_FRAMES = 8;

// Frames de ligne de base exigees avant qu'une brillance puisse etre declaree
// anormale. Trois frames = 48 ms de timbre tenu ; en dessous la "ligne de base"
// n'est qu'un echantillon isole et le moindre ecart lui parait enorme - le
// premier instant de chaque note serait un couac.
constexpr uint8_t AQ_SQUEAK_MIN_BASELINE_FRAMES = 3;

// Duree maximale d'un couac, en frames. 6 frames = environ 96 ms. Au-dela ce
// n'est plus un accident : c'est une note, juste pas la bonne - et elle doit
// etre classee comme telle (overblow ou fausse note), pas comme un couac.
constexpr int AQ_SQUEAK_MAX_FRAMES = 6;

// Delai accorde au retour sur la note cible, en frames. 8 frames = 128 ms. Sans
// retour, l'evenement n'etait pas un accident passager.
constexpr int AQ_SQUEAK_RETURN_FRAMES = 8;

// Montee de brillance exigee par rapport a la ligne de base. 1,6 = +60 %. Sur
// les signaux de reference, un couac a 2637 Hz sur une note a 440 Hz multiplie
// la brillance par 5,9 ; un simple passage souffle la multiplie par 2,5, d'ou
// les autres criteres qui doivent l'accompagner.
constexpr float AQ_SQUEAK_BRIGHTNESS_RATIO = 1.6f;

// Ecart minimal, en demi-tons, entre le pitch du couac et la note cible. 7
// demi-tons (une quinte) : un couac de flute est un partiel superieur
// (+12, +19, +24...), jamais une note voisine. En dessous de cet ecart il
// s'agit d'une fausse note, pas d'un couac.
constexpr int AQ_SQUEAK_MIN_SEMITONES = 7;

// --- Note de qualite --------------------------------------------------------

// Ecart en cents qui annule la composante "justesse". Meme raisonnement que
// AQ_WRONG_NOTE_CENTS : a 50 cents on a change de note, la justesse ne vaut
// plus rien.
constexpr float AQ_QUALITY_CENTS_REF = 50.0f;

// SNR qui sature la composante correspondante. 24 dB, valeur deja retenue par
// AutoCalMath::positionQuality ("~24 dB counts as an excellent SNR") : garder
// deux references differentes ferait diverger la note de qualite live et le
// score de calibration.
constexpr float AQ_QUALITY_SNR_REF_DB = 24.0f;

// Bornes de la composante "qualite harmonique" (HNR). Memes reperes mesures que
// pour la respiration.
constexpr float AQ_QUALITY_HNR_MIN_DB  = 0.0f;
constexpr float AQ_QUALITY_HNR_GOOD_DB = 20.0f;

// Valeur SENTINELLE de la qualite d'attaque. La PHASE 7 (analyse temporelle) ne
// l'a pas encore mesuree et ce fichier ne l'invente pas : tant qu'elle vaut
// ceci, la composante est simplement absente du calcul et le resultat le dit.
constexpr float AQ_ATTACK_NOT_MEASURED = -1.0f;

// Fraction minimale des poids qui doit etre reellement mesuree pour qu'une note
// de qualite ait un sens. En dessous de la moitie des criteres, le nombre
// decrirait surtout ce qu'on n'a pas mesure : on refuse plutot que de le rendre.
constexpr float AQ_QUALITY_MIN_WEIGHT = 0.50f;

/*============================================================================*/
/*  FIN DU BLOC DE SEUILS                                                      */
/*============================================================================*/

// Etat acoustique d'une frame. UN seul etat est rendu : c'est un affichage et
// une decision, pas un inventaire. Les mesures sous-jacentes restent lisibles
// dans AcousticClassification pour qui veut les deux.
enum AcousticState {
  ACOUSTIC_SILENCE,
  ACOUSTIC_GOOD,
  ACOUSTIC_WEAK,
  ACOUSTIC_BREATHY,
  ACOUSTIC_UNSTABLE,
  ACOUSTIC_WRONG_NOTE,
  ACOUSTIC_OVERBLOW,
  ACOUSTIC_SQUEAK,
  ACOUSTIC_CLIPPING
};

// Tout ce que l'appelant SAIT et que AcousticFeatures ne porte pas. Voir
// l'en-tete de ce fichier pour le detail de chaque manque.
struct AcousticContext {
  int expectedMidi = 0;              // note visee, 0 = aucune declaree
  bool fftFresh = false;             // centroide/platitude mesures sur CETTE frame
  bool stabilityMeasured = false;    // l'historique de pitch est-il rempli ?
  const float* frame = nullptr;      // PCM de la frame, optionnel
  size_t frameSize = 0;
  float sampleRate = (float)MIC_SAMPLE_RATE;
};

// Resultat de la mesure de respiration. `valid` faux = aucune des trois
// composantes n'etait disponible ; `value` n'a alors aucun sens.
struct BreathinessResult {
  bool valid = false;
  float value = 0.0f;            // 0 = son pur et timbre, 1 = essentiellement du souffle
  float weightUsed = 0.0f;       // somme des poids des composantes reellement mesurees
  bool usedHnr = false;
  bool usedInterHarmonic = false;
  bool usedFlatness = false;
  float anchorHz = 0.0f;         // frequence sur laquelle la mesure a ete ancree
};

// Resultat de la detection d'overblow. Les trois criteres sont exposes
// separement : un verdict qu'on ne peut pas decomposer ne se debogue pas.
struct OverblowResult {
  bool valid = false;            // les trois criteres ont pu etre EVALUES
  bool detected = false;         // ...et les trois sont remplis
  bool pitchOctaveAbove = false;
  bool octaveEnergyDominant = false;
  bool confident = false;
  float octaveRatio = 0.0f;      // P(2*f_visee) / P(f_visee), 0 si non mesure
  bool octaveRatioMeasured = false;
};

/***********************************************************************************************
 * Detection de couac - etat EXPLICITE
 *
 * Un couac se definit par sa breve duree et par le retour a la note : ni l'un
 * ni l'autre n'est connaissable a l'instant ou il commence. Il faut donc un
 * historique, et cet historique est ICI, dans une structure que l'appelant
 * possede, remet a zero quand il veut (changement de note, silence) et peut
 * inspecter. Aucune variable statique cachee : deux flutes, ou deux tests,
 * n'interferent pas.
 *
 * DEUX VERDICTS, ET C'EST VOULU
 *   `candidate` : les criteres instantanes sont remplis MAINTENANT. C'est ce
 *                 qu'affiche un moniteur temps reel, au prix d'un possible
 *                 dementi quelques frames plus tard.
 *   `confirmed` : l'evenement s'est termine dans le temps imparti ET la note
 *                 cible est revenue. C'est le seul verdict complet, et il
 *                 arrive forcement EN RETARD (jusqu'a AQ_SQUEAK_RETURN_FRAMES
 *                 frames). C'est celui qu'il faut compter, pas afficher.
 ***********************************************************************************************/
struct SqueakDetector {
  float baseline[AQ_SQUEAK_BASELINE_FRAMES] = {};  // brillance des frames NORMALES
  uint8_t baselineCount = 0;
  uint8_t baselineHead = 0;

  int16_t lastNoteMidi = 0;      // derniere note tenue, sert de cible faute de note visee
  uint8_t eventFrames = 0;       // duree de l'evenement en cours, 0 = aucun
  uint8_t eventLength = 0;       // duree de l'evenement qui vient de se terminer
  uint8_t sinceEvent = 0;        // frames ecoulees depuis cette fin
  bool awaitingReturn = false;   // on attend le retour sur la cible
  float frozenBaseline = 0.0f;   // ligne de base figee a l'entree de l'evenement

  bool hasPrevious = false;      // au moins une frame vue
  uint32_t lastFrameSequence = 0;

  void reset() { *this = SqueakDetector(); }
};

struct SqueakResult {
  bool valid = false;            // la detection a pu etre EVALUEE sur cette frame
  bool candidate = false;        // criteres instantanes remplis maintenant
  bool confirmed = false;        // couac complet, confirme retroactivement
  bool rejectedTooLong = false;  // l'evenement a depasse AQ_SQUEAK_MAX_FRAMES
  uint8_t eventFrames = 0;       // duree courante (ou finale) de l'evenement
  float brightnessHz = 0.0f;     // brillance mesuree sur cette frame
  float baselineHz = 0.0f;       // ligne de base a laquelle elle a ete comparee
  bool historyGap = false;       // trou dans la sequence de frames : duree non fiable
  int16_t targetMidi = 0;        // cible retenue (note visee ou note tenue)
};

// Verdict complet d'une frame. `classified` faux signifie que les entrees ne
// permettaient AUCUN verdict : `state` garde alors sa valeur par defaut et ne
// doit pas etre affiche. Aucune valeur par defaut ne peut etre juste dans ce
// cas ; on choisit celle qui ne peut pas passer pour un compliment.
struct AcousticClassification {
  AcousticState state = ACOUSTIC_SILENCE;
  bool classified = false;

  // Ce dont le verdict a ete prive. Un consommateur honnete les affiche.
  bool missingPitch = false;
  bool missingSnr = false;
  bool missingSpectrum = false;
  bool missingExpectedNote = false;
  bool missingStability = false;
  bool missingSqueakHistory = false;
  bool snrUsedFallback = false;      // le SNR compare a un autre etat que le reel

  BreathinessResult breathiness;
  OverblowResult overblow;
  SqueakResult squeak;
  float centsFromExpected = 0.0f;    // 0 si aucune note visee
};

/***********************************************************************************************
 * Ponderation de la note de qualite
 *
 * Nommee et groupee plutot qu'eparpillee en litteraux : ce sont les criteres du
 * cahier des charges, ils se relisent et se reglent ensemble.
 ***********************************************************************************************/
struct QualityWeights {
  float intonation = 0.25f;   // justesse par rapport a la note visee
  float stability  = 0.15f;   // tenue du pitch
  float confidence = 0.10f;   // confiance YIN
  float snr        = 0.15f;   // niveau au-dessus du bruit REEL de l'instrument
  float harmonic   = 0.15f;   // richesse harmonique (HNR)
  float lowBreath  = 0.10f;   // 1 - respiration
  float attack     = 0.10f;   // qualite d'attaque (PHASE 7, pas encore mesuree)
};

// Note de qualite d'une frame. `weightUsed` est la fraction des poids
// reellement mesuree : deux scores de `weightUsed` differents ne sont PAS
// comparables entre eux, et c'est la raison d'etre de ce champ.
struct QualityScore {
  bool valid = false;
  float score = 0.0f;            // 0..1
  float weightUsed = 0.0f;       // 0..1, somme des poids des composantes mesurees
  bool intonationMeasured = false;
  bool intonationVsExpected = false;  // sinon : ecart au temperament le plus proche
  bool stabilityMeasured = false;
  bool confidenceMeasured = false;
  bool snrMeasured = false;
  bool harmonicMeasured = false;
  bool breathMeasured = false;
  bool attackMeasured = false;
  bool attackRejected = false;   // valeur d'attaque fournie mais absurde
};

namespace AcousticQuality {

// --- Utilitaires purs -------------------------------------------------------

// Un pitch est exploitable quand il est dans la plage de detection ET que la
// confiance atteint le seuil du detecteur. AcousticFeatures ne porte pas de
// drapeau `pitchValid` : il faut donc le reconstituer, et le faire au meme
// endroit pour tout le monde.
bool pitchIsUsable(const AcousticFeatures& f);

// Ecart signe, en cents, par rapport a la note VISEE (et non a la plus proche).
// Rend 0 si aucune note n'est visee ou si le pitch n'est pas exploitable :
// l'appelant doit avoir verifie avant.
float centsFromExpected(const AcousticFeatures& f, int expectedMidi);

// Brillance de la frame, exprimee en Hz equivalents.
//
// POURQUOI PAS LE CENTROIDE SPECTRAL : il ne sort de la FFT qu'une frame sur
// MIC_SPECTRAL_DECIMATION, soit toutes les 64 ms, alors qu'un couac peut ne
// durer que deux frames. Il faut une mesure de contenu haute frequence
// disponible a CHAQUE frame.
//
// Estimateur : energie de la difference premiere rapportee a l'energie du
// signal centre. Pour un sinus pur de frequence f, ce rapport vaut exactement
// 4.sin^2(pi.f/Fs), qu'on inverse pour retomber sur f - la mesure est donc
// EXACTE sur un sinus et se lit en Hz. Sur un melange elle rend une frequence
// equivalente ponderee par l'energie : c'est un indicateur monotone de
// brillance, PAS un centroide spectral, et il ne doit pas etre presente comme
// tel. Cout : 2n operations, environ 2 % de YIN.
//
// Rend 0 quand la mesure est impossible (tampon nul, moins de 2 echantillons,
// energie nulle) : 0 Hz n'est pas une brillance plausible, c'est un refus.
float frameBrightnessHz(const float* frame, size_t n,
                        float sampleRate = (float)MIC_SAMPLE_RATE);

// --- Respiration ------------------------------------------------------------

// Frequence sur laquelle ancrer les mesures harmoniques : le pitch detecte
// s'il est exploitable, sinon la note visee. Rend 0 si ni l'un ni l'autre.
float breathinessAnchorHz(const AcousticFeatures& f, const AcousticContext& ctx);

// Indicateur de respiration 0..1, moyenne ponderee des composantes REELLEMENT
// disponibles (rapport harmonique/bruit, energie entre les partiels, platitude
// spectrale), renormalisee par les poids utilises.
//
// MONOTONIE : les trois composantes croissent quand on ajoute du bruit large
// bande a une note harmonique, donc l'indicateur croit aussi. C'est la
// propriete que le test verifie, et elle est plus importante que la valeur
// absolue, qui depend du microphone.
//
// `anchorHz` est la frequence autour de laquelle chercher la structure
// harmonique. La passer explicitement permet de continuer a mesurer quand le
// pitch a lache - c'est-a-dire exactement quand le son est le plus souffle -
// en s'ancrant sur la note visee.
BreathinessResult computeBreathiness(const AcousticFeatures& f, const AcousticContext& ctx,
                                     float anchorHz);

// --- Overblow ---------------------------------------------------------------

// Detection d'overblow a TROIS criteres conjoints. Le champ
// AcousticFeatures::overblowDetected, lui, ne repose que sur le pitch.
// Pourquoi aucun critere ne suffit seul, c'est dans AcousticQuality.cpp, au
// dessus de la fonction.
//
// Le PCM de la frame (ctx.frame) est INDISPENSABLE : le critere spectral se
// mesure a f_visee et 2*f_visee, alors que les harmoniques deja presentes dans
// AcousticFeatures sont ancrees sur la frequence DETECTEE, c'est-a-dire sur
// l'octave pendant un overblow. Sans PCM, `valid` reste faux.
OverblowResult evaluateOverblow(const AcousticFeatures& f, const AcousticContext& ctx);

// --- Couac ------------------------------------------------------------------

// Fait avancer la machine a etats d'UNE frame. Voir SqueakDetector pour la
// difference entre `candidate` et `confirmed`.
SqueakResult updateSqueak(SqueakDetector& d, const AcousticFeatures& f,
                          const AcousticContext& ctx);

// --- Classification ---------------------------------------------------------

// `squeak` peut etre nul : la detection de couac est alors declaree absente
// (`missingSqueakHistory`) au lieu d'etre approximee sans historique.
AcousticClassification classify(const AcousticFeatures& f, const AcousticContext& ctx,
                                SqueakDetector* squeak);

const char* stateName(AcousticState s);

// --- Note de qualite --------------------------------------------------------

// `attackQuality` vaut AQ_ATTACK_NOT_MEASURED tant que la PHASE 7 n'existe pas.
// Le score est alors la moyenne ponderee des SIX autres criteres, renormalisee :
// il vaut pour 90 % du cahier des charges et n'est pas comparable a un score
// calcule avec l'attaque. `QualityScore::weightUsed` porte cette information et
// doit etre affiche avec le score.
QualityScore computeAcousticQuality(const AcousticFeatures& f, const AcousticContext& ctx,
                                    const BreathinessResult& breath, float attackQuality,
                                    const QualityWeights& weights = QualityWeights());

}  // namespace AcousticQuality

#endif  // ACOUSTIC_QUALITY_H
