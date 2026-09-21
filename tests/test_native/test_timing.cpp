/***********************************************************************************************
 * test_timing.cpp - Tests natifs de l'analyse temporelle (PHASE 7)
 *
 * Deux familles de tests, volontairement separees :
 *
 *   - FLUX AUDIO REEL. Des frames construites a partir des generateurs de
 *     audio_signals.h, modulees par une enveloppe, dont le niveau est mesure
 *     par AudioLevel::compute exactement comme en production. Ce sont ces
 *     tests-la qui disent la PRECISION reellement atteinte, parce qu'ils
 *     subissent l'etalement de la fenetre de 32 ms comme la vraie chaine.
 *
 *   - FLUX SYNTHETIQUE. Le niveau en dB et le pitch sont imposes directement.
 *     Ce qui est verifie alors est la LOGIQUE de la machine a etats : ordres
 *     incoherents, plafonds, hysteresis, repliement de millis(). Faire passer
 *     ces cas par du vrai signal ne prouverait rien de plus et rendrait
 *     l'intention illisible.
 *
 * RAPPEL DE RESOLUTION : une frame arrive toutes les 16 ms et son niveau est
 * une moyenne sur les 32 ms qui la precedent. Aucun test ici ne pretend a une
 * precision a la milliseconde sur un instant ABSOLU. Ce qui est exige, et
 * mesure, c'est que les ECARTS soient justes et que le biais systematique soit
 * borne et connu.
 ***********************************************************************************************/
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "settings.h"
#include "AudioLevel.h"
#include "PitchDetector.h"
#include "PitchMath.h"
#include "AcousticFeatures.h"
#include "AcousticTiming.h"
#include "audio_signals.h"

namespace {

using audiosig::ToneSpec;
namespace cfg = AcousticTimingCfg;

constexpr size_t kN = (size_t)MIC_ANALYSIS_FRAME_SIZE;   // 1024 echantillons
constexpr float kFs = (float)MIC_SAMPLE_RATE;            // 32 kHz
constexpr uint32_t kHop = cfg::kFramePeriodMs;           // 16 ms
constexpr uint32_t kSpan = cfg::kFrameSpanMs;            // 32 ms

// La grille d'analyse est bien celle annoncee : si le materiel changeait, tous
// les chiffres de ce fichier deviendraient faux en silence.
static_assert(kHop == 16, "hop attendu de 16 ms (512 a 32 kHz)");
static_assert(kSpan == 32, "fenetre attendue de 32 ms (1024 a 32 kHz)");

// ---------------------------------------------------------------------------
// Flux SYNTHETIQUE : on impose le niveau et le pitch
// ---------------------------------------------------------------------------

struct FakeStream {
  AcousticTiming& t;
  uint32_t ts;

  FakeStream(AcousticTiming& timing, uint32_t startMs) : t(timing), ts(startMs) {}

  // Injecte `count` frames identiques, une toutes les kHop ms.
  void feed(float db, int count = 1, bool pitchValid = false, float hz = 0.0f) {
    for (int i = 0; i < count; i++) {
      TimingFrame f;
      f.timestampMs = ts;
      f.rmsDbFS = db;
      f.pitchValid = pitchValid;
      f.pitchHz = hz;
      t.update(f);
      ts += kHop;
    }
  }

  // Descente (ou montee) progressive : `steps` frames reparties de `fromDb` a
  // `toDb`. Aucune chaine reelle ne produit un echelon de niveau - la fenetre
  // de 32 ms l'interdit - donc les tests de relachement ne doivent pas en
  // fabriquer un.
  void ramp(float fromDb, float toDb, int steps) {
    for (int i = 1; i <= steps; i++) {
      feed(fromDb + (toDb - fromDb) * (float)i / (float)steps, 1);
    }
  }

  // Une frame a un instant IMPOSE, pour les cas ou la grille elle-meme est le
  // sujet du test.
  bool at(uint32_t when, float db, bool pitchValid = false, float hz = 0.0f) {
    TimingFrame f;
    f.timestampMs = when;
    f.rmsDbFS = db;
    f.pitchValid = pitchValid;
    f.pitchHz = hz;
    ts = when + kHop;
    return t.update(f);
  }
};

// ---------------------------------------------------------------------------
// Flux AUDIO : enveloppe appliquee sur les generateurs de audio_signals.h
// ---------------------------------------------------------------------------

// Enveloppe d'amplitude d'une note. Les durees sont en ms, dans le temps
// ABSOLU du flux (le meme que les horodatages de frame).
struct NoteEnvelope {
  float f0 = 587.33f;       // D5
  float onsetMs = 0.0f;     // instant PHYSIQUE d'apparition du son
  float riseMs = 20.0f;     // montee lineaire en amplitude
  float sustainAmp = 0.40f;
  float offMs = 1e9f;       // debut PHYSIQUE de l'extinction
  float fallMs = 30.0f;     // extinction lineaire jusqu'a zero
  float noiseAmp = 0.002f;  // plancher permanent (machinerie)
};

float envAmp(const NoteEnvelope& e, double tMs) {
  if (tMs < (double)e.onsetMs) return 0.0f;
  float a = e.sustainAmp;
  if (tMs < (double)(e.onsetMs + e.riseMs)) {
    a = e.sustainAmp * (float)((tMs - (double)e.onsetMs) / (double)e.riseMs);
  }
  if (tMs >= (double)e.offMs) {
    const double k = (tMs - (double)e.offMs) / (double)e.fallMs;
    a = (k >= 1.0) ? 0.0f : e.sustainAmp * (float)(1.0 - k);
  }
  return a;
}

// Construit les kN echantillons qui SE TERMINENT a `tsMs`. C'est la convention
// d'AcousticFeatures::timestamp (instant de l'analyse, donc fin de fenetre), et
// c'est elle qui produit le retard d'etalement que les tests mesurent.
void buildFrameSamples(std::vector<float>& buf, const NoteEnvelope& e, uint32_t tsMs) {
  const long endSample = lround((double)tsMs * (double)kFs / 1000.0);
  const long firstSample = endSample - (long)kN;
  assert(firstSample >= 0);   // le flux doit demarrer assez loin de zero

  static std::vector<float> tone, noise;
  tone.assign(kN, 0.0f);
  noise.assign(kN, 0.0f);

  // Porteuse "flute" a amplitude 1, phase CONTINUE d'une frame a l'autre grace
  // a startSample : c'est ce que garantit audio_signals.h.
  ToneSpec carrier;
  carrier.sampleRate = kFs;
  carrier.f0 = e.f0;
  carrier.amp = 1.0f;
  carrier.h2 = 0.30f;
  carrier.h3 = 0.12f;
  audiosig::fill(tone.data(), kN, carrier, (size_t)firstSample);

  // Plancher genere separement : il ne doit PAS etre module par l'enveloppe,
  // sinon le bruit de machinerie disparaitrait pendant les silences et le
  // plancher d'avant-note vaudrait zero, ce qui n'arrive jamais en vrai.
  ToneSpec floorNoise;
  floorNoise.sampleRate = kFs;
  floorNoise.noise = e.noiseAmp;
  floorNoise.seed = 4242u;
  audiosig::fill(noise.data(), kN, floorNoise, (size_t)firstSample);

  buf.assign(kN, 0.0f);
  for (size_t i = 0; i < kN; i++) {
    const double tMs = 1000.0 * (double)(firstSample + (long)i) / (double)kFs;
    buf[i] = envAmp(e, tMs) * tone[i] + noise[i];
  }
}

// Scenario complet : un flux de frames et les ordres qui le traversent.
struct Scenario {
  NoteEnvelope env;
  uint32_t startMs = 20000;     // debut du flux (assez loin de zero)
  uint32_t commandMs = 20160;   // ordre MIDI
  uint32_t airMs = 20170;       // consigne d'air (0 = jamais donnee)
  uint32_t valveMs = 20180;     // ouverture de valve (0 = jamais)
  uint32_t releaseMs = 0;       // ordre d'arret (0 = jamais)
  uint32_t durationMs = 900;
  bool interpolate = true;
  bool withPitch = false;       // fait tourner YIN sur chaque frame
};

void runScenario(AcousticTiming& timing, const Scenario& s) {
  timing.reset();
  timing.setEnvelopeInterpolation(s.interpolate);

  PitchDetector det;
  std::vector<float> buf;
  bool sentCmd = false, sentAir = false, sentValve = false, sentRel = false;

  for (uint32_t ts = s.startMs; ts <= s.startMs + s.durationMs; ts += kHop) {
    // Les ordres tombent entre deux frames : ils sont delivres des que la
    // frame courante les depasse, avec leur PROPRE horodatage.
    if (!sentCmd && ts >= s.commandMs) { timing.noteCommanded(s.commandMs); sentCmd = true; }
    if (!sentAir && s.airMs != 0 && ts >= s.airMs) { timing.airCommanded(s.airMs); sentAir = true; }
    if (!sentValve && s.valveMs != 0 && ts >= s.valveMs) { timing.valveOpened(s.valveMs); sentValve = true; }
    if (!sentRel && s.releaseMs != 0 && ts >= s.releaseMs) { timing.noteReleased(s.releaseMs); sentRel = true; }

    buildFrameSamples(buf, s.env, ts);
    const FrameLevel lvl = AudioLevel::compute(buf.data(), kN);

    TimingFrame f;
    f.timestampMs = ts;
    f.rmsDbFS = lvl.rmsDbFS;
    if (s.withPitch) {
      const PitchResult p = det.detect(buf.data(), kN);
      f.pitchValid = p.valid;
      f.pitchHz = p.hz;
    }
    timing.update(f);
  }
}

// Le releve a lire apres un scenario : le dernier TERMINE s'il y en a un,
// sinon celui en cours.
const NoteTiming& result(const AcousticTiming& t) {
  return t.hasLast() ? t.last() : t.current();
}

// ===========================================================================
// 1 - Latence connue : ordre MIDI -> son audible
// ===========================================================================
//
// Le biais d'etalement (fenetre de 32 ms) rend la valeur ABSOLUE difficile a
// predire. Ce qui se predit exactement, en revanche, c'est la VARIATION : si
// le son physique apparait 100 ms plus tard, la latence mesuree doit augmenter
// de 100 ms. Une mesure differentielle annule le biais et permet une tolerance
// serree - c'est elle qui a du mordant, pas une borne large sur l'absolu.

void timing_measures_a_known_command_latency() {
  AcousticTiming t;

  const uint32_t kCmd = 20160;
  uint32_t measured[3] = {0, 0, 0};
  const float kDelaysMs[3] = {60.0f, 160.0f, 260.0f};

  for (int i = 0; i < 3; i++) {
    Scenario s;
    s.env.onsetMs = (float)kCmd + kDelaysMs[i];
    s.commandMs = kCmd;
    runScenario(t, s);
    const NoteTiming& r = result(t);
    assert(r.hasSoundOnset);
    assert(r.commandToSoundLatency.valid);
    measured[i] = r.commandToSoundLatency.ms;
  }

  // Les ecarts mesures suivent les ecarts commandes a moins d'une frame pres.
  const int32_t d1 = (int32_t)measured[1] - (int32_t)measured[0];
  const int32_t d2 = (int32_t)measured[2] - (int32_t)measured[1];
  assert(labs((long)d1 - 100) <= (long)kHop);
  assert(labs((long)d2 - 100) <= (long)kHop);

  // Et l'ABSOLU est juste, lui aussi : l'erreur reste sous une demi-periode de
  // frame. C'est le resultat MESURE (biais moyen -1 ms, |erreur| <= 5 ms sur le
  // balayage complet du test 11), pas une tolerance de confort - une detection
  // calee sur la grille des frames sortirait de cette bande.
  for (int i = 0; i < 3; i++) {
    assert(labs((long)measured[i] - (long)kDelaysMs[i]) <= (long)kHop / 2);
  }

  // Latence air -> son : la consigne d'air est posee apres l'ordre MIDI, donc
  // cette latence est plus courte, de l'ecart exact entre les deux ordres.
  Scenario s;
  s.env.onsetMs = (float)kCmd + 120.0f;
  s.commandMs = kCmd;
  s.airMs = kCmd + 40;
  runScenario(t, s);
  const NoteTiming& r = result(t);
  assert(r.airToSoundLatency.valid);
  assert(r.commandToSoundLatency.ms - r.airToSoundLatency.ms == 40);
}

// ===========================================================================
// 2 - Attaque connue : apparition -> niveau etabli
// ===========================================================================
//
// Meme principe : une montee quatre fois plus longue doit donner une attaque
// mesuree plus longue du meme supplement. La valeur absolue, elle, contient le
// temps que met la fenetre glissante a "voir" le palier, et ce terme est
// commun aux deux mesures.

void timing_measures_a_known_attack() {
  AcousticTiming t;

  // Quatre montees, de l'echelon parfait a la montee tres lente.
  const float kRises[4] = {0.0f, 20.0f, 80.0f, 200.0f};
  uint32_t attack[4] = {0, 0, 0, 0};

  for (int i = 0; i < 4; i++) {
    Scenario sc;
    sc.env.onsetMs = 20300.0f;
    sc.env.riseMs = kRises[i];
    sc.durationMs = 1100;
    runScenario(t, sc);
    const NoteTiming& r = result(t);
    assert(r.hasSoundOnset && r.hasLevelEstablished);
    assert(r.attackTime.valid);
    attack[i] = r.attackTime.ms;

    // Le niveau etabli suit toujours l'apparition, jamais l'inverse.
    assert((int32_t)(r.levelEstablishedTimestamp - r.soundOnsetTimestamp) > 0);

    // BORNE HAUTE : le palier ne peut pas etre reconnu plus tard que la fin de
    // la montee physique, plus le temps que met la fenetre de 32 ms a s'en
    // degager, plus une frame.
    assert((float)attack[i] <= kRises[i] + (float)(kSpan + kHop));

    // BORNE BASSE : meme pour un ECHELON parfait, l'attaque mesuree ne peut
    // pas valoir zero - la fenetre glissante met une fenetre entiere a voir le
    // palier. Annoncer 0 ms serait inventer une precision qui n'existe pas.
    assert(attack[i] >= kSpan);
  }

  // L'echelon donne le plancher de la mesure : le temps d'etablissement propre
  // a la CHAINE, pas a l'instrument. Mesure : 43 ms, soit une fenetre plus une
  // demi-frame.
  assert(attack[0] <= kSpan + 2u * kHop);

  // Et une montee dix fois plus longue se voit tres largement : facteur 3 au
  // moins sur l'attaque mesuree.
  assert(attack[3] >= 3u * attack[0]);

  // LIMITE ASSUMEE ET MESUREE : le critere de plateau se declenche pendant
  // l'approche asymptotique du palier, donc il SOUS-ESTIME une montee lente.
  // Sur 200 ms de montee il rend 184 ms au lieu de ~232. On le verrouille
  // plutot que de le taire : si ce comportement changeait, ce test le dirait.
  assert((float)attack[3] < kRises[3] + (float)kSpan);
  assert((float)attack[3] > 0.6f * kRises[3]);
}

// ===========================================================================
// 3 - Relachement connu : ordre d'arret -> disparition reelle
// ===========================================================================
//
// L'extinction est lineaire en amplitude sur fallMs. Le seuil de chute vaut
// kReleaseFallDb = 20 dB sous le niveau tenu, soit un dixieme de l'amplitude :
// il est donc franchi a 90 % de fallMs, valeur CALCULABLE. Doubler fallMs doit
// allonger le relachement mesure de 0,9 fois le supplement.

void timing_measures_a_known_release() {
  AcousticTiming t;

  Scenario base;
  base.env.onsetMs = 20300.0f;
  base.env.offMs = 20600.0f;
  base.env.fallMs = 30.0f;
  base.releaseMs = 20600;
  base.durationMs = 900;
  runScenario(t, base);
  const NoteTiming rShort = result(t);
  assert(rShort.outcome == TIMING_COMPLETE);
  assert(rShort.hasSoundRelease && rShort.releaseTime.valid);

  Scenario longer = base;
  longer.env.fallMs = 130.0f;
  runScenario(t, longer);
  const NoteTiming rLong = result(t);
  assert(rLong.outcome == TIMING_COMPLETE);
  assert(rLong.releaseTime.valid);

  const int32_t d = (int32_t)rLong.releaseTime.ms - (int32_t)rShort.releaseTime.ms;
  const long expected = 90;   // 0,9 * (130 - 30)
  assert(labs((long)d - expected) <= (long)kHop);

  // Le franchissement PHYSIQUE du seuil a lieu a 0,9 * fallMs apres l'ordre.
  // La mesure ne peut qu'etre en retard - le critere exige que la fenetre de
  // 32 ms soit passee sous le seuil - et ce retard est borne. Mesure : +23 ms
  // pour fall = 30, +19 ms pour fall = 130. C'est un BIAIS connu, pas du bruit.
  for (int i = 0; i < 2; i++) {
    const NoteTiming& r = (i == 0) ? rShort : rLong;
    const long theoretical = (i == 0) ? 27 : 117;   // 0,9 * fallMs
    assert((long)r.releaseTime.ms > theoretical);
    assert((long)r.releaseTime.ms <= theoretical + (long)(kSpan + kHop));
  }
}

// ===========================================================================
// 4 - Stabilisation du pitch connue
// ===========================================================================
//
// Flux synthetique : on impose exactement quand le pitch devient fiable, puis
// quand il cesse de deriver. Le critere exige kPitchStableFrames frames
// consecutives dans une fourchette de kPitchStableCents, et l'instant retenu
// est le DEBUT de cette fenetre.

void timing_measures_pitch_stabilisation() {
  AcousticTiming t;
  FakeStream s(t, 30000);

  s.feed(-60.0f, 10);                 // plancher : alimente la ligne de base
  const uint32_t cmd = s.ts;
  t.noteCommanded(cmd);

  // Apparition franche : deux frames au-dessus du seuil (-60 + 12 = -48).
  s.feed(-20.0f, 2);
  const NoteTiming& cur = t.current();
  assert(cur.hasSoundOnset);
  const uint32_t onset = cur.soundOnsetTimestamp;

  // Trois frames sonnantes SANS pitch fiable : rien ne doit etre date.
  s.feed(-20.0f, 3, false, 0.0f);
  assert(!t.current().hasPitchDetected);

  // Premier pitch fiable. Il derive encore : 587 -> 620 Hz, soit bien plus que
  // kPitchStableCents, donc pas de stabilite.
  const uint32_t firstPitchTs = s.ts;
  s.feed(-20.0f, 1, true, 587.0f);
  assert(t.current().hasPitchDetected);
  assert(t.current().pitchDetectedTimestamp == firstPitchTs);
  s.feed(-20.0f, 1, true, 605.0f);
  s.feed(-20.0f, 1, true, 620.0f);
  assert(!t.current().hasPitchStable);

  // Puis la note se pose : quatre frames a moins de 25 cents d'ecart.
  const uint32_t stableStart = s.ts;
  s.feed(-20.0f, 1, true, 587.0f);
  s.feed(-20.0f, 1, true, 587.5f);
  s.feed(-20.0f, 1, true, 588.0f);
  assert(!t.current().hasPitchStable);     // la fenetre n'est pas encore pleine
  s.feed(-20.0f, 1, true, 587.2f);
  assert(t.current().hasPitchStable);

  // L'instant retenu est le DEBUT de la fenetre stable, pas sa fin.
  assert(t.current().pitchStableTimestamp == stableStart);
  assert(t.current().pitchStabilizationTime.valid);
  assert(t.current().pitchStabilizationTime.ms == (uint32_t)(stableStart - onset));

  // Plancher incompressible et EXPLICITE : le critere demande
  // kPitchStableFrames frames CONSECUTIVES, donc la stabilite ne peut jamais
  // etre datee avant le premier pitch fiable, et la frame qui la CONSTATE
  // arrive (kPitchStableFrames - 1) periodes plus tard.
  assert((int32_t)(t.current().pitchStableTimestamp - firstPitchTs) >= 0);
  assert(stableStart - firstPitchTs == 3u * kHop);   // les trois frames qui derivent
  assert(s.ts - kHop == stableStart + (cfg::kPitchStableFrames - 1u) * kHop);
}

// ===========================================================================
// 5 - Ce qui n'est pas mesure n'est PAS invente
// ===========================================================================

void timing_never_invents_a_measure() {
  // (a) Une note qui n'apparait jamais : plafond d'attente atteint.
  {
    AcousticTiming t;
    FakeStream s(t, 40000);
    s.feed(-60.0f, 10);
    t.noteCommanded(s.ts);
    // Le plancher continue, rien ne sonne, au-dela du plafond.
    s.feed(-60.0f, (int)(cfg::kOnsetTimeoutMs / kHop) + 4);

    assert(t.state() == TIMING_IDLE);
    assert(t.hasLast());
    const NoteTiming& r = t.last();
    assert(r.outcome == TIMING_NO_SOUND);
    assert(!r.hasSoundOnset);
    // AUCUNE mesure n'est renseignee. Un zero se lirait comme une latence
    // nulle : c'est exactement la valeur a ne jamais produire.
    assert(!r.commandToSoundLatency.valid);
    assert(!r.airToSoundLatency.valid);
    assert(!r.attackTime.valid);
    assert(!r.pitchStabilizationTime.valid);
    assert(!r.releaseTime.valid);
  }

  // (b) Aucune consigne d'air declaree : la latence air -> son est IMPOSSIBLE,
  //     alors que la latence ordre -> son, elle, est mesuree.
  {
    AcousticTiming t;
    Scenario s;
    s.env.onsetMs = 20300.0f;
    s.airMs = 0;                 // jamais donnee
    s.valveMs = 0;
    runScenario(t, s);
    const NoteTiming& r = result(t);
    assert(r.commandToSoundLatency.valid);
    assert(!r.airToSoundLatency.valid);
    assert(r.airToSoundLatency.ms == 0);       // la valeur existe mais...
    assert(!r.hasAirCommand);                  // ...rien ne permet de la lire
  }

  // (c) Ordre d'arret avant que le son apparaisse : il n'y a pas de son a
  //     faire disparaitre, donc pas de temps de relachement.
  {
    AcousticTiming t;
    FakeStream s(t, 50000);
    s.feed(-60.0f, 10);
    t.noteCommanded(s.ts);
    s.feed(-60.0f, 3);
    assert(t.noteReleased(s.ts));
    const NoteTiming& r = t.last();
    assert(r.outcome == TIMING_NO_SOUND);
    assert(r.hasReleaseCommand);
    assert(!r.hasSoundRelease);
    assert(!r.releaseTime.valid);
  }

  // (d) La note s'est eteinte TOUTE SEULE avant l'ordre d'arret. La disparition
  //     du son, elle, est bien mesuree - c'est un instant reel. Mais l'ecart
  //     entre l'ordre et cette disparition est NEGATIF : ce n'est pas une
  //     latence de relachement, et le ramener a zero le ferait passer pour un
  //     relachement instantane, c'est-a-dire parfait.
  {
    AcousticTiming t;
    FakeStream s(t, 55000);
    s.feed(-60.0f, 10);
    t.noteCommanded(s.ts);
    s.feed(-20.0f, 10);
    assert(t.state() == TIMING_SUSTAIN);
    const uint32_t rel = s.ts;
    assert(t.noteReleased(rel));
    // Seuil de chute a -40 ; le niveau est deja tres en dessous a la premiere
    // frame qui suit l'ordre.
    s.feed(-70.0f, 2);
    const NoteTiming& r = t.last();
    assert(r.hasSoundRelease);                            // l'instant existe
    assert((int32_t)(r.soundReleaseTimestamp - rel) < 0); // mais il precede l'ordre
    assert(!r.releaseTime.valid);                         // donc aucune latence
    assert(r.releaseTime.ms == 0);
  }
}

// ===========================================================================
// 6 - Note coupee en pleine attaque
// ===========================================================================

void timing_note_cut_during_attack() {
  AcousticTiming t;
  FakeStream s(t, 60000);

  s.feed(-60.0f, 10);
  t.noteCommanded(s.ts);

  // Montee franche mais qui n'a pas le temps de faire de palier : le niveau
  // grimpe de 6 dB par frame, l'ordre d'arret tombe avant.
  s.feed(-45.0f, 1);
  s.feed(-39.0f, 1);
  assert(t.state() == TIMING_ATTACK);
  s.feed(-33.0f, 1);
  s.feed(-27.0f, 1);
  assert(t.state() == TIMING_ATTACK);        // toujours pas de palier

  const uint32_t rel = s.ts;
  assert(t.noteReleased(rel));
  assert(t.state() == TIMING_RELEASING);

  // Reference de chute = maximum atteint (-27), donc seuil a -47. L'extinction
  // est PROGRESSIVE : une vraie chaine ne peut pas produire un echelon de
  // niveau, la fenetre de 32 ms l'interdit.
  s.feed(-32.0f, 1);
  s.feed(-38.0f, 1);
  s.feed(-44.0f, 1);
  s.feed(-50.0f, 1);   // sous le seuil : comptage a 1
  s.feed(-56.0f, 1);   // comptage a 2 -> relache

  const NoteTiming& r = t.last();
  assert(r.outcome == TIMING_CUT_SHORT);
  assert(r.hasSoundOnset);
  assert(r.commandToSoundLatency.valid);
  // Le niveau n'a JAMAIS ete etabli : l'attaque n'a pas de duree mesurable.
  assert(!r.hasLevelEstablished);
  assert(!r.attackTime.valid);
  // Le relachement, lui, s'est bien mesure : le son a disparu.
  assert(r.hasSoundRelease);
  assert(r.releaseTime.valid);
}

// ===========================================================================
// 7 - Repliement de millis()
// ===========================================================================
//
// Toute la machine travaille en uint32_t, comme millis() sur ESP32. Ce test
// place la note A CHEVAL sur le repliement : les horodatages passent de
// 0xFFFFFF.. a 0x000000.., et les durees doivent rester justes. Une
// comparaison ecrite sans le cast (int32_t)(a - b) donnerait ici des durees
// d'environ 4,29 milliards de millisecondes.

void timing_survives_millis_overflow() {
  AcousticTiming t;

  const uint32_t kWrap = 0xFFFFFFFFu;
  const uint32_t start = kWrap - 400u;      // 400 ms avant le repliement
  FakeStream s(t, start);

  s.feed(-60.0f, 10);                        // plancher
  const uint32_t cmd = s.ts;
  t.noteCommanded(cmd);
  assert(t.airCommanded(cmd + 16u));

  // Le son apparait apres le repliement.
  s.feed(-60.0f, 12);                        // 192 ms de silence
  const uint32_t firstLoud = s.ts;
  s.feed(-20.0f, 6);                         // apparition + tenue

  const NoteTiming& cur = t.current();
  assert(cur.hasSoundOnset);
  assert(cur.commandToSoundLatency.valid);
  // L'onset est place au premier franchissement ; sans interpolation il
  // vaudrait firstLoud. Avec, il tombe entre la frame precedente et celle-ci.
  const uint32_t lat = cur.commandToSoundLatency.ms;
  const uint32_t expected = (uint32_t)(firstLoud - cmd);
  assert(lat <= expected);
  assert(lat + kHop >= expected);
  // Et surtout : la duree reste de l'ordre de la centaine de ms, pas de
  // quatre milliards.
  assert(lat < 1000u);
  assert(cur.airToSoundLatency.valid);
  assert(cur.airToSoundLatency.ms + 16u == lat);

  // Le relachement traverse lui aussi le repliement sans perdre le nord.
  const uint32_t rel = s.ts;
  assert(t.noteReleased(rel));
  s.feed(-30.0f, 1);    // au-dessus du seuil (-40) : le son baisse encore
  s.feed(-45.0f, 1);    // sous le seuil : comptage a 1
  s.feed(-60.0f, 1);    // comptage a 2 -> relache
  const NoteTiming& r = t.last();
  assert(r.releaseTime.valid);
  assert(r.releaseTime.ms < 1000u);

  // Le franchissement du repliement a bien eu lieu pendant la note.
  assert(r.noteCommandTimestamp > r.releaseCommandTimestamp);   // 0xFFFF.. > 0x00..
}

// ===========================================================================
// 8 - Ordres incoherents : refuses et comptes, jamais appliques en douce
// ===========================================================================

void timing_rejects_inconsistent_events() {
  AcousticTiming t;
  FakeStream s(t, 70000);
  s.feed(-60.0f, 10);

  // Arret sans note : refuse.
  assert(!t.noteReleased(s.ts));
  assert(t.rejectedEvents() == 1);
  // Consigne d'air sans note : refusee.
  assert(!t.airCommanded(s.ts));
  assert(!t.valveOpened(s.ts));
  assert(t.rejectedEvents() == 3);
  assert(!t.hasLast());          // rien n'a ete range

  const uint32_t cmd = s.ts;
  t.noteCommanded(cmd);

  // Consigne d'air ANTERIEURE a l'ordre MIDI : refusee. Une cause ne suit pas
  // son effet.
  assert(!t.airCommanded(cmd - 50u));
  assert(t.rejectedEvents() == 4);
  assert(!t.current().hasAirCommand);

  // La bonne, elle, passe. La deuxieme est refusee : c'est la PREMIERE qui a
  // lance l'air.
  assert(t.airCommanded(cmd + 10u));
  assert(!t.airCommanded(cmd + 20u));
  assert(t.current().airCommandTimestamp == cmd + 10u);

  // Une frame dont l'horodatage RECULE est rejetee, et n'altere pas l'etat.
  s.feed(-60.0f, 2);
  const uint32_t before = s.ts;
  TimingFrame back;
  back.timestampMs = before - 200u;
  back.rmsDbFS = -10.0f;                    // assez fort pour declencher
  assert(!t.update(back));
  assert(t.rejectedFrames() == 1);
  assert(!t.current().hasSoundOnset);       // la frame n'a rien declenche
  s.ts = before;

  // Valve ouverte APRES l'apparition du son : refusee, ce n'est pas elle qui
  // a ouvert le passage.
  s.feed(-20.0f, 2);
  assert(t.state() == TIMING_ATTACK);
  assert(!t.valveOpened(s.ts));
  assert(!t.current().hasValveOpen);

  // Une nouvelle note en remplace une autre : l'ancienne est rangee en
  // ABORTED, avec ce qui avait ete mesure, et rien de plus.
  const uint16_t rejectedBefore = t.rejectedEvents();
  t.noteCommanded(s.ts);
  assert(t.rejectedEvents() == rejectedBefore);   // ce n'est PAS un refus
  assert(t.hasLast());
  assert(t.last().outcome == TIMING_ABORTED);
  assert(t.last().hasSoundOnset);
  assert(!t.last().hasLevelEstablished);
  assert(!t.last().attackTime.valid);
}

// ===========================================================================
// 9 - Plafonds : aucune mesure ne reste "en cours" indefiniment
// ===========================================================================

void timing_every_pending_measure_is_capped() {
  // (a) Plafond de relachement : le son ne disparait jamais (valve bloquee).
  {
    AcousticTiming t;
    FakeStream s(t, 80000);
    s.feed(-60.0f, 10);
    t.noteCommanded(s.ts);
    s.feed(-20.0f, 8);                       // apparition puis palier
    assert(t.state() == TIMING_SUSTAIN);
    assert(t.noteReleased(s.ts));
    s.feed(-20.0f, (int)(cfg::kReleaseTimeoutMs / kHop) + 4);   // rien ne baisse

    assert(t.state() == TIMING_IDLE);
    const NoteTiming& r = t.last();
    assert(r.outcome == TIMING_TIMEOUT);     // ABANDON explicite
    assert(!r.hasSoundRelease);
    assert(!r.releaseTime.valid);
    // Ce qui avait ete mesure avant reste valide : abandonner la fin ne
    // rature pas le debut.
    assert(r.commandToSoundLatency.valid);
    assert(r.attackTime.valid);
  }

  // (b) Plafond d'attaque : le niveau ondule et ne fait jamais de palier.
  {
    AcousticTiming t;
    FakeStream s(t, 90000);
    s.feed(-60.0f, 10);
    t.noteCommanded(s.ts);
    const int frames = (int)(cfg::kAttackTimeoutMs / kHop) + 8;
    for (int i = 0; i < frames; i++) s.feed((i % 2) ? -24.0f : -20.0f, 1);

    // La note continue d'etre suivie - son relachement reste mesurable - mais
    // l'attaque n'a pas de duree.
    assert(t.state() == TIMING_SUSTAIN);
    assert(t.current().hasSoundOnset);
    assert(!t.current().hasLevelEstablished);
    assert(!t.current().attackTime.valid);
  }

  // (c) Plafond de stabilisation du pitch : il derive sans fin.
  {
    AcousticTiming t;
    FakeStream s(t, 100000);
    s.feed(-60.0f, 10);
    t.noteCommanded(s.ts);
    const int frames = (int)(cfg::kPitchTimeoutMs / kHop) + 8;
    float hz = 500.0f;
    for (int i = 0; i < frames; i++) {
      s.feed(-20.0f, 1, true, hz);
      hz *= 1.02f;                            // ~34 cents par frame
    }
    assert(t.current().hasPitchDetected);
    assert(!t.current().hasPitchStable);
    assert(!t.current().pitchStabilizationTime.valid);
  }

  // (d) Plafond global : un Note Off perdu ne laisse pas la machine armee.
  {
    AcousticTiming t;
    FakeStream s(t, 110000);
    s.feed(-60.0f, 10);
    t.noteCommanded(s.ts);
    s.feed(-20.0f, (int)(cfg::kNoteMaxDurationMs / kHop) + 8);
    assert(t.state() == TIMING_IDLE);
    assert(t.last().outcome == TIMING_TIMEOUT);
    assert(!t.last().hasReleaseCommand);
    assert(!t.last().releaseTime.valid);
  }
}

// ===========================================================================
// 10 - Hysteresis du relachement : une ondulation ne rearme pas le comptage
// ===========================================================================

void timing_release_hysteresis_ignores_a_ripple() {
  // Reference : niveau tenu a -20 dBFS, seuil de chute a -40, bande morte
  // [-40, -37).
  const float kSustain = -20.0f;
  const float kThreshold = kSustain - cfg::kReleaseFallDb;            // -40
  const float kInsideBand = kThreshold + 0.5f * cfg::kReleaseHysteresisDb;  // -38,5
  const float kClearAbove = kThreshold + 2.0f * cfg::kReleaseHysteresisDb;  // -34

  // (a) Ondulation DANS la bande morte : elle ne remet pas le comptage a zero,
  //     donc le relachement est bien declare.
  {
    AcousticTiming t;
    FakeStream s(t, 120000);
    s.feed(-60.0f, 10);
    t.noteCommanded(s.ts);
    s.feed(kSustain, 10);
    assert(t.state() == TIMING_SUSTAIN);
    t.noteReleased(s.ts);

    s.feed(-30.0f, 1);                 // le son baisse, encore au-dessus du seuil
    s.feed(kThreshold - 1.0f, 1);      // sous le seuil : comptage a 1
    assert(t.state() == TIMING_RELEASING);
    s.feed(kInsideBand, 1);            // ondulation : ni confirmation ni annulation
    assert(t.state() == TIMING_RELEASING);
    s.feed(kThreshold - 1.0f, 1);      // comptage a 2 -> relache
    assert(t.state() == TIMING_IDLE);
    assert(t.last().outcome == TIMING_COMPLETE);
    assert(t.last().releaseTime.valid);
  }

  // (b) Remontee FRANCHE au-dessus de la bande : le comptage est annule, et le
  //     relachement n'est pas declare sur la seule frame basse suivante.
  {
    AcousticTiming t;
    FakeStream s(t, 130000);
    s.feed(-60.0f, 10);
    t.noteCommanded(s.ts);
    s.feed(kSustain, 10);
    t.noteReleased(s.ts);

    s.feed(-30.0f, 1);                 // le son baisse, encore au-dessus du seuil
    s.feed(kThreshold - 1.0f, 1);      // comptage a 1
    s.feed(kClearAbove, 1);            // remontee franche : annulation
    assert(t.state() == TIMING_RELEASING);
    s.feed(kThreshold - 1.0f, 1);      // comptage a 1 seulement
    assert(t.state() == TIMING_RELEASING);
    assert(!t.hasLast());
    s.feed(kThreshold - 1.0f, 1);      // comptage a 2 -> relache maintenant
    assert(t.state() == TIMING_IDLE);
    assert(t.last().releaseTime.valid);
  }
}

// ===========================================================================
// 11 - L'interpolation fait mieux que la grille de 16 ms, et on le MESURE
// ===========================================================================
//
// On balaie l'instant PHYSIQUE d'apparition sur une periode de frame entiere,
// par pas de 2 ms, et on regarde la latence mesuree. Le retard de detection
// (etalement de la fenetre) est commun a tous les points du balayage : il se
// retire en soustrayant le decalage impose. Ce qui RESTE est l'erreur que
// l'interpolation est censee supprimer.
//
// Sans interpolation, ce residu doit dessiner une marche d'escalier d'une
// frame entiere. Avec, il doit s'effondrer.

void sweepOnsetResidual(bool interpolate, long& spread, long& meanLag) {
  AcousticTiming t;
  const uint32_t kCmd = 20160;
  long lo = 0, hi = 0, sum = 0;
  int n = 0;

  for (uint32_t off = 0; off < kHop; off += 2) {
    Scenario s;
    s.commandMs = kCmd;
    s.env.onsetMs = (float)(kCmd + 100u + off);
    s.env.riseMs = 8.0f;              // attaque franche : le seuil est net
    s.interpolate = interpolate;
    runScenario(t, s);
    const NoteTiming& r = result(t);
    assert(r.commandToSoundLatency.valid);

    const long residual = (long)r.commandToSoundLatency.ms - (long)(100u + off);
    if (n == 0) { lo = hi = residual; }
    if (residual < lo) lo = residual;
    if (residual > hi) hi = residual;
    sum += residual;
    n++;
  }
  spread = hi - lo;
  meanLag = sum / n;
}

void timing_interpolation_beats_frame_quantisation() {
  long spreadOn = 0, lagOn = 0, spreadOff = 0, lagOff = 0;
  sweepOnsetResidual(true, spreadOn, lagOn);
  sweepOnsetResidual(false, spreadOff, lagOff);

  // SANS interpolation, l'instant detecte est cale sur la grille : le residu
  // dessine une marche d'escalier de presque une periode de frame entiere, et
  // le retard moyen vaut une bonne demi-frame. Mesure : etendue 14 ms,
  // moyenne +9 ms.
  assert(spreadOff >= (long)kHop - 4);
  assert(lagOff >= (long)kHop / 2);

  // AVEC, le biais s'effondre : la mesure n'est plus systematiquement en
  // retard. Mesure : moyenne -1 ms. C'est le gain principal, et il est
  // chiffre.
  assert(labs(lagOn) * 3 <= labs(lagOff));
  assert(labs(lagOn) <= (long)kHop / 4);

  // L'etendue diminue aussi, mais moins : elle passe de 14 a 9 ms. Le residu
  // n'est plus de la quantification, c'est l'erreur de MODELE - on interpole
  // une droite dans un domaine ou l'enveloppe est courbe. Le dire ainsi est
  // plus honnete que d'annoncer une precision a la milliseconde.
  assert(spreadOn < spreadOff);
  assert(spreadOn <= (long)kHop - 4);
}

// ===========================================================================
// 12 - L'etalement de fenetre est un BIAIS mesure, pas une imprecision cachee
// ===========================================================================
//
// Le niveau d'une frame est une moyenne sur les 32 ms qui la precedent. Selon
// le critere, cela se traduit par un retard different, et ce test les mesure
// tous les trois au lieu de les supposer :
//
//   - APPARITION : quasiment aucun retard (le seuil est bas sur la montee et
//     l'interpolation remonte dans la frame precedente).
//   - NIVEAU ETABLI : environ une fenetre entiere, meme pour un echelon.
//   - DISPARITION : une vingtaine de ms.

void timing_window_smearing_is_measured_and_bounded() {
  long spread = 0, lag = 0;
  sweepOnsetResidual(true, spread, lag);

  // Apparition : le biais tient dans un quart de periode de frame.
  assert(labs(lag) <= (long)kHop / 4);
  // Et l'erreur totale, biais compris, dans une demi-periode.
  assert(spread <= (long)kHop - 4);

  AcousticTiming t;

  // Niveau etabli : sur un ECHELON parfait, tout le temps mesure est celui de
  // la chaine. Il vaut environ une fenetre, et surement pas zero.
  {
    Scenario sc;
    sc.env.onsetMs = 20300.0f;
    sc.env.riseMs = 0.0f;
    runScenario(t, sc);
    const NoteTiming& r = result(t);
    assert(r.attackTime.valid);
    assert(r.attackTime.ms >= kSpan);
    assert(r.attackTime.ms <= kSpan + 2u * kHop);
  }

  // Disparition : le retard sur le franchissement physique du seuil est
  // POSITIF, borne, et a peu pres CONSTANT quelle que soit la pente - c'est ce
  // qui en fait un biais exploitable plutot qu'une incertitude.
  {
    const float kFalls[3] = {30.0f, 60.0f, 130.0f};
    long lags[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
      Scenario sc;
      sc.env.onsetMs = 20300.0f;
      sc.env.offMs = 20600.0f;
      sc.env.fallMs = kFalls[i];
      sc.releaseMs = 20600;
      sc.durationMs = 1100;
      runScenario(t, sc);
      const NoteTiming& r = result(t);
      assert(r.releaseTime.valid);
      // 0,9 * fallMs : l'amplitude vaut alors un dixieme du palier, soit
      // exactement kReleaseFallDb = 20 dB sous lui.
      lags[i] = (long)r.releaseTime.ms - lround(0.9 * (double)kFalls[i]);
      assert(lags[i] > 0);
      assert(lags[i] <= (long)(kSpan + kHop));
    }
    long lo = lags[0], hi = lags[0];
    for (int i = 1; i < 3; i++) { if (lags[i] < lo) lo = lags[i]; if (lags[i] > hi) hi = lags[i]; }
    assert(hi - lo <= (long)kHop);   // constant a une frame pres
  }

  // REPETABILITE de l'attaque : decaler l'apparition physique dans la frame ne
  // doit pas faire bouger attackTime de plus de deux periodes de frame. Ce
  // n'est PAS une compensation exacte - l'alignement de la fenetre de plateau
  // introduit une gigue d'environ une frame, mesuree a 15 ms - et c'est dit
  // plutot que masque.
  {
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (int i = 0; i < 8; i++) {
      Scenario sc;
      sc.env.onsetMs = 20300.0f + 2.0f * (float)i;
      sc.env.riseMs = 30.0f;
      runScenario(t, sc);
      const NoteTiming& r = result(t);
      assert(r.attackTime.valid);
      if (r.attackTime.ms < lo) lo = r.attackTime.ms;
      if (r.attackTime.ms > hi) hi = r.attackTime.ms;
    }
    assert(hi - lo <= 2u * kHop);
  }
}

// ===========================================================================
// 13 - Bout en bout depuis AcousticFeatures
// ===========================================================================
//
// La chaine reelle produit des AcousticFeatures. L'adaptateur doit en tirer un
// TimingFrame correct, y compris le drapeau `pitchValid` qu'AcousticFeatures
// n'expose pas et qu'il faut reconstruire avec le MEME critere que
// PitchDetector - sinon du bruit passerait pour un pitch au moment precis ou
// le signal est le moins periodique : l'attaque.

void timing_end_to_end_from_acoustic_features() {
  AcousticTiming t;
  PitchDetector det;
  std::vector<float> buf;

  NoteEnvelope env;
  env.f0 = 587.33f;            // D5
  env.onsetMs = 20300.0f;
  env.riseMs = 25.0f;
  env.offMs = 20700.0f;
  env.fallMs = 40.0f;

  const uint32_t start = 20000, cmd = 20160, air = 20180, rel = 20700;
  bool sentCmd = false, sentAir = false, sentRel = false;
  uint32_t seq = 0;

  for (uint32_t ts = start; ts <= start + 1000u; ts += kHop) {
    if (!sentCmd && ts >= cmd) { t.noteCommanded(cmd); sentCmd = true; }
    if (!sentAir && ts >= air) { assert(t.airCommanded(air)); sentAir = true; }
    if (!sentRel && ts >= rel) { assert(t.noteReleased(rel)); sentRel = true; }

    buildFrameSamples(buf, env, ts);

    AcousticFeatures feat;
    feat.frameSequence = ++seq;
    feat.timestamp = ts;
    AcousticFeatureBuilder::fillLevel(feat, AudioLevel::compute(buf.data(), kN));
    const PitchResult p = det.detect(buf.data(), kN);
    AcousticFeatureBuilder::fillPitch(feat, p, feat.rms > MIC_RMS_ABSOLUTE_MIN);

    const TimingFrame f = AcousticTiming::fromFeatures(feat);
    assert(f.timestampMs == ts);
    assert(f.rmsDbFS == feat.rmsDbFS);
    assert(f.pitchValid == p.valid);
    t.update(f);
  }

  const NoteTiming& r = t.last();
  assert(r.outcome == TIMING_COMPLETE);
  assert(r.commandToSoundLatency.valid && r.commandToSoundLatency.ms > 0);
  assert(r.airToSoundLatency.valid);
  assert(r.attackTime.valid);
  assert(r.releaseTime.valid);
  // Le pitch a ete detecte sur du vrai signal, et apres l'apparition.
  assert(r.hasPitchDetected);
  assert((int32_t)(r.pitchDetectedTimestamp - r.soundOnsetTimestamp) >= 0);
  assert(r.pitchStabilizationTime.valid);
  // Le plancher a bien ete mesure avant l'ordre : pas de repli.
  assert(r.baselineValid);
  assert(r.onsetThresholdDbFS > r.baselineDbFS);

  // L'adaptateur, verifie DIRECTEMENT et pas seulement en passant : une
  // frequence dans la plage accompagnee d'une confiance insuffisante est
  // exactement ce que YIN produit sur un transitoire d'attaque. La prendre
  // pour un pitch daterait pitchDetectedTimestamp sur du bruit.
  {
    AcousticFeatures weak;
    weak.timestamp = 12345;
    weak.rmsDbFS = -20.0f;
    weak.pitchHz = 587.33f;
    weak.pitchConfidence = MIC_YIN_CONFIDENCE_MIN - 0.05f;
    assert(!AcousticTiming::fromFeatures(weak).pitchValid);

    weak.pitchConfidence = MIC_YIN_CONFIDENCE_MIN;
    assert(AcousticTiming::fromFeatures(weak).pitchValid);

    // Aucune frequence : aucun pitch, quelle que soit la confiance annoncee.
    weak.pitchHz = 0.0f;
    weak.pitchConfidence = 1.0f;
    assert(!AcousticTiming::fromFeatures(weak).pitchValid);
  }
}

// ===========================================================================
// 14 - Repli explicite quand aucun plancher n'a ete mesure
// ===========================================================================

void timing_baseline_fallback_is_explicit() {
  AcousticTiming t;
  // Ordre donne AVANT la moindre frame : le critere relatif est impossible.
  t.noteCommanded(140000);
  assert(!t.current().baselineValid);
  // Seul le plancher absolu s'applique, et il vaut ce que le firmware appelle
  // deja "en dessous, ce n'est pas du son".
  assert(fabsf(t.current().onsetThresholdDbFS -
               AudioLevel::toDbFS(MIC_RMS_ABSOLUTE_MIN)) < 0.01f);

  FakeStream s(t, 140000);
  s.feed(-70.0f, 2);                 // sous le plancher absolu : rien
  assert(!t.current().hasSoundOnset);
  s.feed(-30.0f, 2);                 // largement au-dessus : apparition
  assert(t.current().hasSoundOnset);
  assert(t.current().commandToSoundLatency.valid);

  // Un plancher tres bruyant remonte le seuil : c'est tout l'interet du
  // critere relatif.
  AcousticTiming t2;
  FakeStream s2(t2, 150000);
  s2.feed(-30.0f, 10);               // machinerie bruyante a -30 dBFS
  t2.noteCommanded(s2.ts);
  assert(t2.current().baselineValid);
  assert(fabsf(t2.current().onsetThresholdDbFS - (-18.0f)) < 0.01f);
  // Un son a -25 dBFS serait au-dessus du plancher ABSOLU, mais il ne depasse
  // pas le bruit de la machine : ce n'est pas une attaque.
  s2.feed(-25.0f, 4);
  assert(!t2.current().hasSoundOnset);
  s2.feed(-15.0f, 2);
  assert(t2.current().hasSoundOnset);
}

// ===========================================================================
// 15 - Une frame isolee n'est pas une attaque
// ===========================================================================
//
// Un claquement de solenoide (SOLENOID_ACTIVATION_TIME_MS = 50 ms) remplit
// largement une frame d'analyse. Sans confirmation, il daterait une attaque a
// lui tout seul. Avec, il faut kOnsetConfirmFrames frames consecutives - mais
// l'instant retenu reste celui de la PREMIERE, sinon la confirmation se paierait
// en precision.

void timing_isolated_frame_is_not_an_onset() {
  AcousticTiming t;
  FakeStream s(t, 160000);
  s.feed(-60.0f, 10);
  const uint32_t cmd = s.ts;
  t.noteCommanded(cmd);

  // Un pic isole, largement au-dessus du seuil (-48), puis retour au plancher.
  s.feed(-10.0f, 1);
  assert(!t.current().hasSoundOnset);
  s.feed(-60.0f, 3);
  assert(!t.current().hasSoundOnset);
  assert(t.state() == TIMING_WAIT_ONSET);

  // Deux frames consecutives, elles, font une attaque.
  const uint32_t first = s.ts;
  s.feed(-20.0f, 1);
  assert(!t.current().hasSoundOnset);       // la premiere ne suffit pas
  s.feed(-20.0f, 1);
  assert(t.current().hasSoundOnset);

  // Et l'instant retenu est celui de la PREMIERE frame (interpole dans
  // l'intervalle qui la precede), pas celui de la confirmation.
  const uint32_t onset = t.current().soundOnsetTimestamp;
  assert((int32_t)(onset - first) <= 0);
  assert((int32_t)(onset - (first - kHop)) >= 0);
}

}  // namespace

void timing_run_all_tests() {
  timing_measures_a_known_command_latency();
  timing_measures_a_known_attack();
  timing_measures_a_known_release();
  timing_measures_pitch_stabilisation();
  timing_never_invents_a_measure();
  timing_note_cut_during_attack();
  timing_survives_millis_overflow();
  timing_rejects_inconsistent_events();
  timing_every_pending_measure_is_capped();
  timing_release_hysteresis_ignores_a_ripple();
  timing_interpolation_beats_frame_quantisation();
  timing_window_smearing_is_measured_and_bounded();
  timing_end_to_end_from_acoustic_features();
  timing_baseline_fallback_is_explicit();
  timing_isolated_frame_is_not_an_onset();
}

#ifdef STANDALONE_TEST_MAIN
#include <cstdio>
int main() { timing_run_all_tests(); printf("timing tests passed\n"); return 0; }
#endif
