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
#include "AudioRingBuffer.h"
#include "AudioLevel.h"
#include "SpectralAnalyzer.h"
#include "AcousticFeatures.h"
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

// PRECISION EN CENTS - resserree en PHASE 2.
//
// Ce test etait le fil de detente de la PHASE 0 : il exigeait que l'erreur
// reste SUPERIEURE a 10 cents, pour qu'une amelioration ne puisse pas passer
// inapercue. La PHASE 2 l'a fait echouer, comme prevu, et il est desormais
// resserre a l'etat reellement atteint.
//
// Historique mesure sur les memes signaux :
//   PHASE 0 (parabole sur tau entier + fenetre de Hann) : 47 c pire-cas
//   + raffinement fractionnaire, fenetre conservee       : 4,4 c
//   + retrait de la fenetre (etat actuel)                : 0,17 c
void pitch_cents_accuracy_is_sub_cent() {
  PitchDetector det;
  std::vector<float> buf(kFrame);

  const float kCases[] = {-40.0f, -30.0f, -20.0f, -10.0f, 0.0f, +10.0f, +20.0f, +30.0f, +40.0f};
  float worst = 0.0f;
  for (float cents : kCases) {
    const float hz = PitchMath::midiToHz(69) * powf(2.0f, cents / 1200.0f);
    audiosig::pureTone(buf.data(), kFrame, hz, 0.4f, kFs);
    PitchResult r = det.analyse(buf.data(), kFrame);
    assert(r.valid);
    assert(r.midi == 69);
    const float err = fabsf(r.cents - cents);
    if (err > worst) worst = err;
    // Le SIGNE doit maintenant etre correct, ce qui etait faux en PHASE 0.
    if (fabsf(cents) > 5.0f) assert((r.cents < 0.0f) == (cents < 0.0f));
  }
  assert(worst < 1.0f);

  // Sur toute la tessiture utile, y compris le haut ou le pas de tau devient
  // grossier (55 cents par pas a 1046 Hz).
  float worstWide = 0.0f;
  for (int midi = 60; midi <= 88; midi++) {
    const float hz = PitchMath::midiToHz(midi);
    audiosig::pureTone(buf.data(), kFrame, hz, 0.4f, kFs);
    PitchResult r = det.analyse(buf.data(), kFrame);
    assert(r.valid && r.midi == midi);
    const float err = fabsf(r.cents);
    if (err > worstWide) worstWide = err;
  }
  assert(worstWide < 2.0f);
}

// COMPARAISON A/B demandee par la PHASE 2.1 : la fenetre de Hann appliquee
// avant YIN degrade bien la mesure. Les deux chemins partagent desormais le
// meme raffinement fractionnaire, donc la comparaison isole l'effet de la
// SEULE fenetre.
void pitch_window_ab_comparison() {
  std::vector<float> buf(kFrame), copy(kFrame);
  float worstPlain = 0.0f, worstWindowed = 0.0f;
  double sumPlain = 0.0, sumWindowed = 0.0;
  int n = 0;

  for (int midi = 60; midi <= 84; midi++) {
    const float hz = PitchMath::midiToHz(midi);
    audiosig::pureTone(buf.data(), kFrame, hz, 0.4f, kFs);

    PitchDetector a;
    PitchResult plain = a.analyse(buf.data(), kFrame);

    // detectWindowed() modifie son tampon : on lui en donne une copie.
    copy = buf;
    PitchDetector b;
    PitchResult windowed = b.detectWindowed(copy.data(), kFrame);

    assert(plain.valid && windowed.valid);
    assert(plain.midi == midi && windowed.midi == midi);

    const float ep = fabsf(plain.cents);
    const float ew = fabsf(windowed.cents);
    if (ep > worstPlain) worstPlain = ep;
    if (ew > worstWindowed) worstWindowed = ew;
    sumPlain += ep; sumWindowed += ew; n++;

    // Le signal d'origine n'est PAS modifie par analyse() : c'est ce qui permet
    // de reutiliser la meme frame pour l'analyse spectrale.
    assert(buf[10] != copy[10] || buf[10] == 0.0f);
  }

  // L'ecart doit etre net, pas marginal : la fenetre coute au moins un facteur
  // 5 sur l'erreur moyenne.
  assert(sumPlain * 5.0 < sumWindowed);
  assert(worstPlain < worstWindowed);
  assert(worstPlain < 1.0f);
}

// analyse() ne doit JAMAIS modifier le tampon de l'appelant.
void pitch_analysis_is_non_destructive() {
  PitchDetector det;
  std::vector<float> buf(kFrame), before(kFrame);
  audiosig::fluteLike(buf.data(), kFrame, 523.25f, 0.4f, 0.02f, kFs);
  before = buf;

  PitchResult r = det.analyse(buf.data(), kFrame);
  assert(r.valid);
  for (int i = 0; i < kFrame; i++) assert(buf[i] == before[i]);

  // detect() non plus (il n'ajoute que le suivi de stabilite).
  det.detect(buf.data(), kFrame);
  for (int i = 0; i < kFrame; i++) assert(buf[i] == before[i]);
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


// ---------------------------------------------------------------------------
// PHASE 1.1 - Anneau audio : une frame partielle n'est JAMAIS produite
// ---------------------------------------------------------------------------

void ring_never_produces_a_partial_frame() {
  AudioRingBuffer ring;
  AudioCaptureStats st;
  std::vector<float> out(MIC_ANALYSIS_FRAME_SIZE);
  std::vector<float> in(MIC_ANALYSIS_FRAME_SIZE);
  audiosig::pureTone(in.data(), MIC_ANALYSIS_FRAME_SIZE, 440.0f, 0.5f, kFs);

  // Anneau vide : refus, et le refus est COMPTE.
  assert(!ring.readFrame(out.data(), MIC_ANALYSIS_FRAME_SIZE, MIC_ANALYSIS_HOP_SIZE, &st));
  assert(st.bufferUnderruns == 1);
  assert(st.framesProduced == 0);

  // C'est exactement le defaut A0-1 : l'ancienne acquisition aurait traite
  // n'importe laquelle de ces quantites comme une frame complete.
  for (size_t partial : {size_t(1), size_t(100), size_t(300),
                         size_t(MIC_ANALYSIS_FRAME_SIZE - 1)}) {
    AudioRingBuffer r;
    AudioCaptureStats s2;
    r.write(in.data(), partial, &s2);
    assert(!r.readFrame(out.data(), MIC_ANALYSIS_FRAME_SIZE, MIC_ANALYSIS_HOP_SIZE, &s2));
    assert(s2.framesProduced == 0);
    assert(s2.samplesReceived == (uint32_t)partial);
  }

  // Exactement assez : la frame sort, complete et identique a l'entree.
  ring.reset();
  st.reset();
  ring.write(in.data(), MIC_ANALYSIS_FRAME_SIZE, &st);
  assert(ring.readFrame(out.data(), MIC_ANALYSIS_FRAME_SIZE, MIC_ANALYSIS_HOP_SIZE, &st));
  assert(st.framesProduced == 1);
  for (int i = 0; i < MIC_ANALYSIS_FRAME_SIZE; i++) assert(out[i] == in[i]);
}

// ---------------------------------------------------------------------------
// PHASE 1.2 - Recouvrement : frame n+1 commence a hop, pas a frameSize
// ---------------------------------------------------------------------------

void ring_frames_overlap_by_hop() {
  AudioRingBuffer ring;
  AudioCaptureStats st;
  const size_t kFrameSz = MIC_ANALYSIS_FRAME_SIZE;
  const size_t kHop = MIC_ANALYSIS_HOP_SIZE;

  // Rampe numerotee : chaque echantillon porte son index, la position d'une
  // frame se lit donc directement dans son contenu.
  std::vector<float> ramp(2048);
  for (size_t i = 0; i < ramp.size(); i++) ramp[i] = (float)i;

  std::vector<float> f0(kFrameSz), f1(kFrameSz), f2(kFrameSz);
  ring.write(ramp.data(), 2048, &st);
  assert(st.droppedSamples == 0);      // 2048 = capacite exacte, rien ne deborde

  assert(ring.readFrame(f0.data(), kFrameSz, kHop, &st));
  assert(ring.readFrame(f1.data(), kFrameSz, kHop, &st));
  assert(ring.readFrame(f2.data(), kFrameSz, kHop, &st));
  assert(st.framesProduced == 3);

  // frame 0 : 0..1023 / frame 1 : 512..1535 / frame 2 : 1024..2047
  assert(f0[0] == 0.0f);
  assert(f1[0] == (float)kHop);
  assert(f2[0] == (float)(2 * kHop));

  // La seconde moitie de la frame n est la premiere moitie de la frame n+1 :
  // c'est la definition du recouvrement de 50 %.
  for (size_t i = 0; i < kFrameSz - kHop; i++) {
    assert(f0[kHop + i] == f1[i]);
    assert(f1[kHop + i] == f2[i]);
  }

  // Un hop nul ou superieur a la frame est ramene a la taille de frame : pas de
  // boucle infinie, pas de lecture qui n'avance jamais.
  AudioRingBuffer r2;
  AudioCaptureStats s2;
  r2.write(ramp.data(), 2048, &s2);
  std::vector<float> g0(kFrameSz), g1(kFrameSz);
  assert(r2.readFrame(g0.data(), kFrameSz, 0, &s2));
  assert(r2.readFrame(g1.data(), kFrameSz, 99999, &s2));
  assert(g1[0] == (float)kFrameSz);    // avance d'une frame entiere
}

// ---------------------------------------------------------------------------
// PHASE 1.3 - Debordement : les echantillons ANCIENS sont sacrifies, et comptes
// ---------------------------------------------------------------------------

void ring_overrun_drops_oldest_and_counts() {
  AudioRingBuffer ring;
  AudioCaptureStats st;
  const size_t kCap = AudioRingBuffer::capacity();

  std::vector<float> ramp(kCap + 500);
  for (size_t i = 0; i < ramp.size(); i++) ramp[i] = (float)i;

  ring.write(ramp.data(), kCap, &st);
  assert(ring.available() == kCap);
  assert(st.bufferOverruns == 0);

  // 500 echantillons de plus : les 500 PLUS ANCIENS partent.
  const size_t dropped = ring.write(ramp.data() + kCap, 500, &st);
  assert(dropped == 500);
  assert(st.bufferOverruns == 1);
  assert(st.droppedSamples == 500);
  assert(ring.available() == kCap);

  // Le plus ancien echantillon encore present est bien le 500e.
  std::vector<float> out(MIC_ANALYSIS_FRAME_SIZE);
  assert(ring.readFrame(out.data(), MIC_ANALYSIS_FRAME_SIZE, MIC_ANALYSIS_HOP_SIZE, &st));
  assert(out[0] == 500.0f);

  // Ecriture plus grande que l'anneau entier : seuls les DERNIERS echantillons
  // sont conserves. Du son vieux de plusieurs frames n'a plus d'interet.
  AudioRingBuffer r2;
  AudioCaptureStats s2;
  std::vector<float> big(kCap * 3);
  for (size_t i = 0; i < big.size(); i++) big[i] = (float)i;
  const size_t d2 = r2.write(big.data(), big.size(), &s2);
  assert(d2 == big.size() - kCap);
  assert(r2.available() == kCap);
  std::vector<float> o2(MIC_ANALYSIS_FRAME_SIZE);
  assert(r2.readFrame(o2.data(), MIC_ANALYSIS_FRAME_SIZE, MIC_ANALYSIS_HOP_SIZE, &s2));
  assert(o2[0] == (float)(big.size() - kCap));
}

// ---------------------------------------------------------------------------
// PHASE 1.4 - Enroulement : une frame a cheval sur la fin du tableau
// ---------------------------------------------------------------------------

void ring_wraps_without_corrupting_a_frame() {
  AudioRingBuffer ring;
  AudioCaptureStats st;
  const size_t kFrameSz = MIC_ANALYSIS_FRAME_SIZE;
  const size_t kHop = MIC_ANALYSIS_HOP_SIZE;
  std::vector<float> out(kFrameSz);

  // On fait tourner l'anneau sur plusieurs tours complets en verifiant a chaque
  // frame que le contenu est exactement la rampe attendue - y compris quand la
  // frame est coupee par la fin du tableau interne.
  float next = 0.0f;
  std::vector<float> chunk(MIC_I2S_CHUNK_SAMPLES);
  size_t expectedStart = 0;
  int framesChecked = 0;

  for (int iter = 0; iter < 200; iter++) {
    for (size_t i = 0; i < chunk.size(); i++) chunk[i] = next++;
    ring.write(chunk.data(), chunk.size(), &st);
    while (ring.frameReady(kFrameSz)) {
      assert(ring.readFrame(out.data(), kFrameSz, kHop, &st));
      for (size_t i = 0; i < kFrameSz; i++) {
        assert(out[i] == (float)(expectedStart + i));
      }
      expectedStart += kHop;
      framesChecked++;
    }
  }
  assert(st.droppedSamples == 0);       // consommation a la cadence de production
  assert(framesChecked > 50);           // on a bien enroule plusieurs fois
  assert(st.framesProduced == (uint32_t)framesChecked);
}

// ---------------------------------------------------------------------------
// PHASE 1.5 - Regime permanent : production materielle vs consommation
// ---------------------------------------------------------------------------

void ring_steady_state_does_not_drift() {
  // Reproduit le regime reel : le DMA produit Fe echantillons par seconde, et
  // l'analyse consomme un hop par frame. L'equilibre ne tient QUE si la cadence
  // d'analyse suit le hop - c'est la raison pour laquelle AudioAnalyzer n'a
  // aucun minuteur d'analyse.
  AudioRingBuffer ring;
  AudioCaptureStats st;
  std::vector<float> out(MIC_ANALYSIS_FRAME_SIZE);
  std::vector<float> chunk(MIC_I2S_CHUNK_SAMPLES);
  audiosig::pureTone(chunk.data(), chunk.size(), 440.0f, 0.4f, kFs);

  // 5 secondes simulees : 32 000 * 5 / 256 = 625 vidages de DMA.
  for (int i = 0; i < 625; i++) {
    ring.write(chunk.data(), chunk.size(), &st);
    if (ring.frameReady(MIC_ANALYSIS_FRAME_SIZE)) {
      ring.readFrame(out.data(), MIC_ANALYSIS_FRAME_SIZE, MIC_ANALYSIS_HOP_SIZE, &st);
    }
  }
  // Aucun echantillon perdu, et le nombre de frames correspond au debit attendu
  // (un hop consomme par frame).
  assert(st.droppedSamples == 0);
  assert(st.bufferOverruns == 0);
  const uint32_t expectedFrames = (uint32_t)((625 * MIC_I2S_CHUNK_SAMPLES) / MIC_ANALYSIS_HOP_SIZE);
  // Tolerance d'une frame : l'amorcage demande une frame complete.
  assert(st.framesProduced + 3 >= expectedFrames && st.framesProduced <= expectedFrames);
}

// ---------------------------------------------------------------------------
// PHASE 1.6 - Niveau : RMS, crete, dBFS
// ---------------------------------------------------------------------------

void level_rms_peak_and_dbfs() {
  std::vector<float> buf(kFrame);

  // Silence numerique : plancher, jamais -inf ni NaN.
  audiosig::silence(buf.data(), kFrame);
  {
    FrameLevel l = AudioLevel::compute(buf.data(), kFrame);
    assert(l.rms < 1e-6f);
    assert(l.rmsDbFS == MIC_DBFS_FLOOR);
    assert(l.peakDbFS == MIC_DBFS_FLOOR);
    assert(!l.clippingDetected);
  }

  // Sinus pleine echelle : crete 1.0 = 0 dBFS, RMS = 1/sqrt(2) = -3,01 dBFS.
  audiosig::pureTone(buf.data(), kFrame, 440.0f, 1.0f, kFs);
  {
    FrameLevel l = AudioLevel::compute(buf.data(), kFrame);
    assert(fabsf(l.peak - 1.0f) < 0.01f);
    assert(fabsf(l.peakDbFS - 0.0f) < 0.1f);
    assert(fabsf(l.rmsDbFS - (-3.01f)) < 0.1f);
  }

  // Division par deux de l'amplitude = -6,02 dBFS exactement.
  audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.5f, kFs);
  {
    FrameLevel l = AudioLevel::compute(buf.data(), kFrame);
    assert(fabsf(l.rmsDbFS - (-9.03f)) < 0.1f);
    assert(fabsf(l.peakDbFS - (-6.02f)) < 0.1f);
  }

  // Le decalage continu est MESURE mais ne compte pas dans le niveau.
  ToneSpec s; s.sampleRate = kFs; s.f0 = 440.0f; s.amp = 0.5f; s.dc = 0.3f;
  audiosig::fill(buf.data(), kFrame, s);
  {
    FrameLevel l = AudioLevel::compute(buf.data(), kFrame);
    assert(fabsf(l.dcOffset - 0.3f) < 0.01f);
    assert(fabsf(l.rmsDbFS - (-9.03f)) < 0.1f);
  }

  // Conversions : aller-retour et bornes.
  assert(fabsf(AudioLevel::toDbFS(1.0f)) < 1e-4f);
  assert(fabsf(AudioLevel::toDbFS(0.1f) - (-20.0f)) < 1e-3f);
  assert(AudioLevel::toDbFS(0.0f) == MIC_DBFS_FLOOR);
  assert(AudioLevel::toDbFS(-1.0f) == MIC_DBFS_FLOOR);   // valeur absurde bornee
  assert(fabsf(AudioLevel::fromDbFS(-20.0f) - 0.1f) < 1e-4f);

  // Taille nulle / pointeur nul : valeurs par defaut, aucun acces memoire.
  { FrameLevel l = AudioLevel::compute(nullptr, 0); assert(l.rms == 0.0f); }
  { FrameLevel l = AudioLevel::compute(buf.data(), 0); assert(l.rmsDbFS == MIC_DBFS_FLOOR); }
}

// ---------------------------------------------------------------------------
// PHASE 1.7 - Ecretage : transitoire vs saturation permanente
// ---------------------------------------------------------------------------

void level_clipping_detection() {
  std::vector<float> buf(kFrame);

  // Signal sain a -6 dBFS : aucun ecretage.
  audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.5f, kFs);
  {
    FrameLevel l = AudioLevel::compute(buf.data(), kFrame);
    assert(l.clippingRatio == 0.0f);
    assert(!l.clippingDetected);
  }

  // Un SEUL echantillon au rail : compte, mais ne declenche pas l'alerte. Un
  // ecretage bref n'est pas un microphone sature.
  buf[100] = 1.0f;
  {
    FrameLevel l = AudioLevel::compute(buf.data(), kFrame);
    assert(l.clippingRatio > 0.0f);
    assert(l.clippingRatio < MIC_CLIP_RATIO_WARN);
    assert(!l.clippingDetected);
  }

  // Sinus fortement ecrete : une large part de la frame est au rail.
  ToneSpec sat; sat.sampleRate = kFs; sat.f0 = 440.0f; sat.amp = 3.0f; sat.clipAt = 1.0f;
  audiosig::fill(buf.data(), kFrame, sat);
  {
    FrameLevel l = AudioLevel::compute(buf.data(), kFrame);
    assert(l.clippingDetected);
    assert(l.clippingRatio > 0.5f);
  }

  // Juste au-dessus du seuil de proportion : l'alerte se declenche.
  audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.5f, kFs);
  const int needed = (int)(MIC_CLIP_RATIO_WARN * kFrame) + 2;
  for (int i = 0; i < needed; i++) buf[i] = 1.0f;
  {
    FrameLevel l = AudioLevel::compute(buf.data(), kFrame);
    assert(l.clippingDetected);
  }
}


// ---------------------------------------------------------------------------
// PHASE 2.2 - Note attendue : levee deterministe de l'ambiguite d'octave
// ---------------------------------------------------------------------------

void pitch_expected_note_resolves_octave() {
  std::vector<float> buf(kFrame);

  // Sans note attendue, le comportement general reste STRICTEMENT inchange.
  {
    PitchDetector det;
    audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.4f, kFs);
    PitchResult r = det.analyse(buf.data(), kFrame);
    assert(r.valid && r.midi == 69);
    assert(!r.expectedMatch && !r.octaveAbove && !r.octaveBelow);
  }

  // Avec la note attendue, une note correcte est signalee comme telle.
  {
    PitchDetector det;
    det.setExpectedMidiNote(69);
    assert(det.hasExpectedNote() && det.expectedMidiNote() == 69);
    audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.4f, kFs);
    PitchResult r = det.analyse(buf.data(), kFrame);
    assert(r.valid && r.midi == 69);
    assert(r.expectedMatch);
    assert(!r.octaveAbove && !r.octaveBelow);
  }

  // Overblow : la flute sonne une octave au-dessus. La note est detectee pour
  // ce qu'elle est, ET signalee comme octave superieure de l'attendue.
  {
    PitchDetector det;
    det.setExpectedMidiNote(69);
    audiosig::pureTone(buf.data(), kFrame, PitchMath::midiToHz(81), 0.4f, kFs);
    PitchResult r = det.analyse(buf.data(), kFrame);
    assert(r.valid && r.midi == 81);
    assert(r.octaveAbove);
    assert(!r.expectedMatch);
    assert(!r.octaveBelow);
  }

  // Octave en dessous.
  {
    PitchDetector det;
    det.setExpectedMidiNote(81);
    audiosig::pureTone(buf.data(), kFrame, PitchMath::midiToHz(69), 0.4f, kFs);
    PitchResult r = det.analyse(buf.data(), kFrame);
    assert(r.valid && r.midi == 69);
    assert(r.octaveBelow);
    assert(!r.octaveAbove && !r.expectedMatch);
  }

  // Une note voisine (pas une octave) n'est ni un match ni une octave.
  {
    PitchDetector det;
    det.setExpectedMidiNote(69);
    audiosig::pureTone(buf.data(), kFrame, PitchMath::midiToHz(71), 0.4f, kFs);
    PitchResult r = det.analyse(buf.data(), kFrame);
    assert(r.valid && r.midi == 71);
    assert(!r.expectedMatch && !r.octaveAbove && !r.octaveBelow);
  }

  // Bonne note mais franchement fausse en justesse : ce n'est PAS un match.
  // 45 cents arrondit encore a la note 69 (le basculement est a 50 cents) mais
  // depasse MIC_EXPECTED_TOLERANCE_CENTS. C'est precisement la distinction que
  // la calibration doit voir : "la bonne note, mal jouee".
  {
    PitchDetector det;
    det.setExpectedMidiNote(69);
    const float hz = PitchMath::midiToHz(69) * powf(2.0f, 45.0f / 1200.0f);
    audiosig::pureTone(buf.data(), kFrame, hz, 0.4f, kFs);
    PitchResult r = det.analyse(buf.data(), kFrame);
    assert(r.valid && r.midi == 69);
    assert(r.cents > 40.0f && r.cents < 50.0f);
    assert(!r.expectedMatch);
    assert(!r.octaveAbove && !r.octaveBelow);
  }

  // La meme note a 20 cents reste un match : la tolerance n'est pas absurde.
  {
    PitchDetector det;
    det.setExpectedMidiNote(69);
    const float hz = PitchMath::midiToHz(69) * powf(2.0f, 20.0f / 1200.0f);
    audiosig::pureTone(buf.data(), kFrame, hz, 0.4f, kFs);
    PitchResult r = det.analyse(buf.data(), kFrame);
    assert(r.valid && r.midi == 69 && r.expectedMatch);
  }

  // Note attendue invalide : ignoree proprement, pas de comportement bizarre.
  {
    PitchDetector det;
    det.setExpectedMidiNote(0);    assert(!det.hasExpectedNote());
    det.setExpectedMidiNote(-5);   assert(!det.hasExpectedNote());
    det.setExpectedMidiNote(200);  assert(!det.hasExpectedNote());
    det.setExpectedMidiNote(60);   assert(det.hasExpectedNote());
    det.clearExpectedMidiNote();   assert(!det.hasExpectedNote());
  }
}

// Un spectre ou l'harmonique 2 DOMINE la fondamentale est le piege classique de
// l'erreur d'octave. Sans note attendue le detecteur peut se tromper ; avec
// elle, il doit trancher correctement.
void pitch_expected_note_helps_on_dominant_h2() {
  std::vector<float> buf(kFrame);
  ToneSpec s;
  s.sampleRate = kFs;
  s.f0 = PitchMath::midiToHz(62);   // re4, ~293,66 Hz
  s.amp = 0.10f;                    // fondamentale FAIBLE
  s.h2 = 0.50f;                     // harmonique 2 dominante
  s.h3 = 0.15f;
  audiosig::fill(buf.data(), kFrame, s);

  PitchDetector det;
  det.setExpectedMidiNote(62);
  PitchResult r = det.analyse(buf.data(), kFrame);
  assert(r.valid);
  assert(r.midi == 62);           // la fondamentale, pas l'harmonique
  assert(r.expectedMatch);
}

// ---------------------------------------------------------------------------
// PHASE 2.3 - Stabilite
// ---------------------------------------------------------------------------

void pitch_stability_tracking() {
  std::vector<float> buf(kFrame);

  // Note parfaitement tenue : la stabilite monte a 1 une fois l'historique plein.
  {
    PitchDetector det;
    PitchResult r;
    for (int i = 0; i < MIC_PITCH_HISTORY - 1; i++) {
      audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.4f, kFs, (size_t)i * kFrame);
      r = det.detect(buf.data(), kFrame);
      assert(r.stability == 0.0f);   // historique incomplet : 0, pas une valeur inventee
    }
    audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.4f, kFs,
                       (size_t)(MIC_PITCH_HISTORY - 1) * kFrame);
    r = det.detect(buf.data(), kFrame);
    assert(r.stability > 0.99f);
  }

  // Pitch qui derive franchement : la stabilite s'effondre.
  {
    PitchDetector det;
    PitchResult r;
    for (int i = 0; i < MIC_PITCH_HISTORY; i++) {
      const float hz = PitchMath::midiToHz(69) * powf(2.0f, (float)(i * 20) / 1200.0f);
      audiosig::pureTone(buf.data(), kFrame, hz, 0.4f, kFs, (size_t)i * kFrame);
      r = det.detect(buf.data(), kFrame);
    }
    assert(r.stability < 0.1f);   // dispersion >= 140 cents
  }

  // resetTracking() vide bien l'historique.
  {
    PitchDetector det;
    for (int i = 0; i < MIC_PITCH_HISTORY; i++) {
      audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.4f, kFs, (size_t)i * kFrame);
      det.detect(buf.data(), kFrame);
    }
    det.resetTracking();
    audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.4f, kFs);
    PitchResult r = det.detect(buf.data(), kFrame);
    assert(r.stability == 0.0f);
  }

  // Une mesure NON VALIDE n'entre pas dans l'historique : elle ferait paraitre
  // instable une note qui ne l'est pas.
  {
    PitchDetector det;
    for (int i = 0; i < MIC_PITCH_HISTORY; i++) {
      audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.4f, kFs, (size_t)i * kFrame);
      det.detect(buf.data(), kFrame);
    }
    audiosig::whiteNoise(buf.data(), kFrame, 0.4f);
    PitchResult noise = det.detect(buf.data(), kFrame);
    assert(!noise.valid);
    audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.4f, kFs);
    PitchResult back = det.detect(buf.data(), kFrame);
    assert(back.stability > 0.99f);   // l'historique n'a pas ete pollue
  }
}


// ---------------------------------------------------------------------------
// PHASE 3.1 - Goertzel : energie a une frequence exacte
// ---------------------------------------------------------------------------

void spectral_goertzel_isolates_a_frequency() {
  std::vector<float> buf(kFrame);

  // Sinus pur : toute l'energie est a f0, presque rien ailleurs.
  audiosig::pureTone(buf.data(), kFrame, 1000.0f, 0.5f, kFs);
  const float atF0 = SpectralAnalyzer::goertzelPower(buf.data(), kFrame, 1000.0f, kFs);
  const float atOther = SpectralAnalyzer::goertzelPower(buf.data(), kFrame, 1500.0f, kFs);
  assert(atF0 > 0.0f);
  assert(atF0 > 100.0f * atOther);

  // La puissance mesuree correspond a celle d'un sinus d'amplitude A : A^2/4
  // pour la convention de ce Goertzel (energie d'une seule raie complexe).
  const float expected = 0.5f * 0.5f / 4.0f;
  assert(fabsf(atF0 - expected) / expected < 0.05f);

  // Doubler l'amplitude quadruple la puissance.
  audiosig::pureTone(buf.data(), kFrame, 1000.0f, 1.0f, kFs);
  const float louder = SpectralAnalyzer::goertzelPower(buf.data(), kFrame, 1000.0f, kFs);
  assert(fabsf(louder / atF0 - 4.0f) < 0.2f);

  // Un decalage continu ne cree pas d'energie : il est retire.
  ToneSpec dc; dc.sampleRate = kFs; dc.f0 = 1000.0f; dc.amp = 0.5f; dc.dc = 0.7f;
  audiosig::fill(buf.data(), kFrame, dc);
  const float withDc = SpectralAnalyzer::goertzelPower(buf.data(), kFrame, 1000.0f, kFs);
  assert(fabsf(withDc - atF0) / atF0 < 0.05f);

  // Silence : aucune energie nulle part.
  audiosig::silence(buf.data(), kFrame);
  assert(SpectralAnalyzer::goertzelPower(buf.data(), kFrame, 1000.0f, kFs) < 1e-9f);

  // Cibles absurdes : refus, jamais une mesure repliee.
  audiosig::pureTone(buf.data(), kFrame, 1000.0f, 0.5f, kFs);
  assert(SpectralAnalyzer::goertzelPower(buf.data(), kFrame, 0.0f, kFs) == 0.0f);
  assert(SpectralAnalyzer::goertzelPower(buf.data(), kFrame, -100.0f, kFs) == 0.0f);
  assert(SpectralAnalyzer::goertzelPower(buf.data(), kFrame, kFs * 0.5f, kFs) == 0.0f);
  assert(SpectralAnalyzer::goertzelPower(buf.data(), kFrame, 20000.0f, kFs) == 0.0f);
  assert(SpectralAnalyzer::goertzelPower(nullptr, kFrame, 1000.0f, kFs) == 0.0f);
  assert(SpectralAnalyzer::goertzelPower(buf.data(), 0, 1000.0f, kFs) == 0.0f);
}

void spectral_harmonic_ratios() {
  std::vector<float> buf(kFrame);

  // Spectre connu : f0 = 0,50 ; h2 = 0,25 ; h3 = 0,10. Les rapports de
  // PUISSANCE sont donc (0,25/0,50)^2 = 0,25 et (0,10/0,50)^2 = 0,04.
  ToneSpec s2;
  s2.sampleRate = kFs; s2.f0 = 500.0f; s2.amp = 0.50f; s2.h2 = 0.25f; s2.h3 = 0.10f;
  audiosig::fill(buf.data(), kFrame, s2);

  HarmonicEnergies h = SpectralAnalyzer::harmonics(buf.data(), kFrame, 500.0f, kFs);
  assert(h.valid);
  assert(h.measured == 4);
  assert(h.fundamental > h.h2 && h.h2 > h.h3);
  assert(fabsf(h.h2Ratio - 0.25f) < 0.02f);
  assert(fabsf(h.h3Ratio - 0.04f) < 0.01f);
  assert(h.h4Ratio < 0.01f);               // pas de 4e harmonique dans le signal
  assert(h.harmonicTotal > h.fundamental);

  // Sinus pur : les rapports harmoniques sont quasi nuls.
  audiosig::pureTone(buf.data(), kFrame, 500.0f, 0.5f, kFs);
  {
    HarmonicEnergies p = SpectralAnalyzer::harmonics(buf.data(), kFrame, 500.0f, kFs);
    assert(p.valid && p.h2Ratio < 0.01f && p.h3Ratio < 0.01f);
  }

  // Note AIGUE : toutes les harmoniques ne tiennent pas sous Nyquist. On doit
  // le dire, pas mesurer une frequence repliee.
  audiosig::pureTone(buf.data(), kFrame, 7000.0f, 0.5f, kFs);
  {
    HarmonicEnergies hi = SpectralAnalyzer::harmonics(buf.data(), kFrame, 7000.0f, kFs);
    assert(hi.valid);
    assert(hi.measured == 2);    // 7 k et 14 k passent, 21 k et 28 k non
    assert(hi.h3 == 0.0f && hi.h4 == 0.0f);
  }

  // Entrees invalides.
  assert(!SpectralAnalyzer::harmonics(buf.data(), kFrame, 0.0f, kFs).valid);
  assert(!SpectralAnalyzer::harmonics(buf.data(), kFrame, -10.0f, kFs).valid);
  assert(!SpectralAnalyzer::harmonics(nullptr, kFrame, 500.0f, kFs).valid);
  assert(!SpectralAnalyzer::harmonics(buf.data(), kFrame, 20000.0f, kFs).valid);

  // totalPower : A^2/2 pour un sinus d'amplitude A, continu exclu.
  audiosig::pureTone(buf.data(), kFrame, 500.0f, 0.5f, kFs);
  assert(fabsf(SpectralAnalyzer::totalPower(buf.data(), kFrame) - 0.125f) < 0.005f);
  audiosig::dcOnly(buf.data(), kFrame, 0.9f);
  assert(SpectralAnalyzer::totalPower(buf.data(), kFrame) < 1e-6f);
}

#if MIC_FFT_ENABLED
// ---------------------------------------------------------------------------
// PHASE 3.2 - FFT : la raie tombe au bon endroit, avec la bonne amplitude
// ---------------------------------------------------------------------------

void spectral_fft_places_the_peak_correctly() {
  SpectralAnalyzer sa;
  std::vector<float> buf(kFrame);

  assert(!sa.hasSpectrum());
  // Trop court : refus explicite.
  assert(!sa.computeSpectrum(buf.data(), 16));
  assert(!sa.computeSpectrum(nullptr, kFrame));

  // Frequence CENTREE sur un bin : le pic doit y tomber exactement.
  const float binHz = kFs / (float)MIC_FFT_SIZE;
  for (int targetBin : {8, 16, 40, 100}) {
    const float hz = (float)targetBin * binHz;
    audiosig::pureTone(buf.data(), kFrame, hz, 0.5f, kFs);
    assert(sa.computeSpectrum(buf.data(), kFrame));
    assert(sa.hasSpectrum());

    const float* mag = sa.magnitudes();
    size_t peak = 1;
    for (size_t k = 2; k < sa.binCount(); k++) if (mag[k] > mag[peak]) peak = k;
    assert((int)peak == targetBin);
    // Le pic domine largement le reste du spectre.
    assert(mag[peak] > 50.0f * mag[peak + 5]);
  }

  // binToHz est l'inverse exact de l'indexation.
  assert(fabsf(SpectralAnalyzer::binToHz(0, kFs)) < 1e-6f);
  assert(fabsf(SpectralAnalyzer::binToHz(MIC_FFT_SIZE / 2, kFs) - kFs * 0.5f) < 1e-3f);
  assert(sa.binCount() == MIC_FFT_SIZE / 2 + 1);
}

void spectral_centroid_and_flatness() {
  SpectralAnalyzer sa;
  std::vector<float> buf(kFrame);

  // Un sinus grave a un centre de gravite bas, un sinus aigu un centre haut.
  audiosig::pureTone(buf.data(), kFrame, 500.0f, 0.5f, kFs);
  sa.computeSpectrum(buf.data(), kFrame);
  const float lowCentroid = sa.spectralCentroid(kFs);
  const float lowFlatness = sa.spectralFlatness();

  audiosig::pureTone(buf.data(), kFrame, 5000.0f, 0.5f, kFs);
  sa.computeSpectrum(buf.data(), kFrame);
  const float highCentroid = sa.spectralCentroid(kFs);

  assert(highCentroid > lowCentroid * 3.0f);
  // Le centre de gravite d'un sinus pur est proche de sa frequence.
  assert(fabsf(lowCentroid - 500.0f) < 400.0f);
  assert(fabsf(highCentroid - 5000.0f) < 800.0f);

  // Platitude : un spectre a raies est PEU plat, un bruit blanc l'est beaucoup.
  audiosig::whiteNoise(buf.data(), kFrame, 0.4f);
  sa.computeSpectrum(buf.data(), kFrame);
  const float noiseFlatness = sa.spectralFlatness();
  assert(noiseFlatness > lowFlatness * 3.0f);
  assert(lowFlatness >= 0.0f && lowFlatness <= 1.0f);
  assert(noiseFlatness > 0.0f && noiseFlatness <= 1.0f);

  // Une note harmonique riche se situe entre les deux : plus plate qu'un sinus,
  // bien moins qu'un bruit.
  audiosig::fluteLike(buf.data(), kFrame, 800.0f, 0.4f, 0.01f, kFs);
  sa.computeSpectrum(buf.data(), kFrame);
  const float toneFlatness = sa.spectralFlatness();
  assert(toneFlatness < noiseFlatness);

  // Energie par bande : concentree autour de la raie.
  audiosig::pureTone(buf.data(), kFrame, 2000.0f, 0.5f, kFs);
  sa.computeSpectrum(buf.data(), kFrame);
  const float inBand = sa.bandEnergy(1800.0f, 2200.0f, kFs);
  const float outBand = sa.bandEnergy(4000.0f, 8000.0f, kFs);
  assert(inBand > 100.0f * outBand);
  assert(sa.bandEnergy(2200.0f, 1800.0f, kFs) == 0.0f);   // bande inversee
}

// Sans spectre calcule, aucun descripteur ne doit inventer de valeur.
void spectral_accessors_are_safe_without_spectrum() {
  SpectralAnalyzer sa;
  assert(!sa.hasSpectrum());
  assert(sa.spectralCentroid(kFs) == 0.0f);
  assert(sa.spectralFlatness() == 0.0f);
  assert(sa.bandEnergy(100.0f, 1000.0f, kFs) == 0.0f);
}

// La FFT doit rester coherente avec Goertzel : deux methodes independantes qui
// mesurent la meme chose doivent s'accorder.
void spectral_fft_agrees_with_goertzel() {
  SpectralAnalyzer sa;
  std::vector<float> buf(kFrame);
  const float binHz = kFs / (float)MIC_FFT_SIZE;

  ToneSpec s3;
  s3.sampleRate = kFs;
  s3.f0 = 16.0f * binHz;         // centree sur un bin : pas de fuite
  s3.amp = 0.5f;
  s3.h2 = 0.25f;
  audiosig::fill(buf.data(), kFrame, s3);

  assert(sa.computeSpectrum(buf.data(), kFrame));
  const float* mag = sa.magnitudes();

  // Rapport d'amplitude h2/f0 vu par la FFT : 0,25/0,50 = 0,5.
  const float fftRatio = mag[32] / mag[16];
  assert(fabsf(fftRatio - 0.5f) < 0.05f);

  // Goertzel mesure des PUISSANCES : le rapport doit etre le carre.
  HarmonicEnergies h = SpectralAnalyzer::harmonics(buf.data(), kFrame, s3.f0, kFs);
  assert(fabsf(h.h2Ratio - fftRatio * fftRatio) < 0.05f);
}
#endif  // MIC_FFT_ENABLED


// ---------------------------------------------------------------------------
// PHASE 4 - Assemblage des descripteurs acoustiques
// ---------------------------------------------------------------------------

void features_defaults_are_honest() {
  AcousticFeatures f;
  // Tant que rien n'a ete mesure, aucun champ ne doit suggerer une mesure.
  assert(f.frameSequence == 0 && f.timestamp == 0);
  assert(f.rms == 0.0f);
  assert(f.rmsDbFS == MIC_DBFS_FLOOR);      // plancher, pas 0 dBFS
  assert(f.peakDbFS == MIC_DBFS_FLOOR);
  assert(!f.clipping && f.clippingRatio == 0.0f);
  assert(f.pitchHz == 0.0f && f.pitchMidi == 0);
  assert(f.pitchConfidence == 0.0f && f.pitchStability == 0.0f);
  assert(!f.spectralValid);
  assert(!f.soundDetected && !f.expectedNoteDetected && !f.overblowDetected);

  f.rms = 0.5f; f.spectralValid = true; f.overblowDetected = true;
  f.reset();
  assert(f.rms == 0.0f && !f.spectralValid && !f.overblowDetected);
  assert(f.rmsDbFS == MIC_DBFS_FLOOR);
}

void features_level_and_pitch_assembly() {
  std::vector<float> buf(kFrame);
  AcousticFeatures f;

  audiosig::pureTone(buf.data(), kFrame, 440.0f, 0.5f, kFs);
  const FrameLevel lvl = AudioLevel::compute(buf.data(), kFrame);
  AcousticFeatureBuilder::fillLevel(f, lvl);
  assert(fabsf(f.rmsDbFS - (-9.03f)) < 0.1f);
  assert(fabsf(f.peakDbFS - (-6.02f)) < 0.1f);
  assert(!f.clipping);

  PitchDetector det;
  det.setExpectedMidiNote(69);
  PitchResult p = det.analyse(buf.data(), kFrame);
  AcousticFeatureBuilder::fillPitch(f, p, true);
  assert(f.pitchMidi == 69);
  assert(fabsf(f.cents) < 1.0f);
  assert(f.soundDetected);
  assert(f.expectedNoteDetected);
  assert(!f.overblowDetected);

  // Overblow : la note est bien signalee comme octave superieure.
  audiosig::pureTone(buf.data(), kFrame, PitchMath::midiToHz(81), 0.5f, kFs);
  PitchResult over = det.analyse(buf.data(), kFrame);
  AcousticFeatureBuilder::fillPitch(f, over, true);
  assert(f.pitchMidi == 81);
  assert(f.overblowDetected);
  assert(!f.expectedNoteDetected);

  // Un pitch NON VALIDE ne doit produire aucun verdict : annoncer un overblow
  // sur du bruit serait pire que ne rien annoncer.
  PitchResult bogus;
  bogus.valid = false;
  bogus.midi = 81;
  bogus.octaveAbove = true;
  bogus.expectedMatch = true;
  AcousticFeatureBuilder::fillPitch(f, bogus, false);
  assert(!f.overblowDetected);
  assert(!f.expectedNoteDetected);
  assert(!f.soundDetected);
}

void features_spectral_assembly() {
  std::vector<float> buf(kFrame);
  SpectralAnalyzer sa;
  AcousticFeatures f;

  // Note harmonique connue : f0 = 0,50 ; h2 = 0,25 -> rapport de puissance 0,25.
  ToneSpec s4;
  s4.sampleRate = kFs; s4.f0 = 500.0f; s4.amp = 0.50f; s4.h2 = 0.25f; s4.h3 = 0.10f;
  audiosig::fill(buf.data(), kFrame, s4);

  AcousticFeatureBuilder::fillSpectral(f, buf.data(), kFrame, 500.0f, &sa, true);
  assert(f.spectralValid);
  assert(f.fundamentalEnergy > 0.0f);
  assert(fabsf(f.h2Ratio - 0.25f) < 0.02f);
  assert(fabsf(f.h3Ratio - 0.04f) < 0.01f);
#if MIC_FFT_ENABLED
  assert(f.spectralCentroid > 400.0f && f.spectralCentroid < 3000.0f);
  assert(f.spectralFlatness > 0.0f && f.spectralFlatness <= 1.0f);
#endif

  // Un signal presque purement harmonique a un rapport harmonique/bruit eleve ;
  // ajouter du souffle le fait chuter. C'est le comportement attendu, et c'est
  // ce qui servira plus tard a mesurer la respiration.
  const float cleanHnr = f.harmonicToNoiseRatio;
  assert(cleanHnr > 10.0f);

  ToneSpec breathy = s4;
  breathy.noise = 0.35f;
  audiosig::fill(buf.data(), kFrame, breathy);
  AcousticFeatures g;
  AcousticFeatureBuilder::fillSpectral(g, buf.data(), kFrame, 500.0f, &sa, true);
  assert(g.spectralValid);
  assert(g.harmonicToNoiseRatio < cleanHnr - 6.0f);

  // Le rapport est BORNE : un signal synthetique parfait donnerait un infini.
  ToneSpec pure;
  pure.sampleRate = kFs; pure.f0 = 500.0f; pure.amp = 0.5f;
  audiosig::fill(buf.data(), kFrame, pure);
  AcousticFeatures h;
  AcousticFeatureBuilder::fillSpectral(h, buf.data(), kFrame, 500.0f, &sa, true);
  assert(h.harmonicToNoiseRatio <= MIC_HNR_MAX_DB);
  assert(h.harmonicToNoiseRatio >= -MIC_HNR_MAX_DB);

  // Sans fondamentale fiable, RIEN n'est renseigne et spectralValid reste faux.
  AcousticFeatures none;
  none.h2Ratio = 9.0f; none.spectralValid = true;      // valeurs a effacer
  AcousticFeatureBuilder::fillSpectral(none, buf.data(), kFrame, 0.0f, &sa, true);
  assert(!none.spectralValid);
  assert(none.h2Ratio == 0.0f && none.fundamentalEnergy == 0.0f);
  assert(none.harmonicToNoiseRatio == 0.0f);

  AcousticFeatureBuilder::fillSpectral(none, nullptr, 0, 500.0f, &sa, true);
  assert(!none.spectralValid);

  // Sans analyseur FFT, Goertzel fonctionne quand meme : la FFT est optionnelle.
  AcousticFeatures goertzelOnly;
  audiosig::fill(buf.data(), kFrame, s4);
  AcousticFeatureBuilder::fillSpectral(goertzelOnly, buf.data(), kFrame, 500.0f, nullptr, false);
  assert(goertzelOnly.spectralValid);
  assert(fabsf(goertzelOnly.h2Ratio - 0.25f) < 0.02f);
  assert(goertzelOnly.spectralCentroid == 0.0f);   // non mesure, donc non invente
}

// Chaine complete sur une frame : acquisition -> niveau -> pitch -> spectre.
// C'est le scenario que le firmware execute reellement, avec les memes
// composants, a l'I2S pres.
void features_end_to_end_on_one_frame() {
  AudioRingBuffer ring;
  AudioCaptureStats st;
  SpectralAnalyzer sa;
  PitchDetector det;
  AcousticFeatures f;

  // Une note de flute plausible a 587,33 Hz (re5), avec un peu de souffle.
  std::vector<float> chunk(MIC_I2S_CHUNK_SAMPLES);
  std::vector<float> frame(MIC_ANALYSIS_FRAME_SIZE);
  det.setExpectedMidiNote(74);

  size_t produced = 0;
  bool got = false;
  for (int i = 0; i < 20 && !got; i++) {
    audiosig::fluteLike(chunk.data(), chunk.size(), 587.33f, 0.35f, 0.01f, kFs, produced);
    produced += chunk.size();
    ring.write(chunk.data(), chunk.size(), &st);
    got = ring.readFrame(frame.data(), MIC_ANALYSIS_FRAME_SIZE, MIC_ANALYSIS_HOP_SIZE, &st);
  }
  assert(got);
  assert(st.droppedSamples == 0);

  const FrameLevel lvl = AudioLevel::compute(frame.data(), MIC_ANALYSIS_FRAME_SIZE);
  PitchResult p = det.detect(frame.data(), MIC_ANALYSIS_FRAME_SIZE);
  AcousticFeatureBuilder::fillLevel(f, lvl);
  AcousticFeatureBuilder::fillPitch(f, p, lvl.rms > MIC_RMS_THRESHOLD);
  AcousticFeatureBuilder::fillSpectral(f, frame.data(), MIC_ANALYSIS_FRAME_SIZE,
                                       p.hz, &sa, true);

  // La note est identifiee, juste, et reconnue comme celle attendue.
  assert(f.pitchMidi == 74);
  assert(fabsf(f.cents) < 5.0f);
  assert(f.expectedNoteDetected);
  assert(!f.overblowDetected);
  assert(f.soundDetected);
  assert(!f.clipping);
  // Niveau coherent : ni silence ni saturation.
  assert(f.rmsDbFS > -30.0f && f.rmsDbFS < 0.0f);
  // Le spectre reflete bien le modele : H2 a 30 % d'AMPLITUDE, donc ~9 % de
  // PUISSANCE, et un rapport harmonique/bruit franchement positif.
  assert(f.spectralValid);
  assert(f.h2Ratio > 0.03f && f.h2Ratio < 0.20f);
  assert(f.harmonicToNoiseRatio > 5.0f);
}

}  // namespace

void audio_run_all_tests() {
  ref_pitch_pure_tones_in_range();
  ref_pitch_degraded_inputs();
  ref_pitch_saturated_and_harmonic();
  pitch_cents_accuracy_is_sub_cent();
  pitch_window_ab_comparison();
  pitch_analysis_is_non_destructive();
  ref_pitch_octave_relationships();
  ref_rms_reference_values();
  ref_raw_signal_classification();
  ref_signal_generator_is_reproducible();
  ring_never_produces_a_partial_frame();
  ring_frames_overlap_by_hop();
  ring_overrun_drops_oldest_and_counts();
  ring_wraps_without_corrupting_a_frame();
  ring_steady_state_does_not_drift();
  level_rms_peak_and_dbfs();
  level_clipping_detection();
  pitch_expected_note_resolves_octave();
  pitch_expected_note_helps_on_dominant_h2();
  pitch_stability_tracking();
  spectral_goertzel_isolates_a_frequency();
  spectral_harmonic_ratios();
#if MIC_FFT_ENABLED
  spectral_fft_places_the_peak_correctly();
  spectral_centroid_and_flatness();
  spectral_accessors_are_safe_without_spectrum();
  spectral_fft_agrees_with_goertzel();
#endif
  features_defaults_are_honest();
  features_level_and_pitch_assembly();
  features_spectral_assembly();
  features_end_to_end_on_one_frame();
}
