/***********************************************************************************************
 * test_audio.cpp - Tests natifs de la chaine d'analyse acoustique
 *
 * PHASE 0 : tests de reference. Ils capturent le comportement ACTUEL de la
 * detection de pitch et de la classification du signal brut, AVANT toute
 * modification de la chaine audio. Toute evolution ulterieure doit soit les
 * laisser verts, soit les modifier explicitement en disant pourquoi.
 *
 * Tous les signaux sont synthetiques et reproductibles (voir audio_signals.h).
 * Cela valide le TRAITEMENT DU SIGNAL, jamais le comportement acoustique reel
 * d'une flute : la distinction est maintenue dans AUDIO_ARCHITECTURE.md.
 ***********************************************************************************************/
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "settings.h"
#include "PitchDetector.h"
#include "PitchMath.h"
#include "audio_signals.h"

namespace {

using audiosig::ToneSpec;

constexpr int kFrame = MIC_BUFFER_SIZE;
constexpr float kFs = (float)MIC_SAMPLE_RATE;

// ---------------------------------------------------------------------------
// PHASE 0.1 - Reference : detection de pitch sur signaux purs
// ---------------------------------------------------------------------------

void ref_pitch_pure_tones_in_range() {
  PitchDetector det;
  std::vector<float> buf(kFrame);

  // Plage musicale utile de l'instrument : la note MIDI doit etre exacte.
  const float kTones[] = {220.0f, 261.63f, 440.0f, 587.33f, 880.0f, 1046.50f};
  for (float f0 : kTones) {
    audiosig::pureTone(buf.data(), kFrame, f0, 0.4f, kFs);
    PitchResult r = det.detect(buf.data(), kFrame);
    assert(r.valid);
    assert(PitchMath::hzToMidi(r.hz) == PitchMath::hzToMidi(f0));
    assert(r.confidence > 0.7f);
  }

  // Haut de la plage : la resolution de tau a 32 kHz limite la precision. On
  // exige l'absence d'erreur d'octave, pas la justesse au cent pres.
  for (float f0 : {2000.0f, 2500.0f, 3000.0f, 3800.0f}) {
    audiosig::pureTone(buf.data(), kFrame, f0, 0.4f, kFs);
    PitchResult r = det.detect(buf.data(), kFrame);
    assert(r.valid);
    int d = PitchMath::hzToMidi(r.hz) - PitchMath::hzToMidi(f0);
    assert(d >= -1 && d <= 1);
  }
}

// ---------------------------------------------------------------------------
// PHASE 0.2 - Reference : cas degrades
// ---------------------------------------------------------------------------

void ref_pitch_degraded_inputs() {
  PitchDetector det;
  std::vector<float> buf(kFrame);

  // Silence : aucun pitch, aucune frequence.
  audiosig::silence(buf.data(), kFrame);
  { PitchResult r = det.detect(buf.data(), kFrame); assert(!r.valid && r.hz == 0.0f); }

  // Offset continu pur (pas de composante alternative) : aucun pitch.
  audiosig::dcOnly(buf.data(), kFrame, 0.35f);
  { PitchResult r = det.detect(buf.data(), kFrame); assert(!r.valid); }

  // Bruit blanc large bande : pas de periodicite exploitable.
  audiosig::whiteNoise(buf.data(), kFrame, 0.4f);
  { PitchResult r = det.detect(buf.data(), kFrame); assert(!r.valid); }

  // Niveau tres faible mais signal pur : le detecteur lui-meme n'a pas de
  // porte de niveau (c'est AudioAnalyzer qui gate sur le RMS), donc il doit
  // quand meme trouver la periode.
  audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.001f, kFs);
  { PitchResult r = det.detect(buf.data(), kFrame); assert(PitchMath::hzToMidi(r.hz) == 69); }

  // Buffer nul / trop court : refus propre, jamais de lecture hors bornes.
  { PitchResult r = det.detect(buf.data(), 0);  assert(!r.valid && r.hz == 0.0f); }
  { PitchResult r = det.detect(buf.data(), 4);  assert(!r.valid && r.hz == 0.0f); }
  { PitchResult r = det.detect(nullptr, 0);     assert(!r.valid); }

  // Hors plage de detection (MIC_PITCH_MIN_HZ..MIC_PITCH_MAX_HZ) : rejet.
  audiosig::pureTone(buf.data(), kFrame, 80.0f, 0.4f, kFs);
  { PitchResult r = det.detect(buf.data(), kFrame); assert(!r.valid); }
}

void ref_pitch_saturated_and_harmonic() {
  PitchDetector det;
  std::vector<float> buf(kFrame);

  // Signal ecrete : spectre riche en harmoniques impaires, la fondamentale doit
  // rester detectee (pas d'erreur d'octave vers le haut).
  ToneSpec sat; sat.sampleRate = kFs; sat.f0 = 440.0f; sat.amp = 0.9f; sat.clipAt = 0.5f;
  audiosig::fill(buf.data(), kFrame, sat);
  { PitchResult r = det.detect(buf.data(), kFrame); assert(PitchMath::hzToMidi(r.hz) == 69); }

  // Fondamentale + harmonique 2 forte : ne doit PAS descendre d'une octave.
  ToneSpec h; h.sampleRate = kFs; h.f0 = 523.25f; h.amp = 0.4f; h.h2 = 0.35f;
  audiosig::fill(buf.data(), kFrame, h);
  { PitchResult r = det.detect(buf.data(), kFrame); assert(r.valid && PitchMath::hzToMidi(r.hz) == 72); }

  // Note "flute" complete (H2/H3/H4 + souffle) : fondamentale correcte.
  audiosig::fluteLike(buf.data(), kFrame, 587.33f, 0.4f, 0.02f, kFs);
  { PitchResult r = det.detect(buf.data(), kFrame); assert(r.valid && PitchMath::hzToMidi(r.hz) == 74); }
}

// PRECISION EN CENTS - ETAT DE REFERENCE, PAS UN OBJECTIF.
//
// Mesure faite sur la chaine actuelle (PHASE 0) : autour de 440 Hz l'erreur
// atteint 23 cents, et un 440 Hz PUR est rendu a 435,7 Hz (-17 cents). Un pas
// de tau vaut 23,6 cents a cette frequence ; l'interpolation parabolique est
// donc pratiquement sans effet. La cause probable est la fenetre de Hann
// appliquee AVANT la fonction de difference YIN : l'enveloppe biaise
// d(tau) et deplace le minimum.
//
// Ce test verrouille le comportement actuel pour que la PHASE 2 puisse
// demontrer un gain mesurable. Il sera resserre a ce moment-la, pas avant.
void ref_pitch_cents_accuracy_current_limit() {
  PitchDetector det;
  std::vector<float> buf(kFrame);

  const float kCases[] = {-40.0f, -20.0f, -10.0f, 0.0f, +10.0f, +20.0f, +40.0f};
  float worst = 0.0f;
  for (float cents : kCases) {
    const float hz = PitchMath::midiToHz(69) * powf(2.0f, cents / 1200.0f);
    audiosig::pureTone(buf.data(), kFrame, hz, 0.4f, kFs);
    PitchResult r = det.detect(buf.data(), kFrame);
    assert(r.valid);
    // La NOTE reste toujours correcte : c'est ce dont depend l'auto-calibration
    // actuelle, et c'est pour cela que le defaut de precision est passe inapercu.
    assert(PitchMath::hzToMidi(r.hz) == 69);
    const float err = fabsf(PitchMath::hzToCents(r.hz, 69) - cents);
    if (err > worst) worst = err;
  }
  // Borne haute observee aujourd'hui. Un resserrement de cette valeur est le
  // critere de reussite de la PHASE 2.
  assert(worst < 25.0f);
  assert(worst > 10.0f);   // si cela devient faux, la precision a ETE amelioree
}

void ref_pitch_octave_relationships() {
  PitchDetector det;
  std::vector<float> buf(kFrame);
  // Une octave au-dessus et une octave en dessous d'une note de reference
  // doivent etre detectees pour ce qu'elles sont, sans repliement.
  for (int midi : {60, 72, 84}) {
    const float hz = PitchMath::midiToHz(midi);
    audiosig::pureTone(buf.data(), kFrame, hz, 0.4f, kFs);
    PitchResult r = det.detect(buf.data(), kFrame);
    assert(r.valid);
    assert(PitchMath::hzToMidi(r.hz) == midi);
  }
}

// ---------------------------------------------------------------------------
// PHASE 0.3 - Reference : RMS et classification du signal brut
// ---------------------------------------------------------------------------

void ref_rms_reference_values() {
  std::vector<float> buf(kFrame);

  audiosig::silence(buf.data(), kFrame);
  assert(PitchDetector::rms(buf.data(), kFrame) < 1e-6f);

  // Le RMS retire la composante continue : un offset pur ne doit pas compter
  // comme du niveau sonore.
  audiosig::dcOnly(buf.data(), kFrame, 0.5f);
  assert(PitchDetector::rms(buf.data(), kFrame) < 1e-5f);

  // Sinus d'amplitude A : RMS = A / sqrt(2).
  audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.5f, kFs);
  {
    const float r = PitchDetector::rms(buf.data(), kFrame);
    assert(fabsf(r - 0.5f / sqrtf(2.0f)) < 0.01f);
  }

  // Le meme sinus avec un fort offset donne le meme RMS.
  ToneSpec s; s.sampleRate = kFs; s.f0 = 440.0f; s.amp = 0.5f; s.dc = 0.4f;
  audiosig::fill(buf.data(), kFrame, s);
  {
    const float r = PitchDetector::rms(buf.data(), kFrame);
    assert(fabsf(r - 0.5f / sqrtf(2.0f)) < 0.01f);
  }

  // Taille nulle : pas de division par zero.
  assert(PitchDetector::rms(buf.data(), 0) == 0.0f);
}

void ref_raw_signal_classification() {
  const size_t kN = 256;
  std::vector<int32_t> raw(kN);

  // Bus au repos / micro non cable.
  for (size_t i = 0; i < kN; i++) raw[i] = 0;
  assert(PitchDetector::classifyRaw(raw.data(), kN) == MIC_SIG_ALL_ZERO);

  // Ligne bloquee : non nulle mais constante (pas d'horloge).
  for (size_t i = 0; i < kN; i++) raw[i] = (int32_t)1000 << 8;
  assert(PitchDetector::classifyRaw(raw.data(), kN) == MIC_SIG_STUCK);

  // Sature en permanence.
  for (size_t i = 0; i < kN; i++) raw[i] = (int32_t)0x7FFFFF << 8;
  assert(PitchDetector::classifyRaw(raw.data(), kN) == MIC_SIG_SATURATED);

  // Signal normal.
  ToneSpec s; s.sampleRate = kFs; s.f0 = 1000.0f; s.amp = 0.5f;
  audiosig::fillI2s(raw.data(), kN, s);
  assert(PitchDetector::classifyRaw(raw.data(), kN) == MIC_SIG_OK);

  // Taille nulle : traite comme "rien recu", jamais comme un micro sain.
  assert(PitchDetector::classifyRaw(raw.data(), 0) == MIC_SIG_ALL_ZERO);
}

// ---------------------------------------------------------------------------
// PHASE 0.4 - Reference : le generateur de signaux lui-meme
// ---------------------------------------------------------------------------

void ref_signal_generator_is_reproducible() {
  std::vector<float> a(512), b(512);
  ToneSpec s; s.sampleRate = kFs; s.f0 = 440.0f; s.amp = 0.3f; s.noise = 0.1f; s.seed = 777u;
  audiosig::fill(a.data(), 512, s);
  audiosig::fill(b.data(), 512, s);
  for (size_t i = 0; i < 512; i++) assert(a[i] == b[i]);

  // Deux blocs consecutifs doivent former un signal continu : la phase se
  // poursuit, sans discontinuite artificielle au raccord.
  std::vector<float> whole(1024), first(512), second(512);
  ToneSpec p; p.sampleRate = kFs; p.f0 = 500.0f; p.amp = 0.5f;
  audiosig::fill(whole.data(), 1024, p);
  audiosig::fill(first.data(), 512, p, 0);
  audiosig::fill(second.data(), 512, p, 512);
  for (size_t i = 0; i < 512; i++) {
    assert(fabsf(whole[i] - first[i]) < 1e-6f);
    assert(fabsf(whole[512 + i] - second[i]) < 1e-6f);
  }

  // Conversion vers le mot I2S : cale a gauche, les 8 bits bas restent nuls.
  assert((audiosig::toI2sWord(0.0f) & 0xFF) == 0);
  assert(audiosig::toI2sWord(1.0f) > 0);
  assert(audiosig::toI2sWord(-1.0f) < 0);
  assert(audiosig::toI2sWord(2.0f) == audiosig::toI2sWord(1.0f));    // borne haute
  assert(audiosig::toI2sWord(-2.0f) == audiosig::toI2sWord(-1.0f));  // borne basse
}

}  // namespace

void audio_run_all_tests() {
  ref_pitch_pure_tones_in_range();
  ref_pitch_degraded_inputs();
  ref_pitch_saturated_and_harmonic();
  ref_pitch_cents_accuracy_current_limit();
  ref_pitch_octave_relationships();
  ref_rms_reference_values();
  ref_raw_signal_classification();
  ref_signal_generator_is_reproducible();
}
