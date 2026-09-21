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
#include "AudioFilters.h"
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

  // LA CHAINE DE FILTRAGE DE PRODUCTION.
  //
  // AudioAnalyzer::drainI2S() filtre le FLUX (MIC_FILTER_HP_HZ..LP_HZ) avant
  // l'anneau : aucune frame analysee n'est du PCM brut. Les tests qui
  // ETALONNENT une mesure doivent donc l'activer, sans quoi ils calibrent des
  // seuils sur un signal que la production ne presente jamais - c'est
  // exactement ce qui avait laisse passer l'extrapolation de plancher a tout le
  // spectre.
  //
  // Elle est INACTIVE par defaut, et c'est delibere : les tests de priorite de
  // classement et de refus construisent des signaux limites (silence, ecretage,
  // bruit pur) ou des features a la main, et y ajouter un filtre deplacerait ce
  // qu'ils cherchent a piquer sans rien prouver de plus. Ce qui est etalonne
  // passe par la chaine ; ce qui est classe garde son signal.
  AudioFilterChain filters;
  bool filterStream = false;

  // A appeler sur CHAQUE bloc, dans l'ordre du flux et avant analyse() : le
  // filtre est un IIR, sa memoire doit s'etablir sur les frames precedentes.
  void stream(float* buf, size_t n) {
    if (filterStream) filters.processBlock(buf, n);
  }

  void useProductionChain() {
    filterStream = true;
    filters.configureDefaults();   // configure() remet aussi la memoire a zero
  }

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
    // fftValid, et non `runFft` : demander la FFT ne suffit pas a l'obtenir.
    // Quand elle est compilee hors du binaire, la platitude vaut 0 sans avoir
    // ete mesuree, et un `fftFresh` optimiste la ferait passer pour un spectre
    // parfaitement tonal.
    fftFresh = f.fftValid;
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
  // Le HNR d'une frame saine vient de la MESURE spectrale : les seuils
  // AQ_BREATH_HNR_* et AQ_QUALITY_HNR_GOOD_DB ne decrivent que cette
  // echelle-la, et la composante est absente sans ce drapeau.
  f.hnrIsSpectral = true;
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
    // Quatre frames, et la FFT tombe sur la PREMIERE (decimation 0) : la
    // derniere frame n'a donc ni platitude fraiche ni HNR spectral, et cette
    // boucle exerce la seule composante inter-partiels - celle qui survit
    // jusqu'au bout du balayage, quand le pitch a lache et que les deux
    // autres composantes ont disparu. La monotonie des trois composantes
    // ensemble, sur la plage ou le pitch tient, est verrouillee separement par
    // quality_breathiness_rises_with_breath_on_the_spectral_scale().
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
    assert(b.usedInterHarmonic);
#if MIC_FFT_ENABLED
    // Les trois composantes sont la : HNR spectral, inter-partiels, platitude.
    assert(b.usedHnr && b.usedFlatness);
    assert(nearly(b.weightUsed, AQ_BREATH_W_HNR + AQ_BREATH_W_INTER + AQ_BREATH_W_FLATNESS, 1e-5f));
#else
    // Sans FFT il n'y a ni platitude mesuree ni HNR spectral : seule la mesure
    // inter-partiels, faite sur le PCM, subsiste. Elle suffit a un verdict, et
    // `weightUsed` dit exactement ce qui a servi.
    assert(!b.usedHnr && !b.usedFlatness);
    assert(nearly(b.weightUsed, AQ_BREATH_W_INTER, 1e-5f));
#endif
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
// 6.2 bis - Recalibrage des seuils HNR sur la MESURE spectrale
//
// AcousticFeatures::harmonicToNoiseRatio est rempli par deux mesures qui ne
// sont PAS sur la meme echelle : la mesure spectrale (spectre FFT complet,
// fenetres de raies + plancher median) quand la FFT a tourne sur la frame, et
// l'approximation Goertzel a quatre raies sinon. Les trois seuils
// AQ_BREATH_HNR_TONE_DB, AQ_BREATH_HNR_NOISE_DB et AQ_QUALITY_HNR_GOOD_DB
// etaient calibres sur la seconde. Ces tests etablissent la plage reelle de la
// premiere, verrouillent les seuils dessus, et echouent si on remet les
// anciens.
//
// TOUTES CES VALEURS SONT MESUREES SUR DU PCM SYNTHETIQUE. Elles valident le
// traitement du signal, pas le comportement acoustique d'une flute : sur un
// microphone reel, le plancher de la piece et le bruit de la pompe les
// deplaceront, et le recalibrage sera a refaire.
//
// TOUT CE BLOC EXIGE LA FFT. Sans elle il n'y a pas de mesure spectrale du
// tout : `hnrIsSpectral` ne devient jamais vrai, la composante HNR est
// absente partout, et il n'y a plus rien a calibrer. Le dire vaut mieux qu'une
// suite verte qui n'a rien execute.
//
// IL EXIGE AUSSI LA CHAINE DE FILTRAGE DECRITE PAR settings.h. Un etalonnage
// decrit un signal, et le signal analyse depend de MIC_FILTER_HP_HZ et
// MIC_FILTER_LP_HZ : c'est tout le defaut que ce bloc corrige. Si ces coupures
// changent - ou passent a 0 - ces tests echouent, et c'est le comportement
// VOULU : les trois seuils sont alors a re-mesurer, pas a reporter tels quels.
// Ce qui doit rester vrai dans toutes les configurations, c'est la MESURE
// elle-meme ; c'est verifie dans test_spectral_hnr.cpp, qui ne depend d'aucun
// seuil d'instrument.
// ===========================================================================
#if MIC_FFT_ENABLED

// Composantes de reference, recalculees ICI a partir des seuils. Les tests
// comparent la valeur rendue par le firmware a cette arithmetique, de sorte
// qu'un changement de seuil se voie immediatement.
float breathHnrComponent(float db) {
  return clampRef((db - AQ_BREATH_HNR_TONE_DB) /
                  (AQ_BREATH_HNR_NOISE_DB - AQ_BREATH_HNR_TONE_DB));
}
float qualityHnrComponent(float db) {
  return clampRef((db - AQ_QUALITY_HNR_MIN_DB) /
                  (AQ_QUALITY_HNR_GOOD_DB - AQ_QUALITY_HNR_MIN_DB));
}

// Analyse `frames` frames consecutives et rend la DERNIERE. `frames` est
// choisi pour que la derniere retombe sur une frame FFT (decimation 0 modulo
// MIC_SPECTRAL_DECIMATION), sans quoi le HNR rendu serait celui de
// l'approximation Goertzel et non la mesure que ces seuils decrivent.
constexpr int kFramesEndingOnFft = MIC_SPECTRAL_DECIMATION + 1;

AcousticFeatures sustainedNote(Rig& rig, std::vector<float>& buf, float f0, float breath,
                               int midi, AcousticContext& ctxOut) {
  AcousticFeatures f;
  for (int i = 0; i < kFramesEndingOnFft; i++) {
    audiosig::fluteLike(buf.data(), buf.size(), f0, 0.4f, breath, kFs, (size_t)i * buf.size());
    // Le flux passe dans la chaine AVANT l'analyse, comme dans drainI2S(), et
    // les quatre frames precedentes suffisent a etablir la memoire du filtre :
    // 128 ms, soit vingt fois la constante de temps du retrait de continu.
    rig.stream(buf.data(), buf.size());
    f = rig.analyse(buf.data(), buf.size(), midi);
    ctxOut = rig.context(buf.data(), buf.size(), midi);
  }
  return f;
}

// Meme note tenue, mais sur la chaine de PRODUCTION. C'est elle qui sert a
// etalonner les seuils : ils doivent decrire le signal que le firmware analyse
// reellement.
AcousticFeatures sustainedProductionNote(Rig& rig, std::vector<float>& buf, float f0,
                                         float breath, int midi, AcousticContext& ctxOut) {
  rig.useProductionChain();
  return sustainedNote(rig, buf, f0, breath, midi, ctxOut);
}

// La meme frame, filtree par une chaine dont la memoire est etablie : sert aux
// releves faits directement sur SpectralAnalyzer, hors du banc complet.
void productionFrame(std::vector<float>& buf, float f0, float amp, float breath, int index) {
  AudioFilterChain chain;
  chain.configureDefaults();
  for (int i = 0; i < index; i++) {
    audiosig::fluteLike(buf.data(), buf.size(), f0, amp, breath, kFs, (size_t)i * buf.size());
    chain.processBlock(buf.data(), buf.size());
  }
  audiosig::fluteLike(buf.data(), buf.size(), f0, amp, breath, kFs, (size_t)index * buf.size());
  chain.processBlock(buf.data(), buf.size());
}

// Releve direct sur l'analyseur, f0 IMPOSEE : sert a comparer deux signaux sans
// passer par le detecteur de pitch, qui ne survit pas aux memes niveaux de
// souffle selon que le flux a ete filtre ou non.
float directHnrDb(const std::vector<float>& frame, float f0) {
  SpectralAnalyzer sa;
  if (!sa.computeSpectrum(frame.data(), frame.size())) return 0.0f;
  const HarmonicNoiseRatio h = sa.harmonicNoiseRatio(f0, kFs);
  assert(h.valid);
  return h.db;
}

void productionNoiseFrame(std::vector<float>& buf, float amp, uint32_t seed, int frames) {
  AudioFilterChain chain;
  chain.configureDefaults();
  for (int i = 0; i < frames; i++) {
    audiosig::whiteNoise(buf.data(), buf.size(), amp, seed + (uint32_t)i);
    chain.processBlock(buf.data(), buf.size());
  }
}

// ---------------------------------------------------------------------------
// Generateur de note TIMBREE
//
// audio_signals.h s'arrete a la 4e harmonique : c'est precisement le point
// aveugle a exercer. Il faut H5..H8 pour construire une note dont l'energie
// vit au-dessus de ce que les quatre raies de Goertzel mesurent. Le generateur
// reste local plutot que dans audio_signals.h, qui est partage.
// ---------------------------------------------------------------------------
constexpr int kRichPartials = 8;

struct RichTone {
  float f0 = 0.0f;
  float partial[kRichPartials] = {};   // amplitudes ABSOLUES de H1..H8
  float noise = 0.0f;
  uint32_t seed = 4242u;
};

void fillRich(float* buf, size_t n, const RichTone& s, size_t startSample = 0) {
  audiosig::Lcg rng(s.seed + (uint32_t)startSample);
  for (size_t i = 0; i < n; i++) {
    const float t = (float)(startSample + i) / kFs;
    float v = 0.0f;
    for (int k = 0; k < kRichPartials; k++) {
      if (s.partial[k] == 0.0f) continue;
      v += s.partial[k] * sinf(2.0f * audiosig::kPi * s.f0 * (float)(k + 1) * t);
    }
    if (s.noise > 0.0f) v += s.noise * rng.bipolar();
    buf[i] = v;
  }
}

// Reproduction EXACTE de l'approximation Goertzel de AcousticFeatures, gardee
// ici pour servir de temoin : un test de non-regression ne doit pas mesurer sa
// reference a travers le code qu'il surveille.
float legacyHnrDb(const float* x, size_t n, float f0) {
  const HarmonicEnergies h = SpectralAnalyzer::harmonics(x, n, f0, kFs);
  if (!h.valid) return 0.0f;
  const float total = SpectralAnalyzer::totalPower(x, n);
  const float harmonic = 2.0f * h.harmonicTotal;
  const float residual = (total > harmonic) ? (total - harmonic) : 0.0f;
  float db = 0.0f;
  if (residual > 1e-12f && harmonic > 0.0f) db = 10.0f * log10f(harmonic / residual);
  else if (harmonic > 0.0f) db = MIC_HNR_MAX_DB;
  if (db > MIC_HNR_MAX_DB) db = MIC_HNR_MAX_DB;
  if (db < -MIC_HNR_MAX_DB) db = -MIC_HNR_MAX_DB;
  return db;
}

AcousticFeatures sustainedRich(Rig& rig, std::vector<float>& buf, const RichTone& t, int midi,
                               AcousticContext& ctxOut) {
  AcousticFeatures f;
  for (int i = 0; i < 2 * MIC_SPECTRAL_DECIMATION + 1; i++) {
    fillRich(buf.data(), buf.size(), t, (size_t)i * buf.size());
    // Comme drainI2S() : le flux traverse la chaine avant d'etre analyse. Les
    // huit frames precedentes etablissent la memoire du filtre.
    rig.stream(buf.data(), buf.size());
    f = rig.analyse(buf.data(), buf.size(), midi);
    ctxOut = rig.context(buf.data(), buf.size(), midi);
  }
  return f;
}

// Note TRES TIMBREE de reference : fondamentale moderee, H2..H8 fortes, souffle
// minime. C'est le cas que l'approximation Goertzel punit, puisque H5..H8
// tombent hors de ses quatre raies et sont donc comptees comme du bruit.
RichTone timbredReference(float f0) {
  RichTone t;
  t.f0 = f0;
  t.partial[0] = 0.20f; t.partial[1] = 0.28f; t.partial[2] = 0.30f;
  t.partial[3] = 0.30f; t.partial[4] = 0.32f; t.partial[5] = 0.30f;
  t.partial[6] = 0.26f; t.partial[7] = 0.22f;
  t.noise = 0.02f;
  return t;
}

// Plage reelle prise par le HNR spectral sur les generateurs deterministes.
// Le tableau est IMPRIME : c'est lui qui justifie les trois seuils, et il doit
// rester lisible dans la sortie de la suite pour qu'on puisse le comparer, plus
// tard, a un releve sur microphone.
void quality_hnr_scale_is_the_one_the_thresholds_describe() {
  std::vector<float> buf(kFrame);
  std::vector<float> raw(kFrame);

  printf("  [hnr-calib] plage du HNR SPECTRAL sur la CHAINE DE PRODUCTION - PCM\n"
         "  [hnr-calib] synthetique passe dans AudioFilterChain comme le fait\n"
         "  [hnr-calib] drainI2S() (fluteLike 440 Hz, amp 0,40 ; %d frames, la\n"
         "  [hnr-calib] derniere avec FFT ; memoire de filtre etablie).\n",
         kFramesEndingOnFft);
  printf("  [hnr-calib] %-26s %9s %9s %9s %9s %9s\n",
         "signal", "HNR (dB)", "PCM brut", "breath", "compo_br", "compo_q");

  // Sinus pur : rien a mesurer comme bruit, la mesure est bornee.
  {
    Rig rig;
    rig.useProductionChain();
    AcousticFeatures f;
    AcousticContext ctx;
    for (int i = 0; i < kFramesEndingOnFft; i++) {
      audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.4f, kFs, (size_t)i * kFrame);
      rig.stream(buf.data(), kFrame);
      f = rig.analyse(buf.data(), kFrame, kMidiA4);
      ctx = rig.context(buf.data(), kFrame, kMidiA4);
    }
    assert(f.hnrIsSpectral);
    assert(f.harmonicToNoiseRatio == MIC_HNR_MAX_DB);
    printf("  [hnr-calib] %-26s %9.2f %9s %9s %9.3f %9.3f\n", "sinus pur",
           (double)f.harmonicToNoiseRatio, "-", "-",
           (double)breathHnrComponent(f.harmonicToNoiseRatio),
           (double)qualityHnrComponent(f.harmonicToNoiseRatio));
  }

  // Balayage du souffle sur une note harmonique. Le point a 0,350 remplace
  // l'ancien haut de balayage a 0,200 : sur la chaine de production le pitch
  // survit plus loin - le filtrage aide aussi YIN - et la calibration doit
  // aller jusqu'ou la mesure existe encore.
  const float kSweep[] = {0.0f, 0.010f, 0.020f, 0.050f, 0.100f, 0.200f, 0.350f};
  constexpr size_t kPoints = sizeof(kSweep) / sizeof(kSweep[0]);
  float measured[kPoints] = {};
  float rawMeasured[kPoints] = {};
  float worstChainGap = 0.0f;
  for (size_t i = 0; i < kPoints; i++) {
    Rig rig;
    AcousticContext ctx;
    const AcousticFeatures f =
        sustainedProductionNote(rig, buf, 440.0f, kSweep[i], kMidiA4, ctx);
    // Chaque point DOIT porter la mesure spectrale : un seuil calibre sur une
    // echelle ne se verifie pas sur des points mesures sur l'autre.
    assert(f.pitchValid && f.spectralValid && f.hnrIsSpectral);
    measured[i] = f.harmonicToNoiseRatio;

    // LE MEME signal avec et sans la chaine, mesure directement par
    // l'analyseur a f0 imposee. Les deux colonnes doivent rester proches :
    // c'est la preuve que le plancher n'est plus extrapole a une bande que le
    // firmware a videe. Avant correction l'ecart atteignait +12,5 dB ici.
    // La mesure est prise a f0 imposee, et non a travers le banc complet,
    // parce que le detecteur de pitch ne survit pas aux memes niveaux de
    // souffle selon que le flux a ete filtre ou non : on comparerait alors deux
    // notes differentes.
    const int kLastFrame = kFramesEndingOnFft - 1;
    productionFrame(buf, 440.0f, 0.4f, kSweep[i], kLastFrame);
    const float prodDirect = directHnrDb(buf, 440.0f);
    audiosig::fluteLike(raw.data(), kFrame, 440.0f, 0.4f, kSweep[i], kFs,
                        (size_t)kLastFrame * kFrame);
    rawMeasured[i] = directHnrDb(raw, 440.0f);
    const float gap = fabsf(prodDirect - rawMeasured[i]);
    if (gap > worstChainGap) worstChainGap = gap;

    const BreathinessResult b =
        AcousticQuality::computeBreathiness(f, ctx, AcousticQuality::breathinessAnchorHz(f, ctx));
    char label[32];
    snprintf(label, sizeof(label), "fluteLike souffle=%.3f", (double)kSweep[i]);
    printf("  [hnr-calib] %-26s %9.2f %9.2f %9.3f %9.3f %9.3f\n", label, (double)measured[i],
           (double)rawMeasured[i], (double)b.value, (double)breathHnrComponent(measured[i]),
           (double)qualityHnrComponent(measured[i]));
  }

  // Bruit blanc seul : la chaine REFUSE de rendre un HNR (pas de pitch, donc
  // pas de fondamentale fiable). La plage basse se releve donc directement sur
  // l'analyseur, en lui imposant la frequence de la note VISEE. Plusieurs
  // graines, parce que sur du bruit pur le resultat DEPEND de la realisation :
  // c'est une statistique, pas une constante.
  float noiseOnlyDb = -MIC_HNR_MAX_DB;   // le moins negatif des releves
  {
    SpectralAnalyzer sa;
    for (uint32_t seed : {99u, 12345u, 7u}) {
      productionNoiseFrame(buf, 0.4f, seed, kFramesEndingOnFft);
      assert(sa.computeSpectrum(buf.data(), kFrame));
      const HarmonicNoiseRatio h = sa.harmonicNoiseRatio(440.0f, kFs);
      assert(h.valid);
      if (h.db > noiseOnlyDb) noiseOnlyDb = h.db;
      char label[32];
      snprintf(label, sizeof(label), "bruit blanc (graine %u)", seed);
      printf("  [hnr-calib] %-26s %9.2f %9s %9s %9.3f %9.3f\n", label, (double)h.db, "-", "-",
             (double)breathHnrComponent(h.db), (double)qualityHnrComponent(h.db));
      assert(h.db >= -MIC_HNR_MAX_DB);
    }

    // Et la chaine complete, elle, ne rend RIEN plutot qu'un chiffre plausible.
    Rig rig;
    rig.useProductionChain();
    productionNoiseFrame(buf, 0.4f, 99u, 1);
    const AcousticFeatures f = rig.analyse(buf.data(), kFrame, kMidiA4);
    assert(!f.spectralValid && !f.hnrIsSpectral && f.harmonicToNoiseRatio == 0.0f);
  }
  printf("  [hnr-calib] ecart max production / PCM brut sur le balayage : %.2f dB\n",
         (double)worstChainGap);
  fflush(stdout);   // le tableau doit survivre a l'echec d'une assertion ci-dessous

  // --- Ce que ce tableau impose aux trois seuils ---------------------------

  // LE POINT QUI EMPECHE LA RECIDIVE. Le filtrage de production ne doit presque
  // pas deplacer la mesure : il retire du bruit hors bande, que le HNR ne
  // compte plus d'aucun cote du rapport. Cette assertion echoue si le plancher
  // redevient estime puis etendu a tout le spectre - l'ecart repasse alors a
  // plus de 12 dB, et les seuils ci-dessous decrivent de nouveau un signal
  // imaginaire. Verifiee par mutation.
  assert(worstChainGap < 2.0f);

  // Le balayage decroit : la mesure repond au souffle. Les DEUX premiers points
  // sont a egalite, et ce n'est pas un defaut a masquer : sur la chaine de
  // production un souffle de 0,010 - 2,5 % de la fondamentale - donne le meme
  // +40,00 dB qu'une note sans souffle, parce que la mesure BUTE sur
  // MIC_HNR_MAX_DB. Le haut de l'echelle est ecrase contre cette borne, et
  // c'est ce qui dicte le choix de AQ_BREATH_HNR_TONE_DB.
  assert(measured[0] == MIC_HNR_MAX_DB);
  assert(measured[1] == MIC_HNR_MAX_DB);
  for (size_t i = 2; i < kPoints; i++) assert(measured[i] < measured[i - 1]);

  // AQ_BREATH_HNR_TONE_DB doit separer "aucun souffle" de "souffle mesurable".
  assert(AQ_BREATH_HNR_TONE_DB < MIC_HNR_MAX_DB);
  assert(measured[0] > AQ_BREATH_HNR_TONE_DB);          // souffle 0
  // Le premier souffle que la mesure separe de la borne est 0,020 : il doit
  // tomber sous le seuil, sinon toute la plage 0..0,020 est declaree parfaite.
  assert(measured[2] < AQ_BREATH_HNR_TONE_DB);
  // ...et le seuil doit rester a au moins 4 dB de la borne - l'etendue relevee
  // sur six realisations de bruit - sinon c'est la realisation du bruit, et non
  // le souffle, qui deciderait du verdict.
  assert(AQ_BREATH_HNR_TONE_DB <= MIC_HNR_MAX_DB - 4.0f);
  // ... et BIEN AU-DESSUS de l'ancien seuil de 20 dB, sans quoi toute la moitie
  // superieure de la plage serait ecrasee a "parfait".
  assert(AQ_BREATH_HNR_TONE_DB > 30.0f);

  // LE CAS DE L'AUDIT, verrouille : une note dont le souffle vaut 12,5 % de la
  // fondamentale (souffle 0,050 sur amp 0,40) ne doit NI saturer la composante
  // de qualite a 1,00, NI saturer celle de respiration a 0. C'est exactement ce
  // qu'elle faisait avec le plancher etendu a tout le spectre, seuil a 20 dB
  // comme a 34 dB.
  assert(measured[3] < AQ_BREATH_HNR_TONE_DB - 4.0f);
  assert(qualityHnrComponent(measured[3]) < 0.90f);
  assert(breathHnrComponent(measured[3]) > 0.10f);

  // AQ_BREATH_HNR_NOISE_DB est sous la note la plus soufflee dont le pitch
  // survive (souffle 0,350) et au-dessus du bruit blanc pur : la composante
  // n'est donc ni saturee ni inatteignable sur la plage utile.
  assert(AQ_BREATH_HNR_NOISE_DB < measured[kPoints - 1]);
  assert(AQ_BREATH_HNR_NOISE_DB > noiseOnlyDb);
  // BORNE ELARGIE DE -10 A -5 dB, et il faut dire pourquoi. Sur du bruit pur le
  // HNR est une statistique : la somme des bins de raies fluctue autour du
  // plancher et laisse un residu. Le mesurer dans la seule bande passante
  // reduit l'echantillon - 111 bins au lieu de 256 - donc augmente ce residu.
  // Releve sur 2800 tirages : la mediane reste a la borne -40 dB et 82 % des
  // tirages sont sous -10 dB, mais la queue atteint -0,75 dB avant correction
  // et +1,34 dB apres. La marge entre le bruit pur et le seuil 0 dB s'est donc
  // reduite, sur un signal que la chaine complete refuse de toute facon faute
  // de pitch (verifie juste au-dessus). La distribution complete est verrouillee
  // par hnr_pure_tone_and_pure_noise_sit_at_the_extremes.
  assert(noiseOnlyDb < -5.0f);   // le PLUS FAVORABLE des trois releves

  // AQ_QUALITY_HNR_GOOD_DB : la meme plage, donc le meme repere haut.
  assert(AQ_QUALITY_HNR_GOOD_DB > AQ_QUALITY_HNR_MIN_DB);
  assert(nearly(AQ_QUALITY_HNR_GOOD_DB, AQ_BREATH_HNR_TONE_DB, 1e-6f));
}

// La monotonie, sur la plage ou les TROIS composantes sont disponibles, et avec
// assez de points pour qu'un accident ne passe pas : six, pas deux.
//
// Ce test echoue avec les anciens seuils. Avec TONE = 20 dB, les points a
// souffle 0,010 (HNR 34,40 dB) et 0,050 (HNR 20,55 dB) donnent tous deux une
// composante HNR nulle : la moitie du poids de la mesure ne bouge plus, et
// l'ecart de respiration entre une note presque propre et une note franchement
// soufflee tombe de 0,325 a 0,12. Verifie par mutation.
void quality_breathiness_rises_with_breath_on_the_spectral_scale() {
  std::vector<float> buf(kFrame);
  // BALAYAGE DEPLACE SUR LA CHAINE DE PRODUCTION, et les points avec lui.
  // Le point 0,010 sort du balayage monotone : sur le signal reellement
  // analyse, il donne exactement le meme +40,00 dB qu'une note sans souffle
  // (mesure bornee par MIC_HNR_MAX_DB) et exactement la meme respiration. Il
  // n'est pas ecarte en silence - il est verifie a part, plus bas, comme une
  // propriete MESUREE de la chaine. Le haut monte en revanche a 0,350, ou le
  // pitch survit encore une fois le flux filtre : le balayage couvre donc
  // strictement plus de terrain qu'avant, pas moins.
  const float kSweep[] = {0.0f, 0.020f, 0.050f, 0.100f, 0.200f, 0.350f};
  constexpr size_t kPoints = sizeof(kSweep) / sizeof(kSweep[0]);
  static_assert(kPoints >= 4, "au moins quatre points de balayage");

  float value[kPoints] = {};
  for (size_t i = 0; i < kPoints; i++) {
    Rig rig;
    AcousticContext ctx;
    const AcousticFeatures f =
        sustainedProductionNote(rig, buf, 440.0f, kSweep[i], kMidiA4, ctx);
    const BreathinessResult b =
        AcousticQuality::computeBreathiness(f, ctx, AcousticQuality::breathinessAnchorHz(f, ctx));
    assert(b.valid);
    // Les trois composantes sont la : c'est bien la mesure complete qui est
    // verifiee, pas le seul inter-partiels qui survit quand la FFT manque.
    assert(b.usedHnr && b.usedInterHarmonic && b.usedFlatness);
    assert(nearly(b.weightUsed, AQ_BREATH_W_HNR + AQ_BREATH_W_INTER + AQ_BREATH_W_FLATNESS,
                  1e-5f));
    value[i] = b.value;
  }

  printf("  [hnr-calib] respiration le long du balayage (production) :");
  for (size_t i = 0; i < kPoints; i++) printf(" %.3f", (double)value[i]);
  printf("\n");

  // Strictement croissant, sans exception.
  for (size_t i = 1; i < kPoints; i++) assert(value[i] > value[i - 1]);

  // Une note propre reste propre, une note franchement soufflee est declaree
  // soufflee : les deux bouts du balayage tombent du bon cote du verdict.
  assert(value[0] < 0.05f);
  assert(value[kPoints - 1] > AQ_BREATHY_MAX);

  // LA PREUVE QUE LE RECALIBRAGE N'EST PAS COSMETIQUE. Avec TONE = 20 dB, les
  // deux points ci-dessous saturent tous les deux la composante HNR a zero et
  // cet ecart tombe sous 0,15.
  assert(value[3] - value[1] > 0.25f);

  // CE QUE LA CHAINE NE SAIT PAS FAIRE, mesure plutot que tu. Un souffle de
  // 0,010 - 2,5 % de la fondamentale - est indiscernable d'une note sans
  // souffle apres filtrage : la mesure bute sur MIC_HNR_MAX_DB, la platitude
  // reste sous AQ_BREATH_FLATNESS_TONE et les creux inter-partiels sous
  // AQ_BREATH_INTER_TONE_DB. Les trois composantes valent zero de chaque cote.
  // Ce n'est pas une regression du recalibrage : c'est la resolution de la
  // chaine, et un balayage qui pretendrait le contraire mentirait.
  {
    Rig clean, faint;
    AcousticContext ctxClean, ctxFaint;
    const AcousticFeatures fc =
        sustainedProductionNote(clean, buf, 440.0f, 0.0f, kMidiA4, ctxClean);
    const AcousticFeatures ff =
        sustainedProductionNote(faint, buf, 440.0f, 0.010f, kMidiA4, ctxFaint);
    assert(fc.hnrIsSpectral && ff.hnrIsSpectral);
    assert(fc.harmonicToNoiseRatio == MIC_HNR_MAX_DB);
    assert(ff.harmonicToNoiseRatio == MIC_HNR_MAX_DB);
    const BreathinessResult bc = AcousticQuality::computeBreathiness(
        fc, ctxClean, AcousticQuality::breathinessAnchorHz(fc, ctxClean));
    const BreathinessResult bf = AcousticQuality::computeBreathiness(
        ff, ctxFaint, AcousticQuality::breathinessAnchorHz(ff, ctxFaint));
    assert(bc.valid && bf.valid);
    assert(bc.value == 0.0f && bf.value == 0.0f);
    printf("  [hnr-calib] resolution de la chaine : souffle 0,010 donne le meme "
           "%+.2f dB et la meme respiration %.3f qu'une note sans souffle\n",
           (double)ff.harmonicToNoiseRatio, (double)bf.value);
  }
}

// Le seuil ne doit jamais etre applique a l'approximation Goertzel : elle lit
// une quinzaine de dB plus bas sur la MEME note propre, et une frame sur
// MIC_SPECTRAL_DECIMATION porte cette valeur-la.
void quality_hnr_component_is_absent_on_the_goertzel_scale() {
  std::vector<float> buf(kFrame);

  // UNE SEULE NOTE TENUE, analysee frame par frame. Le champ
  // harmonicToNoiseRatio bascule d'une echelle a l'autre au rythme de
  // MIC_SPECTRAL_DECIMATION, alors que le SIGNAL, lui, ne change pas. C'est
  // exactement ce qu'un seuil unique ne peut pas absorber.
  const int kMidi = 71;                                // si4
  const RichTone note = timbredReference(PitchMath::midiToHz(kMidi));
  Rig rig;
  float worstSpectral = MIC_HNR_MAX_DB;   // le plus BAS des releves spectraux
  float bestGoertzel = -MIC_HNR_MAX_DB;   // le plus HAUT des releves Goertzel
  int spectralFrames = 0;
  int goertzelFrames = 0;
  for (int i = 0; i < 4 * MIC_SPECTRAL_DECIMATION; i++) {
    fillRich(buf.data(), kFrame, note, (size_t)i * kFrame);
    const AcousticFeatures f = rig.analyse(buf.data(), kFrame, kMidi);
    const AcousticContext ctx = rig.context(buf.data(), kFrame, kMidi);
    assert(f.pitchValid && f.spectralValid);
    assert(f.hnrIsSpectral == f.fftValid);   // vrai UNIQUEMENT quand la FFT a tourne
    const BreathinessResult b =
        AcousticQuality::computeBreathiness(f, ctx, AcousticQuality::breathinessAnchorHz(f, ctx));
    // La composante suit le drapeau, jamais le seul `spectralValid`.
    assert(b.usedHnr == f.hnrIsSpectral);
    const QualityScore q = AcousticQuality::computeAcousticQuality(
        f, ctx, b, AQ_ATTACK_NOT_MEASURED);
    assert(q.harmonicMeasured == f.hnrIsSpectral);
    if (f.hnrIsSpectral) {
      if (f.harmonicToNoiseRatio < worstSpectral) worstSpectral = f.harmonicToNoiseRatio;
      spectralFrames++;
    } else {
      if (f.harmonicToNoiseRatio > bestGoertzel) bestGoertzel = f.harmonicToNoiseRatio;
      goertzelFrames++;
    }
  }
  assert(spectralFrames == 4);
  assert(goertzelFrames == 4 * (MIC_SPECTRAL_DECIMATION - 1));
  printf("  [hnr-calib] MEME note timbree tenue : mesure spectrale >= %+.2f dB, "
         "approximation Goertzel <= %+.2f dB (ecart %.2f dB)\n",
         (double)worstSpectral, (double)bestGoertzel,
         (double)(worstSpectral - bestGoertzel));
  fflush(stdout);
  // Les deux echelles ne se recouvrent meme pas : le pire releve spectral reste
  // tres au-dessus du meilleur releve Goertzel. Un seuil commun placerait donc
  // la meme note des deux cotes du verdict selon la parite de la frame.
  assert(worstSpectral - bestGoertzel > 10.0f);
  // Ce que cela donnerait si on appliquait quand meme les seuils aux deux :
  // la MEME note serait declaree "quasiment du souffle" une frame sur
  // MIC_SPECTRAL_DECIMATION, et "propre" la suivante.
  assert(breathHnrComponent(bestGoertzel) > 0.90f);
  assert(breathHnrComponent(worstSpectral) < 0.15f);

  // Et le refus est net, pas une valeur moyenne : sans le drapeau, la
  // composante disparait et le poids le dit.
  AcousticFeatures f = healthyFeatures();
  f.hnrIsSpectral = false;
  AcousticContext ctx = plainContext();
  const BreathinessResult b = AcousticQuality::computeBreathiness(f, ctx, 440.0f);
  assert(b.valid && !b.usedHnr);
  assert(nearly(b.weightUsed, AQ_BREATH_W_FLATNESS, 1e-5f));
  BreathinessResult given;
  given.valid = true; given.value = 0.0f; given.weightUsed = 1.0f;
  const QualityScore q =
      AcousticQuality::computeAcousticQuality(f, ctx, given, AQ_ATTACK_NOT_MEASURED);
  assert(q.valid && !q.harmonicMeasured);
  assert(nearly(q.weightUsed, 1.0f - QualityWeights().attack - QualityWeights().harmonic, 1e-5f));
}

// LE test de la PHASE 4 corrigee, porte jusqu'a la note de qualite : une note
// timbree doit etre MIEUX notee qu'une note soufflee de meme niveau. Avec
// l'approximation Goertzel, le classement etait INVERSE - ce test le remontre
// sur les memes signaux, puis verifie que la chaine ne le reproduit plus.
void quality_score_ranks_a_timbred_note_above_a_breathy_one() {
  std::vector<float> buf(kFrame);
  const int kMidi = 71;                                 // si4
  const float f0 = PitchMath::midiToHz(kMidi);          // 493,88 Hz

  const RichTone rich = timbredReference(f0);

  // Note SOUFFLEE : fondamentale dominante, presque pas d'harmoniques, beaucoup
  // de bruit large bande. Le souffle reste sous le seuil ou YIN lache, sans
  // quoi la comparaison porterait sur un refus et non sur une note.
  RichTone airy;
  airy.f0 = f0;
  airy.partial[0] = 0.70f; airy.partial[1] = 0.15f; airy.partial[2] = 0.07f;
  airy.noise = 0.30f;

  // CE TEST RESTE SUR DU PCM BRUT, et il faut dire pourquoi, parce que le reste
  // de ce bloc est passe a la chaine de production.
  //
  // Il compare un CLASSEMENT - la note timbree au-dessus de la note soufflee -
  // et ce classement tient sur les deux signaux : mesure sur la chaine de
  // production, la paire donne respiration 0,174 / 0,481 et qualite 0,954 /
  // 0,713, donc le meme verdict relatif, plus net encore sur la qualite.
  //
  // Ce qui NE tient pas sur la chaine de production, c'est l'assertion absolue
  // `bB.value > AQ_BREATHY_MAX` : 0,481 passe sous le seuil de 0,55. La cause
  // n'est PAS le HNR - il vaut +12,58 dB sur cette note, largement du bon cote -
  // mais les deux autres composantes de la respiration : la platitude spectrale
  // est mesuree sur TOUT le spectre alors que la chaine en a vide 56 %, donc
  // elle s'effondre d'un facteur 2,5, et les creux inter-partiels suivent.
  // AQ_BREATH_FLATNESS_TONE/NOISE et AQ_BREATHY_MAX sont donc encore etalonnes
  // en PCM brut - meme defaut que celui corrige ici pour le HNR, mais sur une
  // mesure qui sert aussi a NoiseModel et a WebConfigurator : le corriger
  // demande son propre chantier. Le signaler vaut mieux que deplacer ce test
  // pour qu'il passe, ou deplacer le seuil pour qu'il tombe du bon cote.
  Rig rigT, rigB;
  AcousticContext ctxT, ctxB;
  AcousticFeatures fT = sustainedRich(rigT, buf, rich, kMidi, ctxT);
  const float legacyT = legacyHnrDb(buf.data(), kFrame, f0);
  const float rmsT = sqrtf(SpectralAnalyzer::totalPower(buf.data(), kFrame));
  AcousticFeatures fB = sustainedRich(rigB, buf, airy, kMidi, ctxB);
  const float legacyB = legacyHnrDb(buf.data(), kFrame, f0);
  const float rmsB = sqrtf(SpectralAnalyzer::totalPower(buf.data(), kFrame));

  // Les deux notes sont au meme niveau : la comparaison ne doit rien a une
  // difference d'amplitude.
  const float levelGapDb = 20.0f * log10f(rmsT / rmsB);
  assert(fabsf(levelGapDb) < 1.0f);

  // Les deux portent bien la MESURE spectrale, sinon on comparerait deux
  // echelles.
  assert(fT.pitchValid && fT.hnrIsSpectral);
  assert(fB.pitchValid && fB.hnrIsSpectral);

  // Le SNR est fourni a l'identique : il ne doit pas expliquer l'ecart.
  fT.snrValid = true; fT.snrDb = 30.0f;
  fB.snrValid = true; fB.snrDb = 30.0f;

  const BreathinessResult bT =
      AcousticQuality::computeBreathiness(fT, ctxT, AcousticQuality::breathinessAnchorHz(fT, ctxT));
  const BreathinessResult bB =
      AcousticQuality::computeBreathiness(fB, ctxB, AcousticQuality::breathinessAnchorHz(fB, ctxB));
  const QualityScore qT =
      AcousticQuality::computeAcousticQuality(fT, ctxT, bT, AQ_ATTACK_NOT_MEASURED);
  const QualityScore qB =
      AcousticQuality::computeAcousticQuality(fB, ctxB, bB, AQ_ATTACK_NOT_MEASURED);

  printf("  [hnr-calib] timbree %.2f dBFS / soufflee %.2f dBFS (ecart %.2f dB)\n",
         (double)(20.0f * log10f(rmsT)), (double)(20.0f * log10f(rmsB)), (double)levelGapDb);
  printf("  [hnr-calib] HNR    : spectral %+6.2f / %+6.2f dB   Goertzel %+6.2f / %+6.2f dB\n",
         (double)fT.harmonicToNoiseRatio, (double)fB.harmonicToNoiseRatio,
         (double)legacyT, (double)legacyB);
  printf("  [hnr-calib] verdict: respiration %.3f / %.3f   qualite %.4f / %.4f\n",
         (double)bT.value, (double)bB.value, (double)qT.score, (double)qB.score);
  fflush(stdout);

  // L'ANCIENNE approximation classait la note soufflee AU-DESSUS de la timbree.
  assert(legacyT < legacyB);

  // La chaine actuelle les classe dans le bon sens, et largement.
  assert(fT.harmonicToNoiseRatio > fB.harmonicToNoiseRatio + 15.0f);
  assert(bT.valid && bB.valid && bT.usedHnr && bB.usedHnr);
  // La note timbree reste franchement du bon cote du verdict, la soufflee du
  // mauvais, et l'ecart entre les deux est large - ce n'est pas un classement
  // arrache a la troisieme decimale.
  assert(bT.value < 0.5f * AQ_BREATHY_MAX);
  assert(bB.value > AQ_BREATHY_MAX);
  assert(bB.value - bT.value > 0.40f);
  assert(qT.valid && qB.valid && qT.harmonicMeasured && qB.harmonicMeasured);
  assert(nearly(qT.weightUsed, qB.weightUsed, 1e-5f));   // meme base de comparaison
  assert(qT.score > qB.score + 0.20f);

  // L'INVERSION, portee jusqu'a la composante de qualite : alimentee avec les
  // valeurs Goertzel, la composante harmonique note la note SOUFFLEE au-dessus
  // de la timbree. Cette inversion-la ne depend d'aucun seuil - c'est la mesure
  // elle-meme qui est fausse - et c'est pour cela que la brancher etait le
  // prealable au recalibrage. La preuve que les seuils, eux, ont bouge est
  // dans quality_breathiness_rises_with_breath_on_the_spectral_scale().
  assert(qualityHnrComponent(legacyT) < qualityHnrComponent(legacyB));
  assert(qualityHnrComponent(fT.harmonicToNoiseRatio) >
         qualityHnrComponent(fB.harmonicToNoiseRatio) + 0.40f);
  assert(breathHnrComponent(fT.harmonicToNoiseRatio) <
         breathHnrComponent(fB.harmonicToNoiseRatio) - 0.40f);
}

#else   // !MIC_FFT_ENABLED

// Sans FFT, AcousticFeatures::hnrIsSpectral ne devient jamais vrai : la
// composante HNR de la respiration et celle de la note de qualite sont donc
// absentes de TOUTES les frames. Ce n'est pas un defaut a corriger ici, c'est
// la consequence assumee d'un binaire sans spectre - mais il faut le verifier,
// sans quoi le repli Goertzel pourrait se glisser sous des seuils qui ne le
// decrivent pas.
void quality_hnr_is_absent_without_fft() {
  std::vector<float> buf(kFrame);
  Rig rig;
  for (int i = 0; i < 4; i++) {
    audiosig::fluteLike(buf.data(), kFrame, 440.0f, 0.4f, 0.01f, kFs, (size_t)i * kFrame);
    const AcousticFeatures f = rig.analyse(buf.data(), kFrame, kMidiA4);
    const AcousticContext ctx = rig.context(buf.data(), kFrame, kMidiA4);
    // Goertzel a tourne - le champ porte donc une valeur - mais elle n'est pas
    // sur l'echelle des seuils, et le drapeau le dit.
    assert(f.spectralValid && !f.fftValid && !f.hnrIsSpectral);
    const BreathinessResult b =
        AcousticQuality::computeBreathiness(f, ctx, AcousticQuality::breathinessAnchorHz(f, ctx));
    assert(!b.usedHnr);
    const QualityScore q =
        AcousticQuality::computeAcousticQuality(f, ctx, b, AQ_ATTACK_NOT_MEASURED);
    assert(!q.harmonicMeasured);
  }
  printf("  [hnr-calib] MIC_FFT_ENABLED = 0 : mesure spectrale absente du binaire, "
         "composante HNR retiree des deux notes\n");
}

#endif  // MIC_FFT_ENABLED

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
#if MIC_FFT_ENABLED
  quality_hnr_scale_is_the_one_the_thresholds_describe();
  quality_breathiness_rises_with_breath_on_the_spectral_scale();
  quality_hnr_component_is_absent_on_the_goertzel_scale();
  quality_score_ranks_a_timbred_note_above_a_breathy_one();
#else
  quality_hnr_is_absent_without_fft();
#endif
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
