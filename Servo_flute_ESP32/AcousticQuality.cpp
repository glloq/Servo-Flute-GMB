#include "AcousticQuality.h"

#include <math.h>

#include "PitchMath.h"
#include "SpectralAnalyzer.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

// NaN et infinis ne sont pas des mesures. Ecrit a la main parce que isfinite()
// est une macro en C et une fonction en C++, et que ce fichier doit compiler
// tel quel des deux cotes.
inline bool aqFinite(float x) {
  return !(x != x) && x < 3.0e38f && x > -3.0e38f;
}

inline float clamp01(float x) {
  if (!aqFinite(x)) return 0.0f;
  return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

// Projette x sur 0..1, `atZero` valant 0 et `atOne` valant 1. Les deux bornes
// peuvent etre dans n'importe quel ordre : c'est ce qui permet d'ecrire
// "20 dB de HNR = pas de souffle, 0 dB = que du souffle" sans inverser le
// signe a la main a chaque usage.
inline float mapToUnit(float x, float atZero, float atOne) {
  const float span = atOne - atZero;
  if (!aqFinite(x) || !aqFinite(span) || span == 0.0f) return 0.0f;
  return clamp01((x - atZero) / span);
}

// Un poids negatif, nul ou non fini ne decrit rien : la composante est
// simplement ignoree, ce qui la fait sortir de la renormalisation au lieu de
// corrompre la somme.
inline bool weightUsable(float w) { return aqFinite(w) && w > 0.0f; }

inline void pushBaseline(SqueakDetector& d, float brightHz) {
  d.baseline[d.baselineHead] = brightHz;
  d.baselineHead = (uint8_t)((d.baselineHead + 1) % AQ_SQUEAK_BASELINE_FRAMES);
  if (d.baselineCount < AQ_SQUEAK_BASELINE_FRAMES) d.baselineCount++;
}

inline float baselineMean(const SqueakDetector& d) {
  if (d.baselineCount == 0) return 0.0f;
  float s = 0.0f;
  for (uint8_t i = 0; i < d.baselineCount; i++) s += d.baseline[i];
  return s / (float)d.baselineCount;
}

// Oublie le timbre tenu sans toucher au suivi de sequence : apres un silence,
// le son qui revient n'a aucune raison d'avoir la brillance de celui d'avant.
inline void forgetHeldNote(SqueakDetector& d) {
  d.baselineCount = 0;
  d.baselineHead = 0;
  d.eventFrames = 0;
  d.eventLength = 0;
  d.sinceEvent = 0;
  d.awaitingReturn = false;
  d.frozenBaseline = 0.0f;
  d.lastNoteMidi = 0;
}

}  // namespace

namespace AcousticQuality {

// ------------------------------------------------------------- utilitaires --

bool pitchIsUsable(const AcousticFeatures& f) {
  if (!aqFinite(f.pitchHz) || !aqFinite(f.pitchConfidence)) return false;
  if (f.pitchMidi <= 0) return false;
  // Hors de la plage que le detecteur sait couvrir, la frequence rendue est un
  // repliement ou un artefact, pas une note.
  if (f.pitchHz < MIC_PITCH_MIN_HZ || f.pitchHz > MIC_PITCH_MAX_HZ) return false;
  return f.pitchConfidence >= AQ_PITCH_MIN_CONFIDENCE;
}

float centsFromExpected(const AcousticFeatures& f, int expectedMidi) {
  if (expectedMidi <= 0 || !pitchIsUsable(f)) return 0.0f;
  const float c = PitchMath::hzToCents(f.pitchHz, expectedMidi);
  return aqFinite(c) ? c : 0.0f;
}

float frameBrightnessHz(const float* frame, size_t n, float sampleRate) {
  if (frame == nullptr || n < 2 || !(sampleRate > 0.0f) || !aqFinite(sampleRate)) return 0.0f;

  float mean = 0.0f;
  for (size_t i = 0; i < n; i++) mean += frame[i];
  mean /= (float)n;
  if (!aqFinite(mean)) return 0.0f;

  // Le continu est retire du denominateur seulement : la difference premiere
  // l'elimine deja d'elle-meme, mais l'energie du signal, elle, le compterait
  // et ferait passer un decalage de polarisation pour du grave.
  float den = 0.0f;
  for (size_t i = 0; i < n; i++) {
    const float c = frame[i] - mean;
    den += c * c;
  }
  float num = 0.0f;
  for (size_t i = 1; i < n; i++) {
    const float d = frame[i] - frame[i - 1];
    num += d * d;
  }
  if (!aqFinite(den) || !aqFinite(num) || !(den > 0.0f)) return 0.0f;

  const float r = num / den;
  if (!aqFinite(r) || !(r > 0.0f)) return 0.0f;
  float s = sqrtf(r) * 0.5f;
  if (s > 1.0f) s = 1.0f;   // r > 4 est hors d'atteinte d'un signal echantillonne
  return sampleRate * asinf(s) / kPi;
}

// ------------------------------------------------------------- respiration --

float breathinessAnchorHz(const AcousticFeatures& f, const AcousticContext& ctx) {
  if (pitchIsUsable(f)) return f.pitchHz;
  // Le pitch a lache : c'est justement le cas ou le son est le plus souffle, et
  // celui ou il faut continuer a mesurer. La note VISEE dit ou les partiels
  // devraient se trouver, ce qui suffit a juger s'ils y sont.
  if (ctx.expectedMidi > 0) {
    const float hz = PitchMath::midiToHz(ctx.expectedMidi);
    if (aqFinite(hz) && hz > 0.0f) return hz;
  }
  return 0.0f;
}

BreathinessResult computeBreathiness(const AcousticFeatures& f, const AcousticContext& ctx,
                                     float anchorHz) {
  BreathinessResult out;
  out.anchorHz = aqFinite(anchorHz) ? anchorHz : 0.0f;

  // LE SILENCE N'EST PAS DU SOUFFLE. La platitude spectrale d'une frame nulle
  // vaut exactement 1 (moyenne geometrique = moyenne arithmetique = 0) : sans
  // ce garde-fou, un microphone debranche serait classe "100 % souffle", ce qui
  // est la pire facon de se tromper - une valeur parfaitement plausible.
  if (!aqFinite(f.rms) || !(f.rms > AQ_SILENCE_RMS)) return out;

  float sum = 0.0f;
  float weight = 0.0f;

  // 1. Rapport harmonique / bruit, ancre sur la fondamentale reellement
  //    detectee : c'est la composante la plus directe, mais elle disparait des
  //    que le pitch lache.
  //    `hnrIsSpectral` est exige : AQ_BREATH_HNR_TONE_DB et _NOISE_DB sont
  //    calibres sur la mesure spectrale. L'approximation Goertzel, qui remplit
  //    le meme champ sur les frames sans FFT, lit environ 16 dB plus bas sur la
  //    MEME note propre - releve sur la chaine de production, fluteLike 440 Hz
  //    amp 0,40 : +40,00 dB en spectral contre +23,66 dB en Goertzel - la
  //    comparer a ces seuils ferait clignoter le verdict a la cadence de
  //    MIC_SPECTRAL_DECIMATION. Une composante absente est un manque visible ;
  //    une composante lue sur la mauvaise echelle est un mensonge.
  if (f.spectralValid && f.hnrIsSpectral && aqFinite(f.harmonicToNoiseRatio) &&
      weightUsable(AQ_BREATH_W_HNR)) {
    sum += AQ_BREATH_W_HNR *
           mapToUnit(f.harmonicToNoiseRatio, AQ_BREATH_HNR_TONE_DB, AQ_BREATH_HNR_NOISE_DB);
    weight += AQ_BREATH_W_HNR;
    out.usedHnr = true;
  }

  // 2. Energie ENTRE les partiels. Un son harmonique laisse les creux vides ;
  //    le souffle les remplit. Mesure directement sur le PCM, autour de l'ancre
  //    fournie, donc disponible meme sans pitch.
  if (ctx.frame != nullptr && ctx.frameSize >= 2 && aqFinite(ctx.sampleRate) &&
      ctx.sampleRate > 0.0f && out.anchorHz > 0.0f && weightUsable(AQ_BREATH_W_INTER)) {
    const float nyquist = ctx.sampleRate * 0.5f;
    float partials = 0.0f;
    float troughs = 0.0f;
    int troughCount = 0;
    for (int k = 1; k <= AQ_BREATH_PARTIALS; k++) {
      const float harmonicHz = out.anchorHz * (float)k;
      if (harmonicHz >= nyquist) break;
      partials += SpectralAnalyzer::goertzelPower(ctx.frame, ctx.frameSize, harmonicHz,
                                                  ctx.sampleRate);
      // Le creux est pris a mi-chemin du partiel suivant : c'est l'endroit le
      // plus eloigne de deux raies, donc celui ou seul du bruit peut se
      // trouver.
      const float troughHz = out.anchorHz * ((float)k + 0.5f);
      if (troughHz < nyquist) {
        troughs += SpectralAnalyzer::goertzelPower(ctx.frame, ctx.frameSize, troughHz,
                                                   ctx.sampleRate);
        troughCount++;
      }
    }
    if (troughCount > 0 && partials > 0.0f && aqFinite(partials) && aqFinite(troughs)) {
      // En dB parce que la grandeur couvre quatre decades entre une note propre
      // et du bruit blanc : une echelle lineaire y ecraserait tout le bas.
      float db = AQ_BREATH_INTER_FLOOR_DB;
      if (troughs > 0.0f) db = 10.0f * log10f(troughs / partials);
      if (!aqFinite(db) || db < AQ_BREATH_INTER_FLOOR_DB) db = AQ_BREATH_INTER_FLOOR_DB;
      sum += AQ_BREATH_W_INTER *
             mapToUnit(db, AQ_BREATH_INTER_TONE_DB, AQ_BREATH_INTER_NOISE_DB);
      weight += AQ_BREATH_W_INTER;
      out.usedInterHarmonic = true;
    }
  }

  // 3. Platitude spectrale. Rafraichie une frame sur MIC_SPECTRAL_DECIMATION
  //    seulement, et sensible au bruit de machinerie autant qu'au souffle :
  //    d'ou le poids le plus faible et l'exigence d'une FFT FRAICHE.
  if (ctx.fftFresh && aqFinite(f.spectralFlatness) && weightUsable(AQ_BREATH_W_FLATNESS)) {
    sum += AQ_BREATH_W_FLATNESS *
           mapToUnit(f.spectralFlatness, AQ_BREATH_FLATNESS_TONE, AQ_BREATH_FLATNESS_NOISE);
    weight += AQ_BREATH_W_FLATNESS;
    out.usedFlatness = true;
  }

  if (!(weight > 0.0f)) return out;   // aucune composante : `valid` reste faux
  out.value = clamp01(sum / weight);
  out.weightUsed = weight;
  out.valid = true;
  return out;
}

// ----------------------------------------------------------------- overblow --

/***********************************************************************************************
 * POURQUOI TROIS CRITERES, ET POURQUOI AUCUN NE SUFFIT SEUL
 *
 * AcousticFeatures::overblowDetected ne regarde que le pitch (PitchResult::
 * octaveAbove). C'est le critere necessaire, ce n'est pas un critere suffisant.
 *
 *   Le pitch seul ne suffit pas. Un estimateur de periode peut rendre la
 *   demi-periode d'un signal parfaitement correct : c'est l'erreur d'octave
 *   classique, celle contre laquelle la PHASE 2 a introduit le suivi de note
 *   attendue - preuve qu'elle existe. Sur cette seule base, une note juste mais
 *   mal estimee serait declaree overblow, et le regulateur couperait de l'air
 *   sur une note qui n'en demandait pas.
 *
 *   La dominance de l'octave seule ne suffit pas. N'importe quelle note
 *   brillante a un H2 fort ; un signal ecrete en fabrique ; un microphone qui
 *   roule dans le grave en fabrique en permanence. Sur ce seul critere, tout un
 *   registre de l'instrument serait declare en overblow en permanence.
 *
 *   La confiance seule ne dit rien de l'octave. Elle dit que la mesure de
 *   periode est nette, pas laquelle.
 *
 * Ensemble, chacun couvre l'angle mort des autres : si l'octave detectee venait
 * d'une erreur de periode, l'energie acoustique serait restee sur la
 * fondamentale visee et le critere spectral tomberait ; si la note etait
 * seulement brillante, le pitch resterait sur la fondamentale visee.
 *
 * LE CRITERE SPECTRAL SE MESURE SUR LA NOTE VISEE, PAS SUR LA NOTE DETECTEE.
 * C'est le point facile a rater : les harmoniques presentes dans
 * AcousticFeatures sont ancrees sur la frequence DETECTEE, laquelle vaut deja
 * 2*f_visee pendant un overblow. Leur "H2" designe donc 4*f_visee et ne repond
 * pas du tout a la question posee. Il faut deux Goertzel neufs, a f_visee et
 * 2*f_visee - donc le PCM de la frame.
 *
 * PORTEE ASSUMEE : seul le SECOND registre (une octave, +12 demi-tons) est
 * couvert. Le troisieme registre d'une flute sonne a la douzieme (+19), dont
 * l'energie se trouve a 3*f_visee et non a 2*f_visee : le critere spectral
 * ecrit ici y repondrait faux. Mieux vaut ne pas le detecter que le detecter
 * mal ; `valid` reste vrai et `detected` faux, ce qui est la reponse exacte a
 * la question "est-ce une octave au-dessus ?".
 ***********************************************************************************************/
OverblowResult evaluateOverblow(const AcousticFeatures& f, const AcousticContext& ctx) {
  OverblowResult out;

  // Sans note visee, "une octave au-dessus" n'a pas de reference : la question
  // elle-meme est vide.
  if (ctx.expectedMidi <= 0) return out;
  if (!pitchIsUsable(f)) return out;

  const float expectedHz = PitchMath::midiToHz(ctx.expectedMidi);
  if (!aqFinite(expectedHz) || !(expectedHz > 0.0f)) return out;

  // Critere 1 : le pitch est a une octave au-dessus de la note visee.
  const float cents = PitchMath::hzToCents(f.pitchHz, ctx.expectedMidi);
  out.pitchOctaveAbove = aqFinite(cents) &&
                         fabsf(cents - PitchMath::kCentsPerOctave) <= AQ_OVERBLOW_CENTS;

  // Critere 2 : l'energie acoustique est bien sur l'octave de la note VISEE.
  if (ctx.frame != nullptr && ctx.frameSize >= 2 && aqFinite(ctx.sampleRate) &&
      ctx.sampleRate > 0.0f) {
    const float nyquist = ctx.sampleRate * 0.5f;
    if (2.0f * expectedHz < nyquist) {
      const float atExpected =
          SpectralAnalyzer::goertzelPower(ctx.frame, ctx.frameSize, expectedHz, ctx.sampleRate);
      const float atOctave = SpectralAnalyzer::goertzelPower(ctx.frame, ctx.frameSize,
                                                             2.0f * expectedHz, ctx.sampleRate);
      if (aqFinite(atExpected) && aqFinite(atOctave)) {
        out.octaveRatioMeasured = true;
        if (atExpected > 0.0f) {
          out.octaveRatio = atOctave / atExpected;
          if (!aqFinite(out.octaveRatio) || out.octaveRatio > AQ_OVERBLOW_RATIO_MAX) {
            out.octaveRatio = AQ_OVERBLOW_RATIO_MAX;
          }
        } else {
          out.octaveRatio = (atOctave > 0.0f) ? AQ_OVERBLOW_RATIO_MAX : 0.0f;
        }
        out.octaveEnergyDominant = (out.octaveRatio >= AQ_OVERBLOW_OCTAVE_RATIO);
      }
    }
  }

  // Critere 3 : la mesure de periode est franche. Plus strict que le seuil
  // d'acceptation du detecteur, parce qu'un faux positif casse une note.
  out.confident = f.pitchConfidence >= AQ_OVERBLOW_MIN_CONFIDENCE;

  // `valid` ne dit pas "il y a overblow", il dit "les trois criteres ont pu
  // etre EVALUES". Sans le critere spectral, deux criteres sur trois ne font
  // pas un verdict degrade : ils ne font pas de verdict du tout.
  out.valid = out.octaveRatioMeasured;
  out.detected = out.valid && out.pitchOctaveAbove && out.octaveEnergyDominant && out.confident;
  return out;
}

// -------------------------------------------------------------------- couac --

SqueakResult updateSqueak(SqueakDetector& d, const AcousticFeatures& f,
                          const AcousticContext& ctx) {
  SqueakResult out;

  // CONTINUITE DES FRAMES. La duree d'un evenement se compte en frames ; or
  // l'anneau d'acquisition jette explicitement les plus anciennes quand il
  // deborde. Une duree comptee a travers un trou serait fausse, et le verdict
  // "bref" avec elle. On refuse la frame et on repart proprement.
  if (d.hasPrevious && f.frameSequence != d.lastFrameSequence + 1u) {
    d.reset();
    d.hasPrevious = true;
    d.lastFrameSequence = f.frameSequence;
    out.historyGap = true;
    return out;
  }
  d.hasPrevious = true;
  d.lastFrameSequence = f.frameSequence;

  // Un couac interrompt une note tenue. Sur du silence il n'y a rien a
  // interrompre, et le timbre d'avant n'a plus de valeur de reference.
  const bool sound = f.soundDetected && aqFinite(f.rms) && f.rms > AQ_SILENCE_RMS;
  if (!sound) {
    forgetHeldNote(d);
    return out;
  }

  // Brillance : mesuree sur le PCM quand il est la. A defaut, le centroide
  // spectral FRAIS fait un remplacant acceptable - il mesure la meme chose en
  // mieux, mais seulement une frame sur MIC_SPECTRAL_DECIMATION, ce qui suffit
  // rarement pour un evenement de deux frames.
  float bright = frameBrightnessHz(ctx.frame, ctx.frameSize, ctx.sampleRate);
  if (!(bright > 0.0f) && ctx.fftFresh && aqFinite(f.spectralCentroid) &&
      f.spectralCentroid > 0.0f) {
    bright = f.spectralCentroid;
  }
  out.brightnessHz = bright;
  if (!(bright > 0.0f)) return out;   // rien de mesurable : `valid` reste faux

  const bool pitchOk = pitchIsUsable(f);

  // Cible : la note visee si elle est declaree, sinon la derniere note
  // reellement tenue. Sans l'une ni l'autre, "revenir a la note" n'a pas de
  // sens et il n'y a pas de verdict a rendre.
  const int16_t target = (ctx.expectedMidi > 0) ? (int16_t)ctx.expectedMidi : d.lastNoteMidi;
  out.targetMidi = target;
  if (target <= 0) {
    if (pitchOk) d.lastNoteMidi = (int16_t)f.pitchMidi;
    pushBaseline(d, bright);
    return out;
  }

  // Une ligne de base d'une seule frame n'en est pas une : tout ecart lui
  // parait enorme, et le premier instant d'une note serait un couac.
  if (d.baselineCount < AQ_SQUEAK_MIN_BASELINE_FRAMES && d.eventFrames == 0) {
    if (pitchOk) d.lastNoteMidi = (int16_t)f.pitchMidi;
    pushBaseline(d, bright);
    return out;
  }

  out.valid = true;

  // La reference est FIGEE a l'entree de l'evenement : laisser la ligne de base
  // suivre les frames du couac reviendrait a s'habituer a lui, donc a
  // l'effacer.
  const float reference = (d.eventFrames > 0) ? d.frozenBaseline : baselineMean(d);
  out.baselineHz = reference;

  const bool brightBurst = (reference > 0.0f) && (bright >= reference * AQ_SQUEAK_BRIGHTNESS_RATIO);

  bool pitchWrong;
  if (pitchOk) {
    // Un couac est un partiel superieur : il est HAUT. Une note voisine, elle,
    // est une fausse note et doit rester classee comme telle.
    pitchWrong = ((int)f.pitchMidi - (int)target) >= AQ_SQUEAK_MIN_SEMITONES;
  } else {
    // Beaucoup de couacs sont inharmoniques et n'ont donc aucun pitch
    // mesurable. L'absence de periodicite sur un son FORT est un signe a part
    // entiere - et c'est precisement pour cela qu'elle ne compte qu'accompagnee
    // de la montee de brillance et de la breve duree.
    pitchWrong = true;
  }

  if (brightBurst && pitchWrong) {
    if (d.eventFrames == 0) d.frozenBaseline = baselineMean(d);
    if (d.eventFrames < 255) d.eventFrames++;
    // Un nouvel evenement annule l'attente de retour du precedent : deux
    // accidents separes par une frame ne font pas un couac confirme.
    d.awaitingReturn = false;
    out.eventFrames = d.eventFrames;
    if (d.eventFrames <= AQ_SQUEAK_MAX_FRAMES) {
      out.candidate = true;
    } else {
      // Trop long pour un accident : c'est une note, juste pas la bonne. Les
      // etats overblow / fausse note s'en chargent.
      out.rejectedTooLong = true;
    }
    // Les frames d'evenement n'entrent PAS dans la ligne de base, sans quoi le
    // couac remonterait lui-meme la reference a laquelle il est compare.
    return out;
  }

  if (d.eventFrames > 0) {
    if (d.eventFrames <= AQ_SQUEAK_MAX_FRAMES) {
      d.awaitingReturn = true;
      d.eventLength = d.eventFrames;
      d.sinceEvent = 0;
    }
    d.eventFrames = 0;
  }

  pushBaseline(d, bright);
  if (pitchOk) d.lastNoteMidi = (int16_t)f.pitchMidi;

  if (d.awaitingReturn) {
    if (d.sinceEvent < 255) d.sinceEvent++;
    if (pitchOk && (int16_t)f.pitchMidi == target) {
      out.confirmed = true;
      out.eventFrames = d.eventLength;
      d.awaitingReturn = false;
    } else if (d.sinceEvent >= AQ_SQUEAK_RETURN_FRAMES) {
      // Pas de retour sur la cible : l'instrument est parti ailleurs, ce
      // n'etait pas un accident passager.
      d.awaitingReturn = false;
    }
  }
  return out;
}

// ------------------------------------------------------------ classification --

/***********************************************************************************************
 * ORDRE DE PRIORITE DES ETATS - et pourquoi celui-la
 *
 * Un seul etat est rendu. L'ordre n'est donc pas cosmetique : il decide quelle
 * information survit quand plusieurs defauts coexistent. Regle generale : ce
 * qui invalide les autres mesures passe en premier, ce qui EXPLIQUE un defaut
 * passe avant le defaut explique, et ce qui est bref passe avant ce qui est
 * installe.
 *
 *  1. ECRETAGE. Un echantillon au rail fabrique des harmoniques qui n'existent
 *     pas : le spectre est pollue, la respiration surestimee, le pitch peut
 *     basculer d'octave. Toute autre mesure devient douteuse, donc aucun autre
 *     verdict ne doit etre rendu par-dessus. (Ecretage et silence ne peuvent
 *     pas coexister : l'ordre entre eux n'arbitre rien.)
 *  2. SILENCE. Sans son, tous les descripteurs sont des divisions par presque
 *     zero. La platitude spectrale d'une frame nulle vaut meme exactement 1,
 *     soit "bruit parfait" : classer avant de verifier la presence rendrait un
 *     microphone debranche "100 % souffle".
 *  3. COUAC. Il ne dure que quelques frames. S'il ne primait pas, ces frames
 *     seraient rangees en "instable" ou "fausse note" et l'accident
 *     disparaitrait dans une statistique. Or un couac est une faute nommee,
 *     ponctuelle et corrigible, ce qu'une instabilite n'est pas.
 *  4. OVERBLOW. C'est une CAUSE identifiee de fausse note : trop d'air. Classer
 *     d'abord "fausse note" garderait le symptome et jetterait le remede.
 *  5. FAUSSE NOTE. Meme symptome, cause inconnue.
 *  6. FAIBLE. Avant "souffle" parce qu'un son trop faible rend la mesure de
 *     souffle peu fiable : on mesurerait le rapport du bruit a lui-meme. Dire
 *     "trop faible" est alors le seul verdict honnete.
 *  7. SOUFFLE. Bonne note, niveau correct, mais le timbre est domine par le
 *     bruit large bande.
 *  8. INSTABLE. Bonne note, niveau correct, timbre correct, mais le pitch
 *     bouge. C'est le defaut le plus fin et il ne doit masquer aucun des
 *     precedents.
 *  9. BON. Aucun defaut retenu.
 *
 * Et un dixieme cas, qui n'est pas un etat : quand il y a du son mais qu'aucune
 * mesure n'a pu le decrire, `classified` reste faux. C'est deliberement le seul
 * moyen de ne pas dire "bon" par defaut.
 ***********************************************************************************************/
AcousticClassification classify(const AcousticFeatures& f, const AcousticContext& ctx,
                                SqueakDetector* squeak) {
  AcousticClassification out;

  out.missingExpectedNote = (ctx.expectedMidi <= 0);
  out.missingSnr = !f.snrValid;
  out.snrUsedFallback = f.snrValid && f.snrUsedFallback;
  out.missingSpectrum = !f.spectralValid;
  out.missingStability = !ctx.stabilityMeasured;
  out.missingPitch = !pitchIsUsable(f);
  out.missingSqueakHistory = (squeak == nullptr);

  // ENTREES CORROMPUES. Une comparaison avec NaN est fausse dans les deux sens :
  // une cascade de tests laisserait passer une frame invalide jusqu'a
  // ACOUSTIC_GOOD, c'est-a-dire jusqu'au verdict le plus flatteur. On refuse.
  if (!aqFinite(f.rms) || !aqFinite(f.rmsDbFS) || !aqFinite(f.clippingRatio)) return out;

  out.breathiness = computeBreathiness(f, ctx, breathinessAnchorHz(f, ctx));
  out.centsFromExpected = centsFromExpected(f, ctx.expectedMidi);

  // La machine a couac avance a CHAQUE frame, meme quand un autre etat
  // l'emporte : son historique est exactement ce qui distingue un accident bref
  // d'un defaut installe, et ne l'avancer que lorsqu'on la consulte creerait
  // dans sa sequence les trous qu'elle surveille.
  if (squeak != nullptr) out.squeak = updateSqueak(*squeak, f, ctx);
  out.overblow = evaluateOverblow(f, ctx);

  out.classified = true;

  if (f.clipping) { out.state = ACOUSTIC_CLIPPING; return out; }

  if (!f.soundDetected || !(f.rms > AQ_SILENCE_RMS)) {
    out.state = ACOUSTIC_SILENCE;
    return out;
  }

  if (out.squeak.valid && out.squeak.candidate) { out.state = ACOUSTIC_SQUEAK; return out; }

  if (out.overblow.valid && out.overblow.detected) { out.state = ACOUSTIC_OVERBLOW; return out; }

  if (!out.missingExpectedNote && !out.missingPitch &&
      fabsf(out.centsFromExpected) > AQ_WRONG_NOTE_CENTS) {
    out.state = ACOUSTIC_WRONG_NOTE;
    return out;
  }

  // Le SNR est le bon critere : il compare la note au bruit REEL de l'etat de
  // l'instrument. Sans profil capture, le niveau absolu reste une mesure, mais
  // une mesure strictement plus faible - elle ignore la piece et la machinerie.
  // `missingSnr` est deja positionne pour que l'appelant le sache.
  const bool weak = f.snrValid ? (aqFinite(f.snrDb) && f.snrDb < AQ_WEAK_SNR_DB)
                               : (f.rmsDbFS < AQ_WEAK_LEVEL_DBFS);
  if (weak) { out.state = ACOUSTIC_WEAK; return out; }

  if (out.breathiness.valid && out.breathiness.value > AQ_BREATHY_MAX) {
    out.state = ACOUSTIC_BREATHY;
    return out;
  }

  if (ctx.stabilityMeasured && aqFinite(f.pitchStability) &&
      f.pitchStability < AQ_UNSTABLE_STABILITY) {
    out.state = ACOUSTIC_UNSTABLE;
    return out;
  }

  // Il y a du son, aucun defaut ne s'est declenche - mais sans pitch
  // exploitable, "bon" serait une affirmation sur une note qu'on n'a pas
  // mesuree. On ne classe pas.
  if (out.missingPitch) {
    out.classified = false;
    out.state = ACOUSTIC_SILENCE;   // valeur par defaut, SANS signification ici
    return out;
  }

  // Sans note visee, BON veut dire "un son musical propre", pas "la bonne
  // note" : `missingExpectedNote` porte la nuance.
  out.state = ACOUSTIC_GOOD;
  return out;
}

const char* stateName(AcousticState s) {
  switch (s) {
    case ACOUSTIC_SILENCE:    return "silence";
    case ACOUSTIC_GOOD:       return "good";
    case ACOUSTIC_WEAK:       return "weak";
    case ACOUSTIC_BREATHY:    return "breathy";
    case ACOUSTIC_UNSTABLE:   return "unstable";
    case ACOUSTIC_WRONG_NOTE: return "wrong_note";
    case ACOUSTIC_OVERBLOW:   return "overblow";
    case ACOUSTIC_SQUEAK:     return "squeak";
    case ACOUSTIC_CLIPPING:   return "clipping";
    default:                  return "?";
  }
}

// ----------------------------------------------------------- note de qualite --

QualityScore computeAcousticQuality(const AcousticFeatures& f, const AcousticContext& ctx,
                                    const BreathinessResult& breath, float attackQuality,
                                    const QualityWeights& w) {
  QualityScore out;

  // Total des poids DEMANDES, donc denominateur de `weightUsed`. Un poids
  // absurde (negatif, NaN) est ignore ici comme il le sera plus bas : les deux
  // doivent compter la meme chose, sinon la fraction rendue serait fausse.
  float total = 0.0f;
  const float requested[7] = {w.intonation, w.stability, w.confidence, w.snr,
                              w.harmonic,   w.lowBreath, w.attack};
  for (int i = 0; i < 7; i++) {
    if (weightUsable(requested[i])) total += requested[i];
  }
  if (!(total > 0.0f)) return out;

  float sum = 0.0f;
  float used = 0.0f;
  const bool pitchOk = pitchIsUsable(f);

  // Justesse. Contre la note VISEE quand elle est declaree ; sinon contre le
  // temperament le plus proche, ce qui reste une mesure ("ce son est juste")
  // mais ne dit plus "c'est la bonne note". Confondre les deux donnerait 100 %
  // de justesse a une note parfaitement accordee... et fausse.
  if (pitchOk && weightUsable(w.intonation)) {
    float cents;
    if (ctx.expectedMidi > 0) {
      cents = centsFromExpected(f, ctx.expectedMidi);
      out.intonationVsExpected = true;
    } else {
      cents = f.cents;
    }
    if (aqFinite(cents)) {
      sum += w.intonation * clamp01(1.0f - fabsf(cents) / AQ_QUALITY_CENTS_REF);
      used += w.intonation;
      out.intonationMeasured = true;
    }
  }

  // Stabilite. Uniquement si l'appelant confirme que l'historique de pitch est
  // rempli : sinon 0 signifie "pas encore mesure" et vaudrait zero pointe a
  // chaque debut de note.
  if (ctx.stabilityMeasured && aqFinite(f.pitchStability) && weightUsable(w.stability)) {
    sum += w.stability * clamp01(f.pitchStability);
    used += w.stability;
    out.stabilityMeasured = true;
  }

  if (pitchOk && aqFinite(f.pitchConfidence) && weightUsable(w.confidence)) {
    sum += w.confidence * clamp01(f.pitchConfidence);
    used += w.confidence;
    out.confidenceMeasured = true;
  }

  if (f.snrValid && aqFinite(f.snrDb) && weightUsable(w.snr)) {
    sum += w.snr * clamp01(f.snrDb / AQ_QUALITY_SNR_REF_DB);
    used += w.snr;
    out.snrMeasured = true;
  }

  // Meme exigence que pour la respiration : AQ_QUALITY_HNR_GOOD_DB decrit la
  // mesure spectrale. Sur l'approximation Goertzel, ce seuil noterait une note
  // propre comme mediocre une frame sur MIC_SPECTRAL_DECIMATION.
  if (f.spectralValid && f.hnrIsSpectral && aqFinite(f.harmonicToNoiseRatio) &&
      weightUsable(w.harmonic)) {
    sum += w.harmonic *
           mapToUnit(f.harmonicToNoiseRatio, AQ_QUALITY_HNR_MIN_DB, AQ_QUALITY_HNR_GOOD_DB);
    used += w.harmonic;
    out.harmonicMeasured = true;
  }

  if (breath.valid && aqFinite(breath.value) && weightUsable(w.lowBreath)) {
    sum += w.lowBreath * clamp01(1.0f - breath.value);
    used += w.lowBreath;
    out.breathMeasured = true;
  }

  // Qualite d'attaque : mesuree par la PHASE 7, qui n'existe pas encore. Tant
  // que la sentinelle est passee, la composante est ABSENTE - pas nulle, pas
  // moyenne, pas devinee. Une valeur fournie mais hors de 0..1 est refusee et
  // signalee : elle vient d'un appelant casse, la traiter comme "non mesuree"
  // sans le dire masquerait le defaut.
  if (attackQuality != AQ_ATTACK_NOT_MEASURED) {
    if (aqFinite(attackQuality) && attackQuality >= 0.0f && attackQuality <= 1.0f) {
      if (weightUsable(w.attack)) {
        sum += w.attack * attackQuality;
        used += w.attack;
        out.attackMeasured = true;
      }
    } else {
      out.attackRejected = true;
    }
  }

  out.weightUsed = used / total;

  // Sous la moitie des criteres, le nombre decrirait surtout ce qui manque. Une
  // note de qualite qu'on ne peut pas comparer d'une frame a l'autre ne sert a
  // rien : on refuse plutot que de la rendre.
  if (!(out.weightUsed >= AQ_QUALITY_MIN_WEIGHT)) return out;

  out.score = clamp01(sum / used);
  out.valid = true;
  return out;
}

}  // namespace AcousticQuality
