/***********************************************************************************************
 * test_spectral_hnr.cpp - Rapport harmonique / bruit mesure sur le spectre complet
 *
 * CE QUE CES TESTS DEMONTRENT
 * ---------------------------
 * Le HNR de la PHASE 4 appelait "bruit" tout ce que quatre raies de Goertzel ne
 * captaient pas. Une note tres timbree, dont l'energie vit surtout au-dessus du
 * 4e rang, etait donc punie comme si elle etait soufflee - au point que
 * l'ancienne mesure classe une note soufflee MIEUX qu'une note timbree de meme
 * niveau. Le test hnr_separates_a_timbred_note_from_a_breathy_note mesure cette
 * inversion et montre que la nouvelle mesure la corrige.
 *
 * Les autres tests verrouillent les trois decisions qui la rendent possible :
 * la largeur de fenetre autour d'une raie (lobe principal de Hann), la mediane
 * comme estimateur de plancher, et le refus explicite de mesurer quand les
 * conditions ne sont pas reunies.
 *
 * Tous les signaux sont synthetiques et reproductibles : cela valide le
 * TRAITEMENT DU SIGNAL, jamais le comportement acoustique reel d'une flute.
 ***********************************************************************************************/
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "settings.h"
#include "SpectralAnalyzer.h"
#include "audio_signals.h"

void spectral_hnr_run_all_tests();

namespace {

constexpr int kFrame = MIC_BUFFER_SIZE;
constexpr float kFs = (float)MIC_SAMPLE_RATE;

#if MIC_FFT_ENABLED

// ---------------------------------------------------------------------------
// Generateurs propres a ce fichier
//
// audio_signals.h s'arrete a la 4e harmonique, ce qui est precisement le point
// aveugle que l'on veut demontrer : il faut H5..H8 pour construire une note
// dont l'energie vit au-dessus de ce que Goertzel mesure. Le generateur reste
// ici plutot que dans audio_signals.h, qui est partage par plusieurs chantiers.
// ---------------------------------------------------------------------------

constexpr int kMaxPartialsInSpec = 8;

struct RichTone {
  float f0 = 0.0f;
  // Amplitudes ABSOLUES de H1..H8. Un zero ne contribue pas.
  float partial[kMaxPartialsInSpec] = {};
  float noise = 0.0f;                  // amplitude du bruit blanc
  uint32_t seed = 4242u;
};

void fillRich(float* buf, size_t n, const RichTone& s) {
  audiosig::Lcg rng(s.seed);
  for (size_t i = 0; i < n; i++) {
    const float t = (float)i / kFs;
    float v = 0.0f;
    for (int k = 0; k < kMaxPartialsInSpec; k++) {
      if (s.partial[k] == 0.0f) continue;
      v += s.partial[k] * sinf(2.0f * audiosig::kPi * s.f0 * (float)(k + 1) * t);
    }
    if (s.noise > 0.0f) v += s.noise * rng.bipolar();
    buf[i] = v;
  }
}

// Reproduction EXACTE de l'approximation PHASE 4 (AcousticFeatures::fillSpectral).
// Elle vit ici, et non derriere un appel a fillSpectral, pour que la comparaison
// "avant / apres" reste valable meme quand le firmware aura bascule sur la
// nouvelle mesure : un test de non-regression ne doit pas mesurer sa reference
// a travers le code qu'il surveille.
float legacyHnrDb(const float* x, size_t n, float f0) {
  const HarmonicEnergies h = SpectralAnalyzer::harmonics(x, n, f0, kFs);
  if (!h.valid) return 0.0f;
  const float total = SpectralAnalyzer::totalPower(x, n);
  const float harmonic = 2.0f * h.harmonicTotal;
  const float residual = (total > harmonic) ? (total - harmonic) : 0.0f;
  float db = 0.0f;
  if (residual > 1e-12f && harmonic > 0.0f) {
    db = 10.0f * log10f(harmonic / residual);
  } else if (harmonic > 0.0f) {
    db = MIC_HNR_MAX_DB;
  }
  if (db > MIC_HNR_MAX_DB) db = MIC_HNR_MAX_DB;
  if (db < -MIC_HNR_MAX_DB) db = -MIC_HNR_MAX_DB;
  return db;
}

// Moyenne ARITHMETIQUE des bins non harmoniques : l'estimateur naif que la
// mediane remplace. Plusieurs tests s'en servent comme temoin.
double meanNonHarmonicPower(const SpectralAnalyzer& sa, float f0) {
  double sum = 0.0;
  size_t count = 0;
  for (size_t k = 1; k < sa.binCount(); k++) {
    if (SpectralAnalyzer::isHarmonicBin(k, f0, kFs)) continue;
    const double m = (double)sa.magnitudes()[k];
    sum += m * m;
    count++;
  }
  return count ? sum / (double)count : 0.0;
}

// Rapport entre la somme des |X[k]|^2 unilaterale et la puissance temporelle,
// pour un bruit large bande. La fenetre de Hann ne restitue que la moyenne de
// w^2, soit 3/8, et le spectre unilateral en porte la moitie : 3/16. C'est la
// reference contre laquelle le plancher estime est verifie EN VALEUR ABSOLUE,
// et pas seulement en tendance.
constexpr float kHannOneSidedPowerFactor = 3.0f / 16.0f;

// ---------------------------------------------------------------------------
// 1 - Ce qui n'est pas mesurable est refuse, pas devine
// ---------------------------------------------------------------------------

void hnr_refuses_what_it_cannot_measure() {
  SpectralAnalyzer sa;
  std::vector<float> buf(kFrame);

  // Aucun spectre calcule : rien a mesurer, et surtout rien a inventer.
  {
    HarmonicNoiseRatio h = sa.harmonicNoiseRatio(500.0f, kFs);
    assert(!h.valid);
    assert(h.db == 0.0f && h.harmonicEnergy == 0.0f && h.noiseEnergy == 0.0f);
    assert(h.noiseFloorPerBin == 0.0f);
    assert(h.harmonicBins == 0 && h.noiseBins == 0 && h.partials == 0);
  }

  // computeSpectrum refuse une frame trop courte : l'etat precedent ne doit pas
  // survivre et servir de spectre.
  audiosig::pureTone(buf.data(), kFrame, 1000.0f, 0.5f, kFs);
  assert(sa.computeSpectrum(buf.data(), kFrame));
  assert(sa.harmonicNoiseRatio(1000.0f, kFs).valid);
  assert(!sa.computeSpectrum(buf.data(), 32));
  assert(!sa.hasSpectrum());
  assert(!sa.harmonicNoiseRatio(1000.0f, kFs).valid);
  assert(!sa.computeSpectrum(nullptr, kFrame));
  assert(!sa.harmonicNoiseRatio(1000.0f, kFs).valid);

  // Spectre valide, mais f0 absurde : refus, jamais une mesure repliee.
  assert(sa.computeSpectrum(buf.data(), kFrame));
  assert(!sa.harmonicNoiseRatio(0.0f, kFs).valid);
  assert(!sa.harmonicNoiseRatio(-440.0f, kFs).valid);
  assert(!sa.harmonicNoiseRatio(kFs * 0.5f, kFs).valid);
  assert(!sa.harmonicNoiseRatio(20000.0f, kFs).valid);
  // Trop grave pour etre distinguee du continu retire (< 3 bins = 187,5 Hz).
  assert(!sa.harmonicNoiseRatio(100.0f, kFs).valid);
  assert(!sa.harmonicNoiseRatio(187.0f, kFs).valid);
  // ...mais toute la plage utile de l'instrument passe.
  assert(sa.harmonicNoiseRatio(MIC_PITCH_MIN_HZ, kFs).valid);
  assert(sa.harmonicNoiseRatio(MIC_PITCH_MAX_HZ, kFs).valid);
  // Frequence d'echantillonnage degeneree.
  assert(!sa.harmonicNoiseRatio(500.0f, 0.0f).valid);
  assert(!sa.harmonicNoiseRatio(500.0f, -32000.0f).valid);

  // Silence numerique : le rapport de deux riens n'existe pas.
  audiosig::silence(buf.data(), kFrame);
  assert(sa.computeSpectrum(buf.data(), kFrame));
  assert(!sa.harmonicNoiseRatio(500.0f, kFs).valid);

  // Un continu pur est retire par computeSpectrum : il ne reste que l'arrondi
  // du calcul, qui ne doit pas passer pour une note.
  audiosig::dcOnly(buf.data(), kFrame, 1e-6f);
  assert(sa.computeSpectrum(buf.data(), kFrame));
  assert(!sa.harmonicNoiseRatio(500.0f, kFs).valid);

  // Un continu FORT laisse une fuite mesurable dans le premier bin. La mesure
  // devient possible, et sa reponse doit etre "aucun contenu harmonique a
  // 500 Hz", pas un rapport flatteur.
  audiosig::dcOnly(buf.data(), kFrame, 0.8f);
  assert(sa.computeSpectrum(buf.data(), kFrame));
  {
    HarmonicNoiseRatio h = sa.harmonicNoiseRatio(500.0f, kFs);
    assert(!h.valid || h.db < -20.0f);
  }
}

// ---------------------------------------------------------------------------
// 2 - Attribution des bins : le lobe principal de Hann, pas un bin isole
// ---------------------------------------------------------------------------

void hnr_harmonic_bins_cover_the_hann_main_lobe() {
  // Grille : 32 kHz sur 512 points = 62,5 Hz par bin.
  const float binHz = kFs / (float)MIC_FFT_SIZE;
  assert(fabsf(binHz - 62.5f) < 1e-3f);

  // hzToBin et binToHz sont inverses l'un de l'autre.
  for (float hz : {62.5f, 500.0f, 1234.5f, 8000.0f}) {
    const float b = SpectralAnalyzer::hzToBin(hz, kFs);
    assert(fabsf(SpectralAnalyzer::binToHz((size_t)(b + 0.5f), kFs) -
                 (float)((size_t)(b + 0.5f)) * binHz) < 1e-3f);
  }
  assert(fabsf(SpectralAnalyzer::hzToBin(500.0f, kFs) - 8.0f) < 1e-4f);

  // f0 CENTREE sur un bin : la raie occupe le bin et ses deux voisins, et les
  // zeros du lobe principal a +-2 bins sont inclus (ils valent zero, mais les
  // exclure rendrait la fenetre dependante de l'alignement).
  const float f0 = 500.0f;            // bin 8
  for (size_t k = 6; k <= 10; k++) assert(SpectralAnalyzer::isHarmonicBin(k, f0, kFs));
  assert(!SpectralAnalyzer::isHarmonicBin(5, f0, kFs));
  assert(!SpectralAnalyzer::isHarmonicBin(11, f0, kFs));
  // 2e harmonique : bin 16.
  for (size_t k = 14; k <= 18; k++) assert(SpectralAnalyzer::isHarmonicBin(k, f0, kFs));
  assert(!SpectralAnalyzer::isHarmonicBin(13, f0, kFs));
  assert(!SpectralAnalyzer::isHarmonicBin(19, f0, kFs));
  // Le continu n'appartient jamais a une raie.
  assert(!SpectralAnalyzer::isHarmonicBin(0, f0, kFs));
  // Hors du spectre.
  assert(!SpectralAnalyzer::isHarmonicBin(MIC_FFT_SIZE / 2 + 1, f0, kFs));
  assert(!SpectralAnalyzer::isHarmonicBin(100000, f0, kFs));

  // f0 A MI-CHEMIN entre deux bins : la fenetre doit suivre la raie reelle, pas
  // un bin arrondi. 531,25 Hz = 8,5 bins -> bins 7..10 (|k - 8,5| <= 2).
  const float half = 531.25f;
  assert(fabsf(SpectralAnalyzer::hzToBin(half, kFs) - 8.5f) < 1e-4f);
  for (size_t k = 7; k <= 10; k++) assert(SpectralAnalyzer::isHarmonicBin(k, half, kFs));
  assert(!SpectralAnalyzer::isHarmonicBin(6, half, kFs));
  assert(!SpectralAnalyzer::isHarmonicBin(11, half, kFs));

  // Un bin loin de toute raie n'est jamais harmonique.
  for (size_t k : {12u, 13u, 20u, 21u, 100u, 200u, 255u}) {
    if (k % 8 <= 2 || k % 8 >= 6) continue;    // eviter les multiples de 8
    assert(!SpectralAnalyzer::isHarmonicBin(k, f0, kFs));
  }

  // Rangs exploitables : ceux dont la raie tient sous Nyquist, plafonnes.
  assert(SpectralAnalyzer::usablePartials(0.0f, kFs) == 0);
  assert(SpectralAnalyzer::usablePartials(-1.0f, kFs) == 0);
  assert(SpectralAnalyzer::usablePartials(100.0f, kFs) == 0);     // < 3 bins
  assert(SpectralAnalyzer::usablePartials(20000.0f, kFs) == 0);   // > Nyquist
  assert(SpectralAnalyzer::usablePartials(500.0f, kFs) == SpectralHnr::kMaxPartials);
  assert(SpectralAnalyzer::usablePartials(4000.0f, kFs) == 4);    // 4, 8, 12, 16 kHz
  assert(SpectralAnalyzer::usablePartials(5500.0f, kFs) == 2);
  assert(SpectralAnalyzer::usablePartials(15000.0f, kFs) == 1);

  // Le decoupage partitionne le spectre : chaque bin analyse est soit une raie,
  // soit du bruit, jamais les deux ni aucun des deux.
  SpectralAnalyzer sa;
  std::vector<float> buf(kFrame);
  audiosig::fluteLike(buf.data(), kFrame, f0, 0.4f, 0.02f, kFs);
  assert(sa.computeSpectrum(buf.data(), kFrame));
  HarmonicNoiseRatio h = sa.harmonicNoiseRatio(f0, kFs);
  assert(h.valid);
  assert((size_t)(h.harmonicBins + h.noiseBins) == sa.binCount() - 1);
  size_t counted = 0;
  for (size_t k = 1; k < sa.binCount(); k++) {
    if (SpectralAnalyzer::isHarmonicBin(k, f0, kFs)) counted++;
  }
  assert(counted == h.harmonicBins);
  // Il reste largement de quoi estimer un plancher, y compris sur la note la
  // plus grave de la plage - le cas ou les raies sont les plus serrees.
  assert(h.noiseBins >= SpectralHnr::kMinNoiseBins);
  HarmonicNoiseRatio low = sa.harmonicNoiseRatio(MIC_PITCH_MIN_HZ, kFs);
  assert(low.noiseBins >= SpectralHnr::kMinNoiseBins);
}

// ---------------------------------------------------------------------------
// 3 - Le plancher est une mediane, et il est JUSTE
// ---------------------------------------------------------------------------

void hnr_noise_floor_is_a_robust_median() {
  SpectralAnalyzer sa;
  std::vector<float> buf(kFrame);

  // (a) Justesse absolue. Sur du bruit blanc seul, l'energie de bruit estimee
  //     doit retrouver la puissance temporelle, au facteur de fenetre pres.
  for (uint32_t seed : {12345u, 777u, 20260921u}) {
    for (float amp : {0.05f, 0.2f, 0.4f}) {
      audiosig::whiteNoise(buf.data(), kFrame, amp, seed);
      assert(sa.computeSpectrum(buf.data(), kFrame));
      HarmonicNoiseRatio h = sa.harmonicNoiseRatio(1000.0f, kFs);
      assert(h.valid);
      const float expected =
          kHannOneSidedPowerFactor * SpectralAnalyzer::totalPower(buf.data(), MIC_FFT_SIZE);
      const float err = fabsf(h.noiseEnergy - expected) / expected;
      assert(err < 0.15f);
    }
  }

  // (b) Le plancher suit la puissance injectee : x2 en amplitude = x4 en
  //     puissance. Un plancher fige passerait (a) par hasard mais pas ceci.
  float previous = 0.0f;
  for (float amp : {0.05f, 0.1f, 0.2f, 0.4f}) {
    audiosig::whiteNoise(buf.data(), kFrame, amp, 12345u);
    assert(sa.computeSpectrum(buf.data(), kFrame));
    HarmonicNoiseRatio h = sa.harmonicNoiseRatio(1000.0f, kFs);
    assert(h.valid && h.noiseFloorPerBin > 0.0f);
    if (previous > 0.0f) {
      const float ratio = h.noiseFloorPerBin / previous;
      assert(ratio > 3.5f && ratio < 4.5f);
    }
    previous = h.noiseFloorPerBin;
  }

  // (c) ROBUSTESSE, le point qui justifie la mediane. On ajoute une raie
  //     parasite forte a 3350 Hz, qui n'est harmonique de rien : une moyenne
  //     arithmetique s'effondrerait, la mediane ne bouge pas.
  RichTone tone;
  tone.f0 = 500.0f;
  tone.partial[0] = 0.5f; tone.partial[1] = 0.2f;
  tone.noise = 0.02f;
  fillRich(buf.data(), kFrame, tone);
  assert(sa.computeSpectrum(buf.data(), kFrame));
  const HarmonicNoiseRatio clean = sa.harmonicNoiseRatio(tone.f0, kFs);
  assert(clean.valid);

  for (size_t i = 0; i < (size_t)kFrame; i++) {
    buf[i] += 0.30f * sinf(2.0f * audiosig::kPi * 3350.0f * (float)i / kFs);
  }
  assert(sa.computeSpectrum(buf.data(), kFrame));
  const HarmonicNoiseRatio spoiled = sa.harmonicNoiseRatio(tone.f0, kFs);
  assert(spoiled.valid);

  const double naiveMean = meanNonHarmonicPower(sa, tone.f0);
  // La moyenne est tiree deux ordres de grandeur au-dessus du vrai plancher...
  assert(naiveMean > 50.0 * (double)spoiled.noiseFloorPerBin);
  // ...alors que la mediane bouge de moins de 30 %.
  const float drift = spoiled.noiseFloorPerBin / clean.noiseFloorPerBin;
  assert(drift > 0.7f && drift < 1.3f);
  // Et donc le HNR lui-meme tient, a moins de 1 dB pres.
  assert(fabsf(spoiled.db - clean.db) < 1.0f);
  // Ce qu'un plancher moyenne aurait coute, en dB :
  const float meanBasedDb =
      clean.db - 10.0f * log10f((float)(naiveMean / (double)spoiled.noiseFloorPerBin));
  assert(meanBasedDb < clean.db - 15.0f);

  printf("  [hnr] plancher : mediane=%.3e  moyenne=%.3e  (x%.0f)  "
         "db mediane=%+.2f  db si moyenne=%+.2f\n",
         (double)spoiled.noiseFloorPerBin, naiveMean,
         naiveMean / (double)spoiled.noiseFloorPerBin, (double)spoiled.db,
         (double)meanBasedDb);
}

// ---------------------------------------------------------------------------
// 4 - Les deux extremes : sinus pur et bruit blanc pur
// ---------------------------------------------------------------------------

void hnr_pure_tone_and_pure_noise_sit_at_the_extremes() {
  SpectralAnalyzer sa;
  std::vector<float> buf(kFrame);

  // Sinus pur : aucun bruit a mesurer, le rapport est infini. On BORNE plutot
  // que de rendre l'infini, et la borne est celle du firmware.
  for (float hz : {500.0f, 1000.0f, 2437.5f}) {
    audiosig::pureTone(buf.data(), kFrame, hz, 0.5f, kFs);
    assert(sa.computeSpectrum(buf.data(), kFrame));
    HarmonicNoiseRatio h = sa.harmonicNoiseRatio(hz, kFs);
    assert(h.valid);
    assert(h.db == MIC_HNR_MAX_DB);
    assert(h.harmonicEnergy > 0.0f);
  }

  // Le niveau ne change pas un RAPPORT : le meme sinus 20 dB plus bas donne le
  // meme HNR.
  audiosig::pureTone(buf.data(), kFrame, 1000.0f, 0.05f, kFs);
  assert(sa.computeSpectrum(buf.data(), kFrame));
  assert(sa.harmonicNoiseRatio(1000.0f, kFs).db == MIC_HNR_MAX_DB);

  // Bruit blanc pur : aucun contenu harmonique, quelle que soit la f0 qu'on
  // pretend y chercher.
  for (uint32_t seed : {12345u, 999u}) {
    audiosig::whiteNoise(buf.data(), kFrame, 0.4f, seed);
    assert(sa.computeSpectrum(buf.data(), kFrame));
    for (float hz : {500.0f, 880.0f, 2000.0f}) {
      HarmonicNoiseRatio h = sa.harmonicNoiseRatio(hz, kFs);
      assert(h.valid);
      // Pas exactement -MIC_HNR_MAX_DB : la somme des bins de raies fluctue
      // autour du plancher, ce qui laisse un residu statistique. Le verdict
      // reste sans ambiguite.
      assert(h.db < -10.0f);
      assert(h.db >= -MIC_HNR_MAX_DB);
    }
  }

  // Bornes respectees dans les deux sens, toujours.
  for (float amp : {0.0005f, 0.01f, 0.5f}) {
    RichTone t; t.f0 = 750.0f; t.partial[0] = 0.5f; t.noise = amp;
    fillRich(buf.data(), kFrame, t);
    assert(sa.computeSpectrum(buf.data(), kFrame));
    HarmonicNoiseRatio h = sa.harmonicNoiseRatio(t.f0, kFs);
    assert(h.valid);
    assert(h.db <= MIC_HNR_MAX_DB && h.db >= -MIC_HNR_MAX_DB);
    assert(!std::isnan(h.db) && !std::isinf(h.db));
  }
}

// ---------------------------------------------------------------------------
// 5 - Monotonie : plus de souffle, moins de HNR
// ---------------------------------------------------------------------------

void hnr_decreases_monotonically_with_breath() {
  SpectralAnalyzer sa;
  std::vector<float> buf(kFrame);

  const float kBreath[] = {0.005f, 0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.4f, 0.8f};
  float previous = MIC_HNR_MAX_DB + 1.0f;
  float first = 0.0f, last = 0.0f;
  for (size_t i = 0; i < sizeof(kBreath) / sizeof(kBreath[0]); i++) {
    RichTone t;
    t.f0 = 500.0f;
    t.partial[0] = 0.5f; t.partial[1] = 0.15f; t.partial[2] = 0.06f;
    t.noise = kBreath[i];
    fillRich(buf.data(), kFrame, t);
    assert(sa.computeSpectrum(buf.data(), kFrame));
    HarmonicNoiseRatio h = sa.harmonicNoiseRatio(t.f0, kFs);
    assert(h.valid);
    // Strictement decroissant : doubler le souffle doit se voir.
    assert(h.db < previous);
    previous = h.db;
    if (i == 0) first = h.db;
    last = h.db;
  }
  // Et l'amplitude du mouvement est celle attendue : le bruit a ete multiplie
  // par 160 en amplitude, donc par 44 dB en puissance.
  assert(first - last > 30.0f);

  // Doubler le bruit doit couter environ 6 dB tant qu'on n'est pas borne.
  RichTone a;
  a.f0 = 1000.0f; a.partial[0] = 0.5f; a.partial[1] = 0.2f; a.noise = 0.05f;
  RichTone b = a; b.noise = 0.10f;
  fillRich(buf.data(), kFrame, a);
  assert(sa.computeSpectrum(buf.data(), kFrame));
  const float dbA = sa.harmonicNoiseRatio(a.f0, kFs).db;
  fillRich(buf.data(), kFrame, b);
  assert(sa.computeSpectrum(buf.data(), kFrame));
  const float dbB = sa.harmonicNoiseRatio(b.f0, kFs).db;
  assert(fabsf((dbA - dbB) - 6.02f) < 1.0f);
}

// ---------------------------------------------------------------------------
// 6 - LE test : une note timbree n'est pas une note soufflee
// ---------------------------------------------------------------------------

void hnr_separates_a_timbred_note_from_a_breathy_note() {
  SpectralAnalyzer sa;
  std::vector<float> timbred(kFrame);
  std::vector<float> breathy(kFrame);

  // Note TRES TIMBREE : fondamentale moderee, H2..H8 fortes, souffle minime.
  // C'est le cas que l'ancienne approximation punit, puisque H5..H8 tombent
  // hors des quatre raies de Goertzel et sont donc comptees comme du bruit.
  RichTone rich;
  rich.f0 = 500.0f;
  rich.partial[0] = 0.20f; rich.partial[1] = 0.28f;
  rich.partial[2] = 0.30f; rich.partial[3] = 0.30f;
  rich.partial[4] = 0.32f; rich.partial[5] = 0.30f;
  rich.partial[6] = 0.26f; rich.partial[7] = 0.22f;
  rich.noise = 0.02f;
  fillRich(timbred.data(), kFrame, rich);

  // Note SOUFFLEE : fondamentale dominante, presque pas d'harmoniques, beaucoup
  // de bruit large bande. Le niveau est ajuste pour etre comparable.
  RichTone breath;
  breath.f0 = 500.0f;
  breath.partial[0] = 0.62f; breath.partial[1] = 0.15f; breath.partial[2] = 0.07f;
  breath.noise = 0.50f;
  fillRich(breathy.data(), kFrame, breath);

  // Les deux notes sont bien au meme niveau : la comparaison ne doit rien a une
  // difference d'amplitude.
  const float rmsT = sqrtf(SpectralAnalyzer::totalPower(timbred.data(), kFrame));
  const float rmsB = sqrtf(SpectralAnalyzer::totalPower(breathy.data(), kFrame));
  const float levelGapDb = 20.0f * log10f(rmsT / rmsB);
  assert(fabsf(levelGapDb) < 1.0f);

  const float oldT = legacyHnrDb(timbred.data(), kFrame, rich.f0);
  const float oldB = legacyHnrDb(breathy.data(), kFrame, breath.f0);

  assert(sa.computeSpectrum(timbred.data(), kFrame));
  const HarmonicNoiseRatio newT = sa.harmonicNoiseRatio(rich.f0, kFs);
  assert(sa.computeSpectrum(breathy.data(), kFrame));
  const HarmonicNoiseRatio newB = sa.harmonicNoiseRatio(breath.f0, kFs);
  assert(newT.valid && newB.valid);

  printf("  [hnr] niveaux : timbree %.2f dBFS, soufflee %.2f dBFS (ecart %.2f dB)\n",
         (double)(20.0f * log10f(rmsT)), (double)(20.0f * log10f(rmsB)),
         (double)levelGapDb);
  printf("  [hnr] ANCIENNE approximation : timbree %+6.2f dB   soufflee %+6.2f dB"
         "   -> ecart %+6.2f dB\n",
         (double)oldT, (double)oldB, (double)(oldT - oldB));
  printf("  [hnr] NOUVELLE mesure        : timbree %+6.2f dB   soufflee %+6.2f dB"
         "   -> ecart %+6.2f dB\n",
         (double)newT.db, (double)newB.db, (double)(newT.db - newB.db));

  // L'ANCIENNE mesure est non seulement incapable de les separer : elle les
  // CLASSE A L'ENVERS. La note soufflee y obtient un meilleur score que la note
  // timbree. C'est exactement le defaut documente en PHASE 4.
  assert(oldT < oldB);
  assert(oldB - oldT > 2.0f);

  // La NOUVELLE mesure les separe largement, et dans le bon sens.
  assert(newT.db > newB.db);
  assert(newT.db - newB.db > 25.0f);
  assert(newT.db > 25.0f);      // la note timbree est reconnue comme propre
  assert(newB.db < 10.0f);      // la note soufflee reste mediocre

  // Et la raison est visible dans les etapes intermediaires : la note timbree a
  // un plancher trois ordres de grandeur plus bas, pour une energie harmonique
  // du meme ordre.
  assert(newT.noiseFloorPerBin < newB.noiseFloorPerBin * 0.01f);
  assert(newT.harmonicEnergy > 0.5f * newB.harmonicEnergy);

  // LE MECANISME, verrouille explicitement : les fenetres de raies doivent
  // capter la quasi-totalite de l'energie spectrale de la note timbree. C'est
  // exactement ce que l'ancienne mesure ne faisait pas - elle en laissait la
  // moitie dans le "bruit". Une fenetre trop etroite (flancs de raie perdus) ou
  // un nombre de rangs trop faible (H5..H8 perdues) font chuter cette part.
  assert(sa.computeSpectrum(timbred.data(), kFrame));
  double spectralTotal = 0.0;
  for (size_t k = 1; k < sa.binCount(); k++) {
    const double m = (double)sa.magnitudes()[k];
    spectralTotal += m * m;
  }
  const double captured = (double)newT.harmonicEnergy / spectralTotal;
  printf("  [hnr] part de l'energie spectrale captee par les fenetres de raies : "
         "%.1f %% (timbree)\n", 100.0 * captured);
  assert(captured > 0.95);

  // Verification directe du defaut : les quatre raies de Goertzel ne voient
  // qu'une fraction de l'energie de la note timbree, alors qu'elles voient
  // presque tout de la note soufflee.
  const HarmonicEnergies gT = SpectralAnalyzer::harmonics(timbred.data(), kFrame, rich.f0, kFs);
  const HarmonicEnergies gB = SpectralAnalyzer::harmonics(breathy.data(), kFrame, breath.f0, kFs);
  const float seenT = 2.0f * gT.harmonicTotal / SpectralAnalyzer::totalPower(timbred.data(), kFrame);
  const float seenB = 2.0f * gB.harmonicTotal / SpectralAnalyzer::totalPower(breathy.data(), kFrame);
  printf("  [hnr] part de la puissance vue par les 4 raies : timbree %.0f %%, "
         "soufflee %.0f %%\n", (double)(100.0f * seenT), (double)(100.0f * seenB));
  assert(seenT < 0.60f);        // plus de 40 % de la note timbree est "du bruit"
  assert(seenB > 0.60f);
}

// ---------------------------------------------------------------------------
// 7 - La mesure ne depend pas de l'alignement de f0 sur la grille FFT
// ---------------------------------------------------------------------------

void hnr_is_insensitive_to_f0_falling_between_bins() {
  SpectralAnalyzer sa;
  std::vector<float> buf(kFrame);

  // 500 Hz tombe pile sur le bin 8 ; 531,25 Hz pile entre deux bins ; les
  // autres sont quelconques. Deux erreurs seraient invisibles sur une f0 alignee
  // et se voient ici : une fenetre trop etroite laisse les flancs de la raie
  // dans le bruit, et une f0 arrondie au bin le plus proche decale les rangs
  // eleves d'autant de demi-bins qu'il y a de rangs.
  const float kTones[] = {500.0f, 517.3f, 531.25f, 546.9f, 562.5f, 583.7f};

  // Tone volontairement RICHE : c'est sur les rangs eleves qu'un decalage de
  // grille se paye, une erreur de f0 d'un demi-bin devenant N/2 bins au rang N.
  RichTone base;
  base.partial[0] = 0.40f; base.partial[1] = 0.30f; base.partial[2] = 0.24f;
  base.partial[3] = 0.20f; base.partial[4] = 0.17f; base.partial[5] = 0.15f;
  base.partial[6] = 0.13f; base.partial[7] = 0.11f;
  base.noise = 0.02f;

  float lo = MIC_HNR_MAX_DB + 1.0f, hi = -MIC_HNR_MAX_DB - 1.0f;
  float oldLo = MIC_HNR_MAX_DB + 1.0f, oldHi = -MIC_HNR_MAX_DB - 1.0f;
  for (float f0 : kTones) {
    RichTone t = base;
    t.f0 = f0;
    fillRich(buf.data(), kFrame, t);
    assert(sa.computeSpectrum(buf.data(), kFrame));
    HarmonicNoiseRatio h = sa.harmonicNoiseRatio(f0, kFs);
    assert(h.valid);
    if (h.db < lo) lo = h.db;
    if (h.db > hi) hi = h.db;

    // Les fenetres suivent les raies REELLES, quel que soit l'alignement : la
    // part d'energie captee ne doit pas dependre de f0. Arrondir f0 au bin le
    // plus proche coute un demi-bin au rang 1 mais N/2 bins au rang N, et les
    // rangs eleves sortiraient alors de leur fenetre - invisible sur le seul
    // ecart en dB, immediat ici.
    double spectral = 0.0;
    for (size_t k = 1; k < sa.binCount(); k++) {
      const double m = (double)sa.magnitudes()[k];
      spectral += m * m;
    }
    assert((double)h.harmonicEnergy / spectral > 0.95);

    const float old = legacyHnrDb(buf.data(), kFrame, f0);
    if (old < oldLo) oldLo = old;
    if (old > oldHi) oldHi = old;
  }
  // Moins de 3 dB d'ecart entre le cas le mieux aligne et le pire.
  printf("  [hnr] f0 alignee ou non sur la grille : %.2f a %.2f dB (etendue %.2f dB) ; "
         "ancienne mesure : %.2f a %.2f dB\n",
         (double)lo, (double)hi, (double)(hi - lo), (double)oldLo, (double)oldHi);
  assert(hi - lo < 3.0f);
  assert(lo > 25.0f);

  // Sur ces memes signaux - riches en harmoniques - l'ancienne approximation
  // est uniformement mauvaise : son pire cas et son meilleur cas sont tous deux
  // plus de 20 dB sous le pire cas de la nouvelle mesure. Ce n'est pas un
  // probleme d'alignement, c'est le defaut de fond.
  assert(lo - oldHi > 20.0f);
}

// ---------------------------------------------------------------------------
// 8 - Les champs exposes sont coherents entre eux
// ---------------------------------------------------------------------------

void hnr_reported_fields_are_self_consistent() {
  SpectralAnalyzer sa;
  std::vector<float> buf(kFrame);

  for (float noise : {0.01f, 0.05f, 0.2f}) {
    RichTone t;
    t.f0 = 750.0f;
    t.partial[0] = 0.4f; t.partial[1] = 0.2f; t.partial[2] = 0.12f; t.partial[3] = 0.08f;
    t.noise = noise;
    fillRich(buf.data(), kFrame, t);
    assert(sa.computeSpectrum(buf.data(), kFrame));
    HarmonicNoiseRatio h = sa.harmonicNoiseRatio(t.f0, kFs);
    assert(h.valid);

    // Le decoupage couvre exactement le spectre analyse.
    assert((size_t)(h.harmonicBins + h.noiseBins) == sa.binCount() - 1);
    assert(h.partials == SpectralAnalyzer::usablePartials(t.f0, kFs));

    // noiseEnergy est bien le plancher etendu a TOUS les bins analyses, et non
    // aux seuls bins ou il a pu etre mesure : le bruit existe aussi sous les
    // raies.
    const float expectedNoise =
        h.noiseFloorPerBin * (float)(h.harmonicBins + h.noiseBins);
    assert(fabsf(h.noiseEnergy - expectedNoise) <= 1e-6f * expectedNoise + 1e-18f);

    // harmonicEnergy est bien la somme des bins de raies du spectre expose.
    double sum = 0.0;
    for (size_t k = 1; k < sa.binCount(); k++) {
      if (!SpectralAnalyzer::isHarmonicBin(k, t.f0, kFs)) continue;
      sum += (double)sa.magnitudes()[k] * sa.magnitudes()[k];
    }
    assert(fabsf((float)sum - h.harmonicEnergy) <= 1e-5f * h.harmonicEnergy);

    // Et db se recalcule a partir des champs publies, bruit des bins de raies
    // deduit. Sans cette deduction le rapport serait flatte.
    const float net = h.harmonicEnergy - h.noiseFloorPerBin * (float)h.harmonicBins;
    assert(net > 0.0f);
    const float recomputed = 10.0f * log10f(net / h.noiseEnergy);
    assert(fabsf(recomputed - h.db) < 0.01f);

    // La deduction n'est pas cosmetique : l'ignorer changerait le resultat.
    const float without = 10.0f * log10f(h.harmonicEnergy / h.noiseEnergy);
    assert(without > h.db);
  }

  // Appeler deux fois de suite sur la meme frame donne exactement la meme
  // chose : la fonction est constante, elle ne consomme pas le spectre.
  audiosig::fluteLike(buf.data(), kFrame, 880.0f, 0.4f, 0.03f, kFs);
  assert(sa.computeSpectrum(buf.data(), kFrame));
  HarmonicNoiseRatio first = sa.harmonicNoiseRatio(880.0f, kFs);
  HarmonicNoiseRatio second = sa.harmonicNoiseRatio(880.0f, kFs);
  assert(first.valid && second.valid);
  assert(first.db == second.db);
  assert(first.noiseFloorPerBin == second.noiseFloorPerBin);
  // Et les descripteurs deja en place n'ont pas bouge.
  const float centroid = sa.spectralCentroid(kFs);
  (void)sa.harmonicNoiseRatio(880.0f, kFs);
  assert(sa.spectralCentroid(kFs) == centroid);
  assert(sa.hasSpectrum());
}

#endif  // MIC_FFT_ENABLED

// Sans FFT, la structure existe toujours et reste honnete : un appelant peut en
// detenir une sans compilation conditionnelle, elle dit simplement qu'aucune
// mesure n'a ete faite.
void hnr_result_defaults_are_honest_without_fft() {
  HarmonicNoiseRatio h;
  assert(!h.valid);
  assert(h.db == 0.0f);
  assert(h.harmonicEnergy == 0.0f && h.noiseEnergy == 0.0f);
  assert(h.noiseFloorPerBin == 0.0f);
  assert(h.harmonicBins == 0 && h.noiseBins == 0 && h.partials == 0);
}

}  // namespace

void spectral_hnr_run_all_tests() {
  hnr_result_defaults_are_honest_without_fft();
#if MIC_FFT_ENABLED
  hnr_refuses_what_it_cannot_measure();
  hnr_harmonic_bins_cover_the_hann_main_lobe();
  hnr_noise_floor_is_a_robust_median();
  hnr_pure_tone_and_pure_noise_sit_at_the_extremes();
  hnr_decreases_monotonically_with_breath();
  hnr_separates_a_timbred_note_from_a_breathy_note();
  hnr_is_insensitive_to_f0_falling_between_bins();
  hnr_reported_fields_are_self_consistent();
#else
  // Sans FFT il n'y a pas de spectre complet, donc pas de HNR spectral. Le dire
  // vaut mieux qu'une suite verte qui n'a rien execute.
  printf("  [hnr] MIC_FFT_ENABLED = 0 : mesure spectrale absente du binaire\n");
#endif
}

#ifdef STANDALONE_TEST_MAIN
#include <cstdio>
int main() { spectral_hnr_run_all_tests(); printf("spectral hnr tests passed\n"); return 0; }
#endif
