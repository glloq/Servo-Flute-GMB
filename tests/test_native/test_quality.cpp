/***********************************************************************************************
 * test_quality.cpp - Tests natifs de la classification acoustique (PHASE 6)
 *
 * Tous les signaux viennent des generateurs reproductibles d'audio_signals.h
 * (LCG a graine explicite, jamais rand()). Cela valide le TRAITEMENT DU SIGNAL,
 * jamais le comportement acoustique reel d'une flute : aucun microphone n'a
 * jamais ete branche sur ce projet.
 *
 * CE QUE CES TESTS CHERCHENT A PIEGER
 * -----------------------------------
 * Une classification a toujours envie de rendre un etat plausible. Les tests
 * les plus importants de ce fichier ne verifient donc pas qu'un bon signal est
 * classe "bon" - c'est le cas facile - mais qu'une entree INSUFFISANTE produit
 * un refus, et jamais un verdict flatteur : silence classe "100 % souffle",
 * NaN qui traverse une cascade de comparaisons jusqu'a ACOUSTIC_GOOD, overblow
 * annonce sur deux criteres au lieu de trois, note de qualite calculee sans
 * l'attaque et presentee comme si elle l'incluait.
 ***********************************************************************************************/
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "settings.h"
#include "AudioLevel.h"
#include "PitchDetector.h"
#include "PitchMath.h"
#include "SpectralAnalyzer.h"
#include "AcousticFeatures.h"
#include "AcousticQuality.h"
#include "NoiseModel.h"
#include "audio_signals.h"

namespace {

using audiosig::ToneSpec;

constexpr size_t kFrame = MIC_ANALYSIS_FRAME_SIZE;
constexpr float kFs = (float)MIC_SAMPLE_RATE;
constexpr int kMidiA4 = 69;    // 440 Hz
constexpr int kMidiA5 = 81;    // 880 Hz, l'octave au-dessus

// ---------------------------------------------------------------------------
// Banc d'essai : la chaine reelle, assemblee comme AudioAnalyzer l'assemble.
// Les frames sont numerotees consecutivement parce que la detection de couac
// compte des durees en frames et refuse une sequence trouee.
// ---------------------------------------------------------------------------
struct Rig {
  PitchDetector det;
  SpectralAnalyzer spec;
  uint32_t seq = 0;
  int decimation = 0;
  int validPitches = 0;
  bool fftFresh = false;

  AcousticFeatures analyse(const float* buf, size_t n, int expectedMidi) {
    AcousticFeatures f;
    f.frameSequence = ++seq;

    const FrameLevel level = AudioLevel::compute(buf, n);
    AcousticFeatureBuilder::fillLevel(f, level);

    if (expectedMidi > 0) det.setExpectedMidiNote(expectedMidi);
    else det.clearExpectedMidiNote();
    const PitchResult p = det.detect(buf, n);
    AcousticFeatureBuilder::fillPitch(f, p, level.rms > MIC_RMS_THRESHOLD);
    if (p.valid) validPitches++;

    const bool runFft = (decimation++ % MIC_SPECTRAL_DECIMATION) == 0;
    AcousticFeatureBuilder::fillSpectral(f, buf, n, p.valid ? p.hz : 0.0f, &spec, runFft);
    fftFresh = runFft && f.spectralValid;
    return f;
  }

  AcousticContext context(const float* buf, size_t n, int expectedMidi) const {
    AcousticContext ctx;
    ctx.expectedMidi = expectedMidi;
    ctx.fftFresh = fftFresh;
    ctx.stabilityMeasured = (validPitches >= MIC_PITCH_HISTORY);
    ctx.frame = buf;
    ctx.frameSize = n;
    ctx.sampleRate = kFs;
    return ctx;
  }
};

// Une note tenue plausible, utilisee comme reference "bonne note" partout.
void goodNote(std::vector<float>& buf, float f0, float breath, size_t startSample) {
  audiosig::fluteLike(buf.data(), buf.size(), f0, 0.4f, breath, kFs, startSample);
}

// Features construites a la main : indispensables pour tester l'ORDRE DE
// PRIORITE, ou il faut faire coexister des defauts qu'aucun signal reel ne
// presente simultanement.
AcousticFeatures healthyFeatures() {
  AcousticFeatures f;
  f.frameSequence = 1;
  f.rms = 0.30f;
  f.rmsDbFS = -10.5f;
  f.peakDbFS = -6.0f;
  f.clippingRatio = 0.0f;
  f.clipping = false;
  f.pitchHz = 440.0f;
  f.pitchMidi = kMidiA4;
  f.cents = 0.0f;
  f.pitchConfidence = 0.99f;
  f.pitchStability = 0.95f;
  f.spectralValid = true;
  f.fundamentalEnergy = 0.02f;
  f.h2Ratio = 0.09f;
  f.h3Ratio = 0.01f;
  f.harmonicToNoiseRatio = 24.0f;
  f.spectralCentroid = 650.0f;
  f.spectralFlatness = 0.02f;
  f.snrValid = true;
  f.snrDb = 30.0f;
  f.soundDetected = true;
  return f;
}

AcousticContext plainContext(int expectedMidi = kMidiA4) {
  AcousticContext ctx;
  ctx.expectedMidi = expectedMidi;
  ctx.fftFresh = true;
  ctx.stabilityMeasured = true;
  ctx.sampleRate = kFs;
  return ctx;
}

// Amorce l'historique de couac sans avoir a jouer huit frames. C'est
// precisement ce que permet un etat EXPLICITE : un test peut le construire.
void primeSqueak(SqueakDetector& d, float baselineHz, int16_t noteMidi, uint32_t lastSeq) {
  d.reset();
  for (int i = 0; i < AQ_SQUEAK_BASELINE_FRAMES; i++) d.baseline[i] = baselineHz;
  d.baselineCount = AQ_SQUEAK_BASELINE_FRAMES;
  d.lastNoteMidi = noteMidi;
  d.hasPrevious = true;
  d.lastFrameSequence = lastSeq;
}

bool nearly(float a, float b, float tol) { return fabsf(a - b) <= tol; }

// Le test refait a la main la borne 0..1 que la ponderation applique a chaque
// composante : l'arithmetique du score est ainsi verrouillee, et tout
// changement de poids se voit ici avant de se voir sur un instrument.
float clampRef(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// ===========================================================================
// 6.1 - Brillance : la mesure haute frequence disponible a CHAQUE frame
// ===========================================================================
void quality_brightness_is_exact_on_a_sine() {
  std::vector<float> buf(kFrame);

  // Sur un sinus pur l'estimateur est exact par construction : il inverse
  // 4.sin^2(pi.f/Fs). Si ce test casse, ce n'est plus une mesure en Hz.
  for (float f0 : {220.0f, 440.0f, 880.0f, 1760.0f, 3520.0f}) {
    audiosig::pureTone(buf.data(), kFrame, f0, 0.4f, kFs);
    const float b = AcousticQuality::frameBrightnessHz(buf.data(), kFrame, kFs);
    assert(nearly(b, f0, f0 * 0.01f));
  }

  // Le continu ne doit pas passer pour du grave : la meme note avec un fort
  // decalage donne la meme brillance.
  ToneSpec s; s.sampleRate = kFs; s.f0 = 440.0f; s.amp = 0.4f; s.dc = 0.5f;
  audiosig::fill(buf.data(), kFrame, s);
  assert(nearly(AcousticQuality::frameBrightnessHz(buf.data(), kFrame, kFs), 440.0f, 10.0f));

  // Monotone quand on souffle sur la note : c'est ce qui rend la mesure
  // utilisable pour reperer une bouffee haute frequence.
  float previous = 0.0f;
  for (float breath : {0.0f, 0.05f, 0.10f, 0.20f, 0.40f}) {
    audiosig::fluteLike(buf.data(), kFrame, 440.0f, 0.4f, breath, kFs);
    const float b = AcousticQuality::frameBrightnessHz(buf.data(), kFrame, kFs);
    assert(b > previous);
    previous = b;
  }

  // Entrees degenerees : un refus (0) et jamais une valeur plausible.
  assert(AcousticQuality::frameBrightnessHz(nullptr, kFrame, kFs) == 0.0f);
  assert(AcousticQuality::frameBrightnessHz(buf.data(), 0, kFs) == 0.0f);
  assert(AcousticQuality::frameBrightnessHz(buf.data(), 1, kFs) == 0.0f);
  assert(AcousticQuality::frameBrightnessHz(buf.data(), kFrame, 0.0f) == 0.0f);
  assert(AcousticQuality::frameBrightnessHz(buf.data(), kFrame, -1.0f) == 0.0f);
  audiosig::silence(buf.data(), kFrame);
  assert(AcousticQuality::frameBrightnessHz(buf.data(), kFrame, kFs) == 0.0f);
  audiosig::dcOnly(buf.data(), kFrame, 0.5f);
  assert(AcousticQuality::frameBrightnessHz(buf.data(), kFrame, kFs) == 0.0f);
  std::vector<float> nan(kFrame, NAN);
  assert(AcousticQuality::frameBrightnessHz(nan.data(), kFrame, kFs) == 0.0f);
}

// ===========================================================================
// 6.2 - Respiration : monotone, et muette quand rien n'est mesurable
// ===========================================================================
void quality_breathiness_is_monotone() {
  std::vector<float> buf(kFrame);

  // PROPRIETE CENTRALE : ajouter du bruit large bande a une note harmonique
  // fait MONTER l'indicateur, sans exception.
  const float kBreath[] = {0.0f, 0.01f, 0.02f, 0.05f, 0.10f, 0.20f, 0.40f, 0.80f};
  float previous = -1.0f;
  for (float breath : kBreath) {
    Rig rig;
    AcousticFeatures f;
    AcousticContext ctx;
    // Quatre frames pour que la FFT tombe au moins une fois sur la derniere.
    for (int i = 0; i < 4; i++) {
      goodNote(buf, 440.0f, breath, (size_t)i * kFrame);
      f = rig.analyse(buf.data(), kFrame, kMidiA4);
      ctx = rig.context(buf.data(), kFrame, kMidiA4);
    }
    const BreathinessResult b =
        AcousticQuality::computeBreathiness(f, ctx, AcousticQuality::breathinessAnchorHz(f, ctx));
    assert(b.valid);
    assert(b.value > previous);
    previous = b.value;
  }

  // Bornes : une note propre est proche de 0, du bruit blanc proche de 1.
  {
    Rig rig;
    goodNote(buf, 440.0f, 0.0f, 0);
    AcousticFeatures f = rig.analyse(buf.data(), kFrame, kMidiA4);
    AcousticContext ctx = rig.context(buf.data(), kFrame, kMidiA4);
    const BreathinessResult b =
        AcousticQuality::computeBreathiness(f, ctx, AcousticQuality::breathinessAnchorHz(f, ctx));
    assert(b.valid && b.value < 0.05f);
    assert(b.usedHnr && b.usedInterHarmonic && b.usedFlatness);
    assert(nearly(b.weightUsed, AQ_BREATH_W_HNR + AQ_BREATH_W_INTER + AQ_BREATH_W_FLATNESS, 1e-5f));
  }
  {
    Rig rig;
    audiosig::whiteNoise(buf.data(), kFrame, 0.4f, 99u);
    AcousticFeatures f = rig.analyse(buf.data(), kFrame, kMidiA4);
    AcousticContext ctx = rig.context(buf.data(), kFrame, kMidiA4);
    const BreathinessResult b =
        AcousticQuality::computeBreathiness(f, ctx, AcousticQuality::breathinessAnchorHz(f, ctx));
    // Pas de pitch sur du bruit : l'ancre retombe sur la note VISEE, et la
    // mesure continue au lieu de disparaitre exactement quand elle compte.
    assert(b.valid && !b.usedHnr && b.usedInterHarmonic);
    assert(nearly(b.anchorHz, PitchMath::midiToHz(kMidiA4), 0.1f));
    assert(b.value > 0.90f);
  }
}

void quality_breathiness_refuses_what_it_cannot_measure() {
  std::vector<float> buf(kFrame);
  Rig rig;

  // LE SILENCE N'EST PAS DU SOUFFLE. La platitude spectrale d'une frame nulle
  // vaut exactement 1 : sans garde-fou, un micro debranche serait "100 % souffle".
  audiosig::silence(buf.data(), kFrame);
  {
    AcousticFeatures f = rig.analyse(buf.data(), kFrame, kMidiA4);
    AcousticContext ctx = rig.context(buf.data(), kFrame, kMidiA4);
    ctx.fftFresh = true;
    f.spectralFlatness = 1.0f;          // ce que la FFT rend vraiment sur du silence
    const BreathinessResult b =
        AcousticQuality::computeBreathiness(f, ctx, 440.0f);
    assert(!b.valid);
    assert(b.value == 0.0f);
  }

  // Aucune composante disponible : pas de spectre Goertzel, pas de FFT
  // fraiche, pas de PCM. Refus, pas une valeur moyenne.
  {
    AcousticFeatures f = healthyFeatures();
    f.spectralValid = false;
    AcousticContext ctx = plainContext();
    ctx.fftFresh = false;
    const BreathinessResult b = AcousticQuality::computeBreathiness(f, ctx, 440.0f);
    assert(!b.valid && b.weightUsed == 0.0f);
  }

  // Entrees degenerees : chacune retire SA composante, sans invalider les autres.
  {
    AcousticFeatures f = healthyFeatures();
    AcousticContext ctx = plainContext();
    ctx.frame = nullptr; ctx.frameSize = kFrame;
    BreathinessResult b = AcousticQuality::computeBreathiness(f, ctx, 440.0f);
    assert(b.valid && !b.usedInterHarmonic && b.usedHnr && b.usedFlatness);

    goodNote(buf, 440.0f, 0.02f, 0);
    ctx.frame = buf.data(); ctx.frameSize = 0;
    b = AcousticQuality::computeBreathiness(f, ctx, 440.0f);
    assert(b.valid && !b.usedInterHarmonic);

    // Ancre absurde : negative, nulle, ou au-dessus de Nyquist.
    ctx.frameSize = kFrame;
    for (float anchor : {0.0f, -440.0f, kFs, 1e9f}) {
      b = AcousticQuality::computeBreathiness(f, ctx, anchor);
      assert(!b.usedInterHarmonic);
    }
    // NaN dans les descripteurs : la composante disparait, rien ne se propage.
    f.harmonicToNoiseRatio = NAN;
    f.spectralFlatness = NAN;
    b = AcousticQuality::computeBreathiness(f, ctx, 440.0f);
    assert(!b.usedHnr && !b.usedFlatness);
    f.rms = NAN;
    b = AcousticQuality::computeBreathiness(f, ctx, 440.0f);
    assert(!b.valid);
  }
}

// ===========================================================================
// 6.3 - Overblow : trois criteres, et aucun ne suffit seul
// ===========================================================================
void quality_overblow_requires_three_criteria() {
  std::vector<float> octave(kFrame);
  std::vector<float> fundamental(kFrame);
  audiosig::fluteLike(octave.data(), kFrame, 880.0f, 0.4f, 0.02f, kFs);
  audiosig::fluteLike(fundamental.data(), kFrame, 440.0f, 0.4f, 0.02f, kFs);

  // Cas nominal : l'instrument vise un La3 et sonne un La4.
  {
    Rig rig;
    AcousticFeatures f = rig.analyse(octave.data(), kFrame, kMidiA4);
    AcousticContext ctx = rig.context(octave.data(), kFrame, kMidiA4);
    const OverblowResult o = AcousticQuality::evaluateOverblow(f, ctx);
    assert(o.valid && o.detected);
    assert(o.pitchOctaveAbove && o.octaveEnergyDominant && o.confident);
    assert(o.octaveRatio >= AQ_OVERBLOW_OCTAVE_RATIO);
  }

  // La bonne note : aucun des criteres d'octave.
  {
    Rig rig;
    AcousticFeatures f = rig.analyse(fundamental.data(), kFrame, kMidiA4);
    AcousticContext ctx = rig.context(fundamental.data(), kFrame, kMidiA4);
    const OverblowResult o = AcousticQuality::evaluateOverblow(f, ctx);
    assert(o.valid && !o.detected && !o.pitchOctaveAbove && !o.octaveEnergyDominant);
  }

  // LE CAS QUI COMPTE : le pitch dit "une octave au-dessus" mais le PCM est
  // celui d'une note juste. C'est l'erreur de periode classique d'un estimateur
  // temporel. Le critere spectral doit la rattraper, sinon le regulateur
  // couperait de l'air sur une note correcte.
  {
    AcousticFeatures f = healthyFeatures();
    f.pitchHz = 880.0f;
    f.pitchMidi = kMidiA5;
    AcousticContext ctx = plainContext(kMidiA4);
    ctx.frame = fundamental.data();
    ctx.frameSize = kFrame;
    const OverblowResult o = AcousticQuality::evaluateOverblow(f, ctx);
    assert(o.valid && o.pitchOctaveAbove && o.confident);
    assert(!o.octaveEnergyDominant);
    assert(!o.detected);
  }

  // Miroir : l'energie est sur l'octave, mais le pitch est reste sur la
  // fondamentale visee. Une note simplement brillante ne doit pas declencher.
  {
    AcousticFeatures f = healthyFeatures();
    AcousticContext ctx = plainContext(kMidiA4);
    ctx.frame = octave.data();
    ctx.frameSize = kFrame;
    const OverblowResult o = AcousticQuality::evaluateOverblow(f, ctx);
    assert(o.valid && o.octaveEnergyDominant && !o.pitchOctaveAbove && !o.detected);
  }

  // Confiance insuffisante : entre le seuil du detecteur (0,80) et celui,
  // volontairement plus strict, de l'overblow (0,85).
  {
    AcousticFeatures f = healthyFeatures();
    f.pitchHz = 880.0f;
    f.pitchMidi = kMidiA5;
    f.pitchConfidence = 0.82f;
    AcousticContext ctx = plainContext(kMidiA4);
    ctx.frame = octave.data();
    ctx.frameSize = kFrame;
    const OverblowResult o = AcousticQuality::evaluateOverblow(f, ctx);
    assert(o.valid && o.pitchOctaveAbove && o.octaveEnergyDominant);
    assert(!o.confident && !o.detected);
  }

  // Sans PCM, deux criteres sur trois ne font PAS un verdict degrade : ils ne
  // font pas de verdict.
  {
    AcousticFeatures f = healthyFeatures();
    f.pitchHz = 880.0f;
    f.pitchMidi = kMidiA5;
    AcousticContext ctx = plainContext(kMidiA4);
    const OverblowResult o = AcousticQuality::evaluateOverblow(f, ctx);
    assert(!o.valid && !o.detected && !o.octaveRatioMeasured);
  }

  // Entrees insuffisantes ou absurdes.
  {
    AcousticFeatures f = healthyFeatures();
    f.pitchHz = 880.0f; f.pitchMidi = kMidiA5;
    AcousticContext ctx = plainContext(0);          // aucune note visee
    ctx.frame = octave.data(); ctx.frameSize = kFrame;
    assert(!AcousticQuality::evaluateOverblow(f, ctx).valid);

    ctx.expectedMidi = kMidiA4;
    f.pitchConfidence = 0.10f;                      // pitch rejete par le detecteur
    assert(!AcousticQuality::evaluateOverblow(f, ctx).valid);

    f = healthyFeatures(); f.pitchHz = NAN; f.pitchMidi = kMidiA5;
    assert(!AcousticQuality::evaluateOverblow(f, ctx).valid);

    f = healthyFeatures(); f.pitchHz = 880.0f; f.pitchMidi = kMidiA5;
    ctx.frameSize = 1;                              // frame trop courte
    assert(!AcousticQuality::evaluateOverblow(f, ctx).valid);
  }
}

// ===========================================================================
// 6.4 - Couac : bref, haut, et suivi d'un retour a la note
// ===========================================================================
void quality_squeak_needs_burst_brevity_and_return() {
  std::vector<float> buf(kFrame);

  // Sequence complete : note tenue, deux frames de couac, retour.
  {
    Rig rig;
    SqueakDetector sq;
    size_t start = 0;
    bool sawCandidate = false;
    bool sawConfirmed = false;

    for (int i = 0; i < 10; i++) {                       // note tenue
      goodNote(buf, 440.0f, 0.02f, start); start += kFrame;
      AcousticFeatures f = rig.analyse(buf.data(), kFrame, kMidiA4);
      AcousticContext ctx = rig.context(buf.data(), kFrame, kMidiA4);
      const SqueakResult r = AcousticQuality::updateSqueak(sq, f, ctx);
      assert(!r.candidate && !r.confirmed);
    }
    for (int i = 0; i < 2; i++) {                        // le couac
      audiosig::fluteLike(buf.data(), kFrame, 2637.0f, 0.35f, 0.05f, kFs, start);
      start += kFrame;
      AcousticFeatures f = rig.analyse(buf.data(), kFrame, kMidiA4);
      AcousticContext ctx = rig.context(buf.data(), kFrame, kMidiA4);
      const SqueakResult r = AcousticQuality::updateSqueak(sq, f, ctx);
      assert(r.valid && r.candidate && !r.rejectedTooLong);
      sawCandidate = true;
    }
    for (int i = 0; i < 4; i++) {                        // retour a la note
      goodNote(buf, 440.0f, 0.02f, start); start += kFrame;
      AcousticFeatures f = rig.analyse(buf.data(), kFrame, kMidiA4);
      AcousticContext ctx = rig.context(buf.data(), kFrame, kMidiA4);
      const SqueakResult r = AcousticQuality::updateSqueak(sq, f, ctx);
      if (r.confirmed) { sawConfirmed = true; assert(r.eventFrames == 2); }
      assert(!r.candidate);
    }
    assert(sawCandidate && sawConfirmed);
  }

  // Trop long : ce n'est plus un accident, c'est une note - juste pas la bonne.
  {
    Rig rig;
    SqueakDetector sq;
    size_t start = 0;
    for (int i = 0; i < 10; i++) {
      goodNote(buf, 440.0f, 0.02f, start); start += kFrame;
      AcousticQuality::updateSqueak(sq, rig.analyse(buf.data(), kFrame, kMidiA4),
                                    rig.context(buf.data(), kFrame, kMidiA4));
    }
    bool rejected = false;
    for (int i = 0; i < AQ_SQUEAK_MAX_FRAMES + 3; i++) {
      audiosig::fluteLike(buf.data(), kFrame, 2637.0f, 0.35f, 0.05f, kFs, start);
      start += kFrame;
      const SqueakResult r = AcousticQuality::updateSqueak(
          sq, rig.analyse(buf.data(), kFrame, kMidiA4),
          rig.context(buf.data(), kFrame, kMidiA4));
      if (i >= AQ_SQUEAK_MAX_FRAMES) { assert(!r.candidate && r.rejectedTooLong); rejected = true; }
    }
    assert(rejected);
    // Et meme si la note revient ensuite, l'evenement n'est PAS confirme.
    for (int i = 0; i < 4; i++) {
      goodNote(buf, 440.0f, 0.02f, start); start += kFrame;
      const SqueakResult r = AcousticQuality::updateSqueak(
          sq, rig.analyse(buf.data(), kFrame, kMidiA4),
          rig.context(buf.data(), kFrame, kMidiA4));
      assert(!r.confirmed);
    }
  }

  // Sans retour a la cible, l'evenement n'est jamais confirme.
  {
    Rig rig;
    SqueakDetector sq;
    size_t start = 0;
    for (int i = 0; i < 10; i++) {
      goodNote(buf, 440.0f, 0.02f, start); start += kFrame;
      AcousticQuality::updateSqueak(sq, rig.analyse(buf.data(), kFrame, kMidiA4),
                                    rig.context(buf.data(), kFrame, kMidiA4));
    }
    audiosig::fluteLike(buf.data(), kFrame, 2637.0f, 0.35f, 0.05f, kFs, start);
    start += kFrame;
    assert(AcousticQuality::updateSqueak(sq, rig.analyse(buf.data(), kFrame, kMidiA4),
                                         rig.context(buf.data(), kFrame, kMidiA4)).candidate);
    for (int i = 0; i < AQ_SQUEAK_RETURN_FRAMES + 4; i++) {
      goodNote(buf, 659.26f, 0.02f, start); start += kFrame;   // une autre note
      const SqueakResult r = AcousticQuality::updateSqueak(
          sq, rig.analyse(buf.data(), kFrame, kMidiA4),
          rig.context(buf.data(), kFrame, kMidiA4));
      assert(!r.confirmed);
    }
  }

  // Un trou dans la sequence de frames rend la duree non mesurable : la
  // detection le DIT et repart de zero, au lieu de compter a travers le trou.
  {
    SqueakDetector sq;
    primeSqueak(sq, 600.0f, kMidiA4, 100);
    AcousticFeatures f = healthyFeatures();
    f.frameSequence = 105;                 // quatre frames perdues
    AcousticContext ctx = plainContext();
    const SqueakResult r = AcousticQuality::updateSqueak(sq, f, ctx);
    assert(r.historyGap && !r.valid && !r.candidate);
    assert(sq.baselineCount == 0);
  }

  // Le silence efface le timbre tenu : la note qui revient n'a aucune raison
  // d'avoir la brillance de celle d'avant.
  {
    SqueakDetector sq;
    primeSqueak(sq, 600.0f, kMidiA4, 10);
    AcousticFeatures f = healthyFeatures();
    f.frameSequence = 11;
    f.soundDetected = false;
    f.rms = 0.0f;
    const SqueakResult r = AcousticQuality::updateSqueak(sq, f, plainContext());
    assert(!r.valid && sq.baselineCount == 0 && sq.lastNoteMidi == 0);
  }

  // Sans ligne de base, aucun verdict : le premier instant d'une note ne peut
  // pas etre un couac.
  {
    SqueakDetector sq;
    AcousticFeatures f = healthyFeatures();
    AcousticContext ctx = plainContext();
    ctx.fftFresh = true;
    f.spectralCentroid = 8000.0f;         // brillance enorme des la premiere frame
    f.pitchMidi = kMidiA4 + 24;
    f.pitchHz = PitchMath::midiToHz(kMidiA4 + 24);
    for (uint8_t i = 0; i < AQ_SQUEAK_MIN_BASELINE_FRAMES; i++) {
      f.frameSequence = (uint32_t)i + 1u;
      const SqueakResult r = AcousticQuality::updateSqueak(sq, f, ctx);
      assert(!r.valid && !r.candidate);
    }
  }

  // Sans brillance mesurable (ni PCM ni FFT fraiche), pas de verdict.
  {
    SqueakDetector sq;
    primeSqueak(sq, 600.0f, kMidiA4, 10);
    AcousticFeatures f = healthyFeatures();
    f.frameSequence = 11;
    AcousticContext ctx = plainContext();
    ctx.fftFresh = false;
    const SqueakResult r = AcousticQuality::updateSqueak(sq, f, ctx);
    assert(!r.valid && r.brightnessHz == 0.0f);
  }

  // Sans note visee ET sans note deja tenue, "revenir a la note" n'a pas de
  // sens : la detection apprend la note et la ligne de base, mais ne rend aucun
  // verdict.
  {
    SqueakDetector sq;
    AcousticFeatures f = healthyFeatures();
    AcousticContext ctx = plainContext(0);
    for (uint32_t i = 1; i <= 2; i++) {
      f.frameSequence = i;
      const SqueakResult r = AcousticQuality::updateSqueak(sq, f, ctx);
      assert(!r.valid && r.targetMidi == (i == 1 ? 0 : kMidiA4));
    }
    assert(sq.lastNoteMidi == kMidiA4 && sq.baselineCount == 2);
  }

  // Une note voisine n'est pas un couac, meme brillante : c'est une fausse note.
  {
    SqueakDetector sq;
    primeSqueak(sq, 600.0f, kMidiA4, 10);
    AcousticFeatures f = healthyFeatures();
    f.frameSequence = 11;
    f.spectralCentroid = 5000.0f;
    f.pitchMidi = kMidiA4 + AQ_SQUEAK_MIN_SEMITONES - 1;
    f.pitchHz = PitchMath::midiToHz(f.pitchMidi);
    const SqueakResult r = AcousticQuality::updateSqueak(sq, f, plainContext());
    assert(r.valid && !r.candidate);
  }
}

// ===========================================================================
// 6.5 - Classification : l'ordre de priorite est celui qui est documente
// ===========================================================================
void quality_state_priority_is_respected() {
  // 1. L'ecretage prime sur tout : il rend toutes les autres mesures douteuses.
  {
    AcousticFeatures f = healthyFeatures();
    f.clipping = true;
    f.clippingRatio = 0.2f;
    f.pitchMidi = kMidiA4 + 5;                 // fausse note
    f.pitchHz = PitchMath::midiToHz(f.pitchMidi);
    f.pitchStability = 0.1f;                   // instable
    f.snrDb = 2.0f;                            // faible
    f.harmonicToNoiseRatio = -5.0f;            // souffle
    AcousticClassification c = AcousticQuality::classify(f, plainContext(), nullptr);
    assert(c.classified && c.state == ACOUSTIC_CLIPPING);
  }

  // 2. Le silence prime sur tout le reste : sans son, rien n'est mesurable.
  {
    AcousticFeatures f = healthyFeatures();
    f.soundDetected = false;
    f.rms = 0.0f;
    f.pitchStability = 0.0f;
    AcousticClassification c = AcousticQuality::classify(f, plainContext(), nullptr);
    assert(c.classified && c.state == ACOUSTIC_SILENCE);
    // Un RMS sous le plancher absolu est du silence, meme si la porte de niveau
    // en amont s'est trompee.
    f.soundDetected = true;
    f.rms = AQ_SILENCE_RMS * 0.5f;
    c = AcousticQuality::classify(f, plainContext(), nullptr);
    assert(c.state == ACOUSTIC_SILENCE);
  }

  // 3. Le couac prime sur l'instabilite et sur la fausse note : bref et nomme,
  //    il disparaitrait sinon dans une statistique d'instabilite.
  {
    SqueakDetector sq;
    primeSqueak(sq, 600.0f, kMidiA4, 10);
    AcousticFeatures f = healthyFeatures();
    f.frameSequence = 11;
    f.spectralCentroid = 6000.0f;
    f.pitchMidi = kMidiA4 + 12;
    f.pitchHz = PitchMath::midiToHz(f.pitchMidi);
    f.pitchStability = 0.05f;
    AcousticClassification c = AcousticQuality::classify(f, plainContext(), &sq);
    assert(c.classified && c.state == ACOUSTIC_SQUEAK);
  }

  // 5. Fausse note quand aucune cause n'est identifiee.
  {
    AcousticFeatures f = healthyFeatures();
    f.pitchMidi = kMidiA4 + 2;
    f.pitchHz = PitchMath::midiToHz(f.pitchMidi);
    AcousticClassification c = AcousticQuality::classify(f, plainContext(), nullptr);
    assert(c.classified && c.state == ACOUSTIC_WRONG_NOTE);
    assert(nearly(c.centsFromExpected, 200.0f, 1.0f));
    // La meme note, mais sans cible declaree : on ne peut PAS parler de fausse
    // note, et le verdict le dit.
    c = AcousticQuality::classify(f, plainContext(0), nullptr);
    assert(c.missingExpectedNote && c.state != ACOUSTIC_WRONG_NOTE);
  }

  // 6. Faible prime sur souffle : un son trop faible rend la mesure de souffle
  //    peu fiable, on mesurerait le rapport du bruit a lui-meme.
  {
    AcousticFeatures f = healthyFeatures();
    f.snrDb = 5.0f;
    f.harmonicToNoiseRatio = -10.0f;
    f.spectralFlatness = 0.9f;
    AcousticClassification c = AcousticQuality::classify(f, plainContext(), nullptr);
    assert(c.classified && c.state == ACOUSTIC_WEAK);
    assert(c.breathiness.valid && c.breathiness.value > AQ_BREATHY_MAX);
  }

  // 7. Souffle prime sur instable.
  {
    AcousticFeatures f = healthyFeatures();
    f.harmonicToNoiseRatio = -10.0f;
    f.spectralFlatness = 0.9f;
    f.pitchStability = 0.05f;
    AcousticClassification c = AcousticQuality::classify(f, plainContext(), nullptr);
    assert(c.classified && c.state == ACOUSTIC_BREATHY);
  }

  // 8. Instable en dernier : le defaut le plus fin ne masque rien.
  {
    AcousticFeatures f = healthyFeatures();
    f.pitchStability = AQ_UNSTABLE_STABILITY - 0.05f;
    AcousticClassification c = AcousticQuality::classify(f, plainContext(), nullptr);
    assert(c.classified && c.state == ACOUSTIC_UNSTABLE);
  }

  // 9. Rien a redire.
  {
    AcousticFeatures f = healthyFeatures();
    AcousticClassification c = AcousticQuality::classify(f, plainContext(), nullptr);
    assert(c.classified && c.state == ACOUSTIC_GOOD);
  }

  // Chaque etat porte un nom, aucun ne rend "?".
  const AcousticState kAll[] = {ACOUSTIC_SILENCE, ACOUSTIC_GOOD,       ACOUSTIC_WEAK,
                                ACOUSTIC_BREATHY, ACOUSTIC_UNSTABLE,   ACOUSTIC_WRONG_NOTE,
                                ACOUSTIC_OVERBLOW, ACOUSTIC_SQUEAK,    ACOUSTIC_CLIPPING};
  for (AcousticState s : kAll) {
    const char* n = AcousticQuality::stateName(s);
    assert(n != nullptr && n[0] != '\0' && n[0] != '?');
  }
}

// ===========================================================================
// 6.6 - Classification : ce qui n'est pas mesure n'est pas invente
// ===========================================================================
void quality_classify_refuses_to_guess() {
  // Du son, mais aucune mesure exploitable : pas de pitch, pas de spectre, pas
  // de PCM, pas de note visee. Aucun etat n'est rendu.
  {
    AcousticFeatures f = healthyFeatures();
    f.pitchConfidence = 0.1f;
    f.pitchHz = 0.0f;
    f.pitchMidi = 0;
    f.spectralValid = false;
    AcousticContext ctx = plainContext(0);
    ctx.fftFresh = false;
    AcousticClassification c = AcousticQuality::classify(f, ctx, nullptr);
    assert(!c.classified);
    assert(c.missingPitch && c.missingSpectrum && c.missingExpectedNote);
    assert(!c.breathiness.valid);
  }

  // NaN : une comparaison avec NaN est fausse dans les deux sens, donc une
  // cascade de tests laisserait la frame filer jusqu'a ACOUSTIC_GOOD.
  for (int which = 0; which < 3; which++) {
    AcousticFeatures f = healthyFeatures();
    if (which == 0) f.rms = NAN;
    if (which == 1) f.rmsDbFS = NAN;
    if (which == 2) f.clippingRatio = NAN;
    AcousticClassification c = AcousticQuality::classify(f, plainContext(), nullptr);
    assert(!c.classified);
    assert(c.state != ACOUSTIC_GOOD);
  }

  // Un pitch HORS de la plage que le detecteur sait couvrir n'est pas une note :
  // c'est un repliement ou un artefact. Quelle que soit la confiance affichee,
  // il ne doit fonder aucun verdict ni aucune composante de qualite.
  for (float hz : {MIC_PITCH_MIN_HZ * 0.5f, MIC_PITCH_MAX_HZ * 2.0f}) {
    AcousticFeatures f = healthyFeatures();
    f.pitchHz = hz;
    f.pitchMidi = (int16_t)PitchMath::hzToMidi(hz);
    f.pitchConfidence = 0.99f;
    AcousticContext ctx = plainContext(f.pitchMidi);
    const AcousticClassification c = AcousticQuality::classify(f, ctx, nullptr);
    assert(c.missingPitch);
    assert(!c.classified && c.state != ACOUSTIC_GOOD);
    BreathinessResult b;
    b.valid = true;
    b.value = 0.0f;
    const QualityScore q =
        AcousticQuality::computeAcousticQuality(f, ctx, b, AQ_ATTACK_NOT_MEASURED);
    assert(!q.intonationMeasured && !q.confidenceMeasured);
  }

  // Stabilite inconnue : PitchResult::stability vaut 0 tant que l'historique
  // n'est pas rempli, exactement comme pour une note tres instable. Sans la
  // confirmation de l'appelant, on ne classe PAS "instable" - sinon chaque
  // debut de note serait un defaut.
  {
    AcousticFeatures f = healthyFeatures();
    f.pitchStability = 0.0f;
    AcousticContext ctx = plainContext();
    ctx.stabilityMeasured = false;
    AcousticClassification c = AcousticQuality::classify(f, ctx, nullptr);
    assert(c.classified && c.state == ACOUSTIC_GOOD && c.missingStability);
    ctx.stabilityMeasured = true;
    c = AcousticQuality::classify(f, ctx, nullptr);
    assert(c.state == ACOUSTIC_UNSTABLE && !c.missingStability);
  }

  // Sans historique de couac, la detection est DECLAREE absente et non
  // approximee.
  {
    AcousticFeatures f = healthyFeatures();
    AcousticClassification c = AcousticQuality::classify(f, plainContext(), nullptr);
    assert(c.missingSqueakHistory && !c.squeak.valid);
    SqueakDetector sq;
    c = AcousticQuality::classify(f, plainContext(), &sq);
    assert(!c.missingSqueakHistory);
  }

  // Sans SNR, le niveau absolu sert de repli - mais le verdict dit qu'il a
  // servi, parce que c'est un critere strictement plus faible.
  {
    AcousticFeatures f = healthyFeatures();
    f.snrValid = false;
    f.rmsDbFS = AQ_WEAK_LEVEL_DBFS - 5.0f;
    f.rms = 0.005f;
    AcousticClassification c = AcousticQuality::classify(f, plainContext(), nullptr);
    assert(c.classified && c.state == ACOUSTIC_WEAK && c.missingSnr);
    // Avec un vrai profil de bruit, c'est le SNR qui decide, pas le niveau.
    f.snrValid = true;
    f.snrDb = 30.0f;
    c = AcousticQuality::classify(f, plainContext(), nullptr);
    assert(!c.missingSnr && c.state != ACOUSTIC_WEAK);
    // Un SNR obtenu par repli sur l'ambiance surestime probablement la qualite.
    f.snrUsedFallback = true;
    c = AcousticQuality::classify(f, plainContext(), nullptr);
    assert(c.snrUsedFallback);
  }
}

// ===========================================================================
// 6.7 - Note de qualite : ponderation nommee, attaque non inventee
// ===========================================================================
void quality_score_reports_what_it_could_not_measure() {
  const QualityWeights w;
  const float kTotal = w.intonation + w.stability + w.confidence + w.snr + w.harmonic +
                       w.lowBreath + w.attack;
  assert(nearly(kTotal, 1.0f, 1e-6f));

  AcousticFeatures f = healthyFeatures();
  AcousticContext ctx = plainContext();
  BreathinessResult breath;
  breath.valid = true;
  breath.value = 0.0f;
  breath.weightUsed = 1.0f;

  // Note parfaite, attaque NON MESUREE : le score porte sur 90 % des criteres
  // et le dit. Verification arithmetique exacte, pour que tout changement de
  // ponderation se voie ici.
  {
    const QualityScore q =
        AcousticQuality::computeAcousticQuality(f, ctx, breath, AQ_ATTACK_NOT_MEASURED);
    assert(q.valid && !q.attackMeasured && !q.attackRejected);
    assert(nearly(q.weightUsed, 1.0f - w.attack, 1e-5f));
    const float expected =
        (w.intonation * 1.0f + w.stability * 0.95f + w.confidence * 0.99f +
         w.snr * clampRef(30.0f / AQ_QUALITY_SNR_REF_DB) +
         w.harmonic * clampRef((24.0f - AQ_QUALITY_HNR_MIN_DB) /
                               (AQ_QUALITY_HNR_GOOD_DB - AQ_QUALITY_HNR_MIN_DB)) +
         w.lowBreath * 1.0f) /
        (kTotal - w.attack);
    assert(nearly(q.score, expected, 1e-4f));
    assert(q.intonationMeasured && q.intonationVsExpected && q.stabilityMeasured &&
           q.confidenceMeasured && q.snrMeasured && q.harmonicMeasured && q.breathMeasured);
  }

  // Attaque fournie : la composante entre dans le calcul et `weightUsed` passe
  // a 1. Une attaque parfaite monte le score, une attaque ratee le descend.
  {
    const QualityScore good = AcousticQuality::computeAcousticQuality(f, ctx, breath, 1.0f);
    const QualityScore bad = AcousticQuality::computeAcousticQuality(f, ctx, breath, 0.0f);
    const QualityScore none =
        AcousticQuality::computeAcousticQuality(f, ctx, breath, AQ_ATTACK_NOT_MEASURED);
    assert(good.valid && good.attackMeasured && nearly(good.weightUsed, 1.0f, 1e-5f));
    assert(bad.valid && bad.attackMeasured);
    assert(good.score > none.score && none.score > bad.score);
  }

  // Attaque absurde : REFUSEE et signalee. La traiter silencieusement comme
  // "non mesuree" masquerait un appelant casse.
  for (float bogus : {2.0f, -5.0f, NAN, INFINITY}) {
    const QualityScore q = AcousticQuality::computeAcousticQuality(f, ctx, breath, bogus);
    assert(q.attackRejected && !q.attackMeasured);
    assert(nearly(q.weightUsed, 1.0f - w.attack, 1e-5f));
  }

  // Chaque composante manquante retire SON poids, pas un autre.
  {
    AcousticFeatures g = f;
    g.snrValid = false;
    QualityScore q = AcousticQuality::computeAcousticQuality(g, ctx, breath, AQ_ATTACK_NOT_MEASURED);
    assert(!q.snrMeasured && nearly(q.weightUsed, 1.0f - w.attack - w.snr, 1e-5f));

    g = f; g.spectralValid = false;
    q = AcousticQuality::computeAcousticQuality(g, ctx, breath, AQ_ATTACK_NOT_MEASURED);
    assert(!q.harmonicMeasured && nearly(q.weightUsed, 1.0f - w.attack - w.harmonic, 1e-5f));

    AcousticContext c2 = ctx;
    c2.stabilityMeasured = false;
    q = AcousticQuality::computeAcousticQuality(f, c2, breath, AQ_ATTACK_NOT_MEASURED);
    assert(!q.stabilityMeasured && nearly(q.weightUsed, 1.0f - w.attack - w.stability, 1e-5f));

    BreathinessResult none;
    q = AcousticQuality::computeAcousticQuality(f, ctx, none, AQ_ATTACK_NOT_MEASURED);
    assert(!q.breathMeasured && nearly(q.weightUsed, 1.0f - w.attack - w.lowBreath, 1e-5f));
  }

  // Sans note visee, la justesse est mesuree contre le temperament le plus
  // proche : c'est une autre question, et le drapeau le dit.
  {
    AcousticContext c2 = plainContext(0);
    const QualityScore q =
        AcousticQuality::computeAcousticQuality(f, c2, breath, AQ_ATTACK_NOT_MEASURED);
    assert(q.intonationMeasured && !q.intonationVsExpected);
  }
  // Une note parfaitement accordee mais FAUSSE ne doit pas marquer 100 % de
  // justesse : c'est exactement le piege de f.cents.
  {
    AcousticFeatures g = f;
    g.pitchMidi = kMidiA4 + 2;
    g.pitchHz = PitchMath::midiToHz(g.pitchMidi);
    g.cents = 0.0f;
    const QualityScore wrong =
        AcousticQuality::computeAcousticQuality(g, ctx, breath, AQ_ATTACK_NOT_MEASURED);
    const QualityScore right =
        AcousticQuality::computeAcousticQuality(f, ctx, breath, AQ_ATTACK_NOT_MEASURED);
    assert(wrong.score < right.score - 0.2f);
  }

  // Sous la moitie des criteres, le score decrirait surtout ce qui manque : il
  // est REFUSE. Le cas qui compte n'est pas "rien n'a ete mesure" - trivial -
  // mais "un peu a ete mesure" : c'est la seule facon de verifier qu'un seuil
  // de couverture existe vraiment, et pas seulement un garde-fou contre la
  // division par zero.
  {
    AcousticFeatures g = healthyFeatures();
    g.pitchConfidence = 0.1f;        // retire justesse + confiance
    g.pitchHz = 0.0f; g.pitchMidi = 0;
    g.snrValid = false;              // retire le SNR
    g.spectralValid = false;         // retire le HNR
    AcousticContext c2 = plainContext();
    BreathinessResult none;

    // Seule la stabilite reste : 15 % des criteres.
    QualityScore q = AcousticQuality::computeAcousticQuality(g, c2, none, AQ_ATTACK_NOT_MEASURED);
    assert(q.stabilityMeasured);
    assert(q.weightUsed > 0.0f && q.weightUsed < AQ_QUALITY_MIN_WEIGHT);
    assert(!q.valid && q.score == 0.0f);

    // Stabilite + respiration + attaque : 35 %, toujours insuffisant.
    q = AcousticQuality::computeAcousticQuality(g, c2, breath, 1.0f);
    assert(q.stabilityMeasured && q.breathMeasured && q.attackMeasured);
    assert(q.weightUsed > 0.0f && q.weightUsed < AQ_QUALITY_MIN_WEIGHT);
    assert(!q.valid);

    // Plus rien du tout : refus aussi, evidemment.
    c2.stabilityMeasured = false;
    q = AcousticQuality::computeAcousticQuality(g, c2, none, AQ_ATTACK_NOT_MEASURED);
    assert(!q.valid && q.weightUsed == 0.0f);
  }

  // Ponderation degeneree : tout a zero, ou des poids negatifs.
  {
    QualityWeights zero = {0, 0, 0, 0, 0, 0, 0};
    assert(!AcousticQuality::computeAcousticQuality(f, ctx, breath, 1.0f, zero).valid);
    QualityWeights odd;
    odd.snr = -1.0f;
    odd.harmonic = NAN;
    const QualityScore q =
        AcousticQuality::computeAcousticQuality(f, ctx, breath, AQ_ATTACK_NOT_MEASURED, odd);
    assert(q.valid && !q.snrMeasured && !q.harmonicMeasured);
  }
}

// ===========================================================================
// 6.8 - De bout en bout, sur des signaux synthetiques
// ===========================================================================
void quality_end_to_end_states() {
  std::vector<float> buf(kFrame);

  // Un vrai profil de bruit, capture avant les notes. Son niveau est celui
  // d'une machinerie en marche : c'est ce qui rend le cas "note faible"
  // realiste - la note n'est pas faible dans l'absolu, elle est faible DEVANT
  // le bruit de l'instrument, et c'est tout l'objet de la PHASE 5.
  NoiseModel noise;
  noise.beginCapture(NOISE_AMBIENT);
  for (int i = 0; i < MIC_NOISE_MIN_FRAMES + 2; i++) {
    audiosig::whiteNoise(buf.data(), kFrame, 0.06f, 31u + (uint32_t)i);
    noise.accumulate(buf.data(), kFrame, nullptr);
  }
  assert(noise.endCapture());

  struct Case {
    const char* name;
    AcousticState expected;
  };

  // Chaque cas rejoue une note tenue assez longtemps pour que la stabilite et
  // la FFT soient disponibles, puis classe la derniere frame.
  auto run = [&](int kind, int expectedMidi) {
    Rig rig;
    SqueakDetector sq;
    AcousticClassification last;
    size_t start = 0;
    for (int i = 0; i < 12; i++) {
      switch (kind) {
        case 0: goodNote(buf, 440.0f, 0.02f, start); break;                    // bonne note
        case 1: goodNote(buf, 440.0f, 0.50f, start); break;                    // souffle
        case 2: audiosig::fluteLike(buf.data(), kFrame, 440.0f, 0.10f, 0.005f, kFs, start);
                break;                                                          // faible
        case 3: goodNote(buf, 493.88f, 0.02f, start); break;                   // fausse note
        case 4: goodNote(buf, 880.0f, 0.02f, start); break;                    // overblow
        case 5: { ToneSpec s; s.sampleRate = kFs; s.f0 = 440.0f; s.amp = 1.6f;
                  s.clipAt = 0.99f; audiosig::fill(buf.data(), kFrame, s, start); break; }
        case 6: audiosig::silence(buf.data(), kFrame); break;
        default: break;
      }
      start += kFrame;
      AcousticFeatures f = rig.analyse(buf.data(), kFrame, expectedMidi);
      const SnrResult snr = noise.snrDb(f.rms, NOISE_AMBIENT);
      f.snrValid = snr.valid;
      f.snrUsedFallback = snr.usedFallback;
      f.snrDb = snr.db;
      last = AcousticQuality::classify(f, rig.context(buf.data(), kFrame, expectedMidi), &sq);
    }
    return last;
  };

  const Case kCases[] = {
      {"bonne note", ACOUSTIC_GOOD},   {"souffle", ACOUSTIC_BREATHY},
      {"note faible", ACOUSTIC_WEAK},  {"fausse note", ACOUSTIC_WRONG_NOTE},
      {"overblow", ACOUSTIC_OVERBLOW}, {"ecretage", ACOUSTIC_CLIPPING},
      {"silence", ACOUSTIC_SILENCE},
  };
  for (int i = 0; i < (int)(sizeof(kCases) / sizeof(kCases[0])); i++) {
    const AcousticClassification c = run(i, kMidiA4);
    if (!(c.classified && c.state == kCases[i].expected)) {
      // Sur stderr, non tamponne : un diagnostic perdu par l'abort d'un assert
      // ne sert a rien.
      fprintf(stderr, "  [quality] %s -> %s (classified=%d) au lieu de %s\n", kCases[i].name,
              AcousticQuality::stateName(c.state), (int)c.classified,
              AcousticQuality::stateName(kCases[i].expected));
    }
    assert(c.classified);
    assert(c.state == kCases[i].expected);
  }

  // Un couac au milieu d'une note tenue, de bout en bout - et ce qu'il laisse
  // derriere lui. Juste apres l'accident l'etat est INSTABLE, et ce n'est pas
  // un defaut du classement : l'historique de pitch contient encore le couac,
  // donc la note EST instable sur les MIC_PITCH_HISTORY dernieres frames. Il
  // faut autant de frames propres pour qu'elle redevienne bonne.
  {
    const int kSqueakAt = 12;
    const int kSqueakLen = 2;
    Rig rig;
    SqueakDetector sq;
    size_t start = 0;
    int squeakFrames = 0;
    int unstableAfter = 0;
    AcousticState last = ACOUSTIC_SILENCE;
    const int kTotal = kSqueakAt + kSqueakLen + MIC_PITCH_HISTORY + 2;
    for (int i = 0; i < kTotal; i++) {
      const bool squeaking = (i >= kSqueakAt && i < kSqueakAt + kSqueakLen);
      if (squeaking) audiosig::fluteLike(buf.data(), kFrame, 2637.0f, 0.35f, 0.05f, kFs, start);
      else goodNote(buf, 440.0f, 0.02f, start);
      start += kFrame;
      AcousticFeatures f = rig.analyse(buf.data(), kFrame, kMidiA4);
      const SnrResult snr = noise.snrDb(f.rms, NOISE_AMBIENT);
      f.snrValid = snr.valid; f.snrDb = snr.db;
      const AcousticClassification c =
          AcousticQuality::classify(f, rig.context(buf.data(), kFrame, kMidiA4), &sq);
      if (squeaking) { assert(c.state == ACOUSTIC_SQUEAK); squeakFrames++; }
      else {
        assert(c.state != ACOUSTIC_SQUEAK);
        if (i < kSqueakAt) assert(c.state == ACOUSTIC_GOOD);
        if (i == kSqueakAt + kSqueakLen) assert(c.state == ACOUSTIC_UNSTABLE);
        if (i > kSqueakAt + kSqueakLen && c.state == ACOUSTIC_UNSTABLE) unstableAfter++;
      }
      last = c.state;
    }
    assert(squeakFrames == kSqueakLen);
    assert(unstableAfter > 0);       // la trace du couac survit dans la stabilite
    assert(last == ACOUSTIC_GOOD);   // ...mais elle finit par s'effacer
  }

  // UN COUAC N'EST PAS UN OVERBLOW, ET RECIPROQUEMENT : le meme saut d'octave
  // est un couac s'il est bref, un overblow s'il s'installe. C'est la raison
  // d'etre de la priorite entre les deux.
  {
    Rig rig;
    SqueakDetector sq;
    size_t start = 0;
    int squeakFrames = 0;
    int overblowFrames = 0;
    for (int i = 0; i < 24; i++) {
      if (i >= 10) goodNote(buf, 880.0f, 0.02f, start);
      else goodNote(buf, 440.0f, 0.02f, start);
      start += kFrame;
      AcousticFeatures f = rig.analyse(buf.data(), kFrame, kMidiA4);
      const AcousticClassification c =
          AcousticQuality::classify(f, rig.context(buf.data(), kFrame, kMidiA4), &sq);
      if (c.state == ACOUSTIC_SQUEAK) squeakFrames++;
      if (c.state == ACOUSTIC_OVERBLOW) overblowFrames++;
    }
    assert(squeakFrames > 0 && squeakFrames <= AQ_SQUEAK_MAX_FRAMES);
    assert(overblowFrames > 0);
  }
}

}  // namespace

void quality_run_all_tests() {
  quality_brightness_is_exact_on_a_sine();
  quality_breathiness_is_monotone();
  quality_breathiness_refuses_what_it_cannot_measure();
  quality_overblow_requires_three_criteria();
  quality_squeak_needs_burst_brevity_and_return();
  quality_state_priority_is_respected();
  quality_classify_refuses_to_guess();
  quality_score_reports_what_it_could_not_measure();
  quality_end_to_end_states();
}

#ifdef STANDALONE_TEST_MAIN
#include <cstdio>
int main() { quality_run_all_tests(); printf("quality tests passed\n"); return 0; }
#endif
