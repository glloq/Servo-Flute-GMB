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
 * Les autres tests verrouillent les quatre decisions qui la rendent possible :
 * la largeur de fenetre autour d'une raie (lobe principal de Hann), la mediane
 * comme estimateur de plancher, la BANDE sur laquelle plancher et energie sont
 * mesures, et le refus explicite de mesurer quand les conditions ne sont pas
 * reunies.
 *
 * LA CHAINE MESUREE EST CELLE DE LA PRODUCTION
 * --------------------------------------------
 * AudioAnalyzer::drainI2S() filtre le flux avant l'anneau : la FFT ne recoit
 * jamais de PCM brut. Cette suite mesurait pourtant du PCM brut, et c'est ce
 * qui avait laisse passer une extrapolation de plancher a tout le spectre alors
 * que 56 % de ce spectre est dans la bande coupee. Les tests qui portent sur la
 * mesure elle-meme passent desormais par AudioFilterChain, memoire de filtre
 * etablie avant la frame mesuree (voir ProductionStream).
 *
 * Tous les signaux sont synthetiques et reproductibles : cela valide le
 * TRAITEMENT DU SIGNAL, jamais le comportement acoustique reel d'une flute. Un
 * filtrage de production applique a du PCM synthetique reste du PCM
 * synthetique.
 ***********************************************************************************************/
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "settings.h"
#include "SpectralAnalyzer.h"
#include "AudioFilters.h"
// Pour AQ_BREATH_FLATNESS_TONE / AQ_BREATH_FLATNESS_NOISE. Ce fichier teste
// l'analyseur, pas la qualite - mais la section 13 doit verifier que les
// bornes de la respiration sont ATTEIGNABLES sur le signal que la chaine
// presente, et les recopier ici les rendrait muettes le jour ou elles
// bougeraient. On lit donc les vraies constantes.
#include "AcousticQuality.h"
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
// La BANDE, recalculee ici depuis settings.h
//
// Le HNR ne se mesure que dans la bande que AudioAnalyzer::drainI2S() laisse
// passer. Les bornes sont recalculees dans ce fichier, a la main, depuis les
// memes macros : un test qui les lirait par SpectralAnalyzer::analysisBand()
// verifierait le code avec lui-meme et passerait meme si la bande etait fausse.
// ---------------------------------------------------------------------------
constexpr size_t kLastBin = MIC_FFT_SIZE / 2;

size_t expectedBandLo() {
  if (MIC_FILTER_HP_HZ <= 0.0f) return 1;   // etage desactive : tout le spectre
  size_t k = 1;
  while (k < kLastBin && (float)k * kFs / (float)MIC_FFT_SIZE < MIC_FILTER_HP_HZ) k++;
  return k;
}
size_t expectedBandHi() {
  if (MIC_FILTER_LP_HZ <= 0.0f) return kLastBin;
  size_t k = kLastBin;
  while (k > 0 && (float)k * kFs / (float)MIC_FFT_SIZE > MIC_FILTER_LP_HZ) k--;
  return k;
}
size_t expectedBandCount() {
  const size_t lo = expectedBandLo(), hi = expectedBandHi();
  return (hi >= lo) ? (hi - lo + 1) : 0;
}

// Energie des bins de raies DANS la bande, lue sur le spectre expose.
double harmonicEnergyInBand(const SpectralAnalyzer& sa, float f0) {
  double sum = 0.0;
  for (size_t k = expectedBandLo(); k <= expectedBandHi(); k++) {
    if (!SpectralAnalyzer::isHarmonicBin(k, f0, kFs)) continue;
    sum += (double)sa.magnitudes()[k] * sa.magnitudes()[k];
  }
  return sum;
}
double spectralEnergyInBand(const SpectralAnalyzer& sa) {
  double sum = 0.0;
  for (size_t k = expectedBandLo(); k <= expectedBandHi(); k++) {
    sum += (double)sa.magnitudes()[k] * sa.magnitudes()[k];
  }
  return sum;
}

// ---------------------------------------------------------------------------
// LA CHAINE DE PRODUCTION
//
// AudioAnalyzer::drainI2S() filtre le FLUX (AudioFilterChain) avant l'anneau :
// la FFT ne voit JAMAIS le PCM brut. Un test qui mesure du PCM brut mesure une
// chaine qui n'existe pas - c'est exactement ce qui avait laisse passer
// l'extrapolation a plancher plat.
//
// Le filtre est un IIR : sa memoire doit etre ETABLIE avant la frame mesuree,
// sinon on mesure un transitoire. `frameAt()` fait donc defiler les frames
// precedentes du meme signal continu dans la MEME chaine, comme le flux reel.
// ---------------------------------------------------------------------------
struct ProductionStream {
  AudioFilterChain chain;
  ProductionStream() { chain.configureDefaults(); }

  // Remplit `out` avec la frame d'indice `index` d'une note tenue, filtree par
  // une chaine dont la memoire a ete etablie sur les `index` frames d'avant.
  void fluteFrame(float* out, size_t n, float f0, float amp, float breath, int index) {
    chain.configureDefaults();          // configure() remet la memoire a zero
    for (int i = 0; i < index; i++) {
      audiosig::fluteLike(out, n, f0, amp, breath, kFs, (size_t)i * n);
      chain.processBlock(out, n);
    }
    audiosig::fluteLike(out, n, f0, amp, breath, kFs, (size_t)index * n);
    chain.processBlock(out, n);
  }

  // Meme chose pour du bruit blanc pur. Le bruit est le signal le plus PLAT
  // qui puisse exister : c'est lui qui montre le mieux ce que la chaine fait a
  // une mesure de FORME spectrale, et c'est lui qui sert de borne haute a
  // AQ_BREATH_FLATNESS_NOISE.
  void noiseFrame(float* out, size_t n, float amp, uint32_t seed, int index) {
    chain.configureDefaults();
    for (int i = 0; i <= index; i++) {
      audiosig::whiteNoise(out, n, amp, seed + (uint32_t)i);
      chain.processBlock(out, n);
    }
  }
};

// Nombre de frames de rodage avant la frame mesuree. Quatre frames = 128 ms a
// 32 kHz, soit vingt fois la constante de temps du retrait de continu (pole
// 0,995, environ 6 ms) : le transitoire est eteint.
constexpr int kWarmFrames = 4;

// L'ANCIEN estimateur, reproduit ici : mediane des bins non harmoniques sur TOUT
// le spectre, etendue a TOUT le spectre. Il vit dans le test, et non derriere un
// appel au firmware, pour que la comparaison reste valable apres la correction -
// c'est la reference dont on doit montrer qu'elle surestime.
float flatFloorHnrDb(const SpectralAnalyzer& sa, float f0) {
  std::vector<double> p;
  double harmonic = 0.0;
  size_t harmonicBins = 0;
  for (size_t k = 1; k <= kLastBin; k++) {
    const double e = (double)sa.magnitudes()[k] * sa.magnitudes()[k];
    if (SpectralAnalyzer::isHarmonicBin(k, f0, kFs)) { harmonic += e; harmonicBins++; }
    else p.push_back(e);
  }
  if (p.empty()) return 0.0f;
  std::sort(p.begin(), p.end());
  const double floorPerBin = p[(p.size() + 1) / 2 - 1] * (double)SpectralHnr::kMedianToMeanPower;
  const double noise = floorPerBin * (double)kLastBin;
  double net = harmonic - floorPerBin * (double)harmonicBins;
  if (net <= 0.0) return -MIC_HNR_MAX_DB;
  double db = 10.0 * log10(net / noise);
  if (db > MIC_HNR_MAX_DB) db = MIC_HNR_MAX_DB;
  if (db < -MIC_HNR_MAX_DB) db = -MIC_HNR_MAX_DB;
  return (float)db;
}

// L'ANCIENNE platitude et l'ANCIEN centroide : moyenne geometrique et moyenne
// ponderee sur TOUT le spectre. Comme flatFloorHnrDb ci-dessus, ils vivent dans
// le test et non derriere un appel au firmware, pour que la comparaison reste
// valable APRES la correction. Ce sont les references dont on doit montrer
// qu'elles mesurent le filtre.
// Le plancher 1e-12 est celui de SpectralAnalyzer::spectralFlatness : la seule
// difference entre les deux fonctions doit etre le DOMAINE.
double wholeSpectrumFlatness(const SpectralAnalyzer& sa) {
  const double kFloor = 1e-12;
  double logSum = 0.0, arithSum = 0.0;
  size_t n = 0;
  for (size_t k = 1; k <= kLastBin; k++) {
    double m = sa.magnitudes()[k];
    if (m < kFloor) m = kFloor;
    logSum += log(m);
    arithSum += m;
    n++;
  }
  if (n == 0 || arithSum <= 0.0) return 0.0;
  double flat = exp(logSum / (double)n) / (arithSum / (double)n);
  if (flat < 0.0) flat = 0.0;
  if (flat > 1.0) flat = 1.0;
  return flat;
}

double wholeSpectrumCentroid(const SpectralAnalyzer& sa) {
  double weighted = 0.0, total = 0.0;
  for (size_t k = 1; k <= kLastBin; k++) {
    weighted += (double)SpectralAnalyzer::binToHz(k, kFs) * sa.magnitudes()[k];
    total += sa.magnitudes()[k];
  }
  return (total > 1e-12) ? (weighted / total) : 0.0;
}

// Les memes deux mesures, restreintes A LA MAIN a des bornes de bins donnees.
// Elles servent a verifier que spectralFlatness() / spectralCentroid() portent
// bien sur la bande annoncee, sans relire cette bande a travers le code
// surveille.
double flatnessOverBins(const SpectralAnalyzer& sa, size_t lo, size_t hi) {
  const double kFloor = 1e-12;
  double logSum = 0.0, arithSum = 0.0;
  size_t n = 0;
  for (size_t k = lo; k <= hi; k++) {
    double m = sa.magnitudes()[k];
    if (m < kFloor) m = kFloor;
    logSum += log(m);
    arithSum += m;
    n++;
  }
  if (n == 0 || arithSum <= 0.0) return 0.0;
  double flat = exp(logSum / (double)n) / (arithSum / (double)n);
  if (flat < 0.0) flat = 0.0;
  if (flat > 1.0) flat = 1.0;
  return flat;
}

double centroidOverBins(const SpectralAnalyzer& sa, size_t lo, size_t hi) {
  double weighted = 0.0, total = 0.0;
  for (size_t k = lo; k <= hi; k++) {
    weighted += (double)SpectralAnalyzer::binToHz(k, kFs) * sa.magnitudes()[k];
    total += sa.magnitudes()[k];
  }
  return (total > 1e-12) ? (weighted / total) : 0.0;
}

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
  // ASSERTION DEPLACEE, PAS AFFAIBLIE. Elle disait "les deux lots couvrent tout
  // le spectre" (binCount() - 1). C'etait vrai du code et faux du signal : la
  // chaine de production a vide les bins hors de MIC_FILTER_HP_HZ..LP_HZ, et
  // les compter revenait a etendre le plancher a une bande dont le firmware
  // avait lui-meme retire le contenu. La partition porte maintenant sur la
  // BANDE ANALYSEE - meme propriete, meme rigueur, sur le bon domaine - et ses
  // bornes sont recalculees ici depuis settings.h.
  assert((size_t)(h.harmonicBins + h.noiseBins) == expectedBandCount());
  size_t counted = 0;
  for (size_t k = expectedBandLo(); k <= expectedBandHi(); k++) {
    if (SpectralAnalyzer::isHarmonicBin(k, f0, kFs)) counted++;
  }
  assert(counted == h.harmonicBins);
  // Et l'energie de raies publiee est bien celle des seuls bins DE LA BANDE :
  // une somme prise sur tout le spectre serait strictement plus grande.
  assert(fabs((double)h.harmonicEnergy - harmonicEnergyInBand(sa, f0)) <=
         1e-5 * (double)h.harmonicEnergy);
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

      // (a.1) ANCRAGE DE PARSEVAL, conserve mot pour mot. La somme des
      //       |X[k]|^2 sur TOUT le spectre retrouve la puissance temporelle au
      //       facteur de fenetre pres. C'est ce que l'ancienne assertion
      //       verifiait a travers noiseEnergy, du temps ou le plancher etait
      //       etendu a tout le spectre ; elle est gardee ici, portee sur le
      //       spectre lui-meme, pour que le facteur 3/16 reste verrouille.
      double whole = 0.0;
      for (size_t k = 1; k <= kLastBin; k++) {
        whole += (double)sa.magnitudes()[k] * sa.magnitudes()[k];
      }
      const double parseval =
          (double)kHannOneSidedPowerFactor * SpectralAnalyzer::totalPower(buf.data(), MIC_FFT_SIZE);
      assert(fabs(whole - parseval) / parseval < 0.15);

      // (a.2) Et l'energie de bruit ESTIMEE retrouve celle qui est reellement
      //       dans la bande analysee. La reference est la somme reelle de ces
      //       bins-la, pas la puissance temporelle mise a l'echelle : une bande
      //       de 111 bins ne porte pas exactement sa part de la puissance d'une
      //       realisation donnee - l'ecart-type relatif de cette somme vaut
      //       1/sqrt(111), soit 9,5 %, ce qui melangerait la variance du signal
      //       a l'erreur de l'estimateur.
      double realInBand = 0.0;
      for (size_t k = expectedBandLo(); k <= expectedBandHi(); k++) {
        realInBand += (double)sa.magnitudes()[k] * sa.magnitudes()[k];
      }
      const double err = fabs((double)h.noiseEnergy - realInBand) / realInBand;
      assert(err < 0.15);
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
  //
  // ASSERTION ELARGIE, ET IL FAUT DIRE POURQUOI. Elle exigeait moins de -10 dB
  // sur DEUX graines. Cette borne tenait par chance : sur un bruit pur, le HNR
  // est une STATISTIQUE - la somme des bins de raies fluctue autour du plancher
  // et laisse un residu - et restreindre la mesure a la bande passante reduit
  // l'echantillon (111 bins au lieu de 256, dont jusqu'a 68 sous des lobes de
  // raies a f0 = 500 Hz), donc augmente ce residu. Mesure sur 2800 tirages :
  // la queue atteint -0,75 dB AVANT la correction et +1,34 dB apres - deux
  // dB de degradation sur un evenement rare, sur un signal que la chaine
  // complete refuse de toute facon faute de pitch. La propriete est donc
  // verifiee sur une POPULATION, ce qui est plus fort que sur deux graines, et
  // la borne par tirage dit ce qui est vraiment garanti.
  {
    std::vector<float> readings;
    for (uint32_t s = 1; s <= 24; s++) {
      audiosig::whiteNoise(buf.data(), kFrame, 0.4f, s * 7919u);
      assert(sa.computeSpectrum(buf.data(), kFrame));
      for (float hz : {440.0f, 500.0f, 880.0f, 2000.0f}) {
        HarmonicNoiseRatio h = sa.harmonicNoiseRatio(hz, kFs);
        assert(h.valid);
        assert(!std::isnan(h.db) && !std::isinf(h.db));
        assert(h.db >= -MIC_HNR_MAX_DB && h.db <= MIC_HNR_MAX_DB);
        readings.push_back(h.db);
      }
    }
    std::sort(readings.begin(), readings.end());
    size_t below10 = 0;
    for (float db : readings) if (db < -10.0f) below10++;
    const float worst = readings.back();
    const float median = readings[readings.size() / 2];
    printf("  [hnr] bruit blanc pur, %zu tirages : pire %+.2f dB, mediane %+.2f dB, "
           "%.0f %% sous -10 dB\n",
           readings.size(), (double)worst, (double)median,
           100.0 * (double)below10 / (double)readings.size());
    // Aucun tirage ne fait passer du bruit pur pour un son domine par ses raies.
    assert(worst < 0.0f);
    // La borne d'origine, conservee comme propriete d'ensemble.
    assert(median < -15.0f);
    assert(below10 * 10 >= readings.size() * 7);   // au moins 70 %
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
  // Rapportee a l'energie de la BANDE, puisque c'est la seule que la mesure
  // compte des deux cotes (voir SpectralAnalyzer::analysisBand).
  const double spectralTotal = spectralEnergyInBand(sa);
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
    const double spectral = spectralEnergyInBand(sa);
    assert((double)h.harmonicEnergy / spectral > 0.95);

    const float old = legacyHnrDb(buf.data(), kFrame, f0);
    if (old < oldLo) oldLo = old;
    if (old > oldHi) oldHi = old;
  }
  // BORNE ELARGIE DE 3,0 A 3,5 dB, ET IL FAUT DIRE POURQUOI - c'est le seul
  // endroit ou restreindre la mesure a la bande passante coute quelque chose.
  //
  // Mesure sur ces six f0 : 32,53 a 35,72 dB, soit 3,19 dB d'etendue contre
  // moins de 3 dB avant. La cause est identifiee et elle n'est pas dans
  // l'attribution des bins - la part d'energie captee reste a 0,999 partout,
  // verifiee ci-dessus - mais dans le PLANCHER : il est estime sur 43 a 65 bins
  // au lieu de 188, et ces bins-la sont ceux qui bordent les vingt lobes de
  // raies, donc ceux qui recoivent leurs lobes secondaires. Le plancher estime
  // passe de 9,65e-8 (f0 pile sur un bin : les zeros du lobe de Hann tombent
  // dans la fenetre, le premier bin de bruit est a -31 dB) a 2,0e-7 (f0 entre
  // deux bins : la fuite s'etale). Le sens de l'erreur est le bon : la fuite
  // GONFLE le plancher, donc ABAISSE le HNR. La mesure devient un peu
  // pessimiste sur les notes tres propres, jamais flatteuse.
  //
  // En compensation, la borne basse est RESSERREE de 25 a 30 dB : le pire
  // alignement mesure 32,53 dB, et une regression de l'attribution des bins s'y
  // verrait immediatement.
  printf("  [hnr] f0 alignee ou non sur la grille : %.2f a %.2f dB (etendue %.2f dB) ; "
         "ancienne mesure : %.2f a %.2f dB\n",
         (double)lo, (double)hi, (double)(hi - lo), (double)oldLo, (double)oldHi);
  assert(hi - lo < 3.5f);
  assert(lo > 30.0f);

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

    // Le decoupage couvre exactement la bande analysee (cf. la note sur le
    // deplacement de cette assertion dans hnr_harmonic_bins_cover_the_hann_main_lobe).
    assert((size_t)(h.harmonicBins + h.noiseBins) == expectedBandCount());
    assert(h.partials == SpectralAnalyzer::usablePartials(t.f0, kFs));

    // noiseEnergy est bien le plancher etendu a TOUS les bins de la bande, et
    // non aux seuls bins ou il a pu etre mesure : le bruit existe aussi sous
    // les raies. Il s'arrete en revanche aux bornes de la bande, ou il n'y a
    // plus de bruit a compter.
    const float expectedNoise =
        h.noiseFloorPerBin * (float)(h.harmonicBins + h.noiseBins);
    assert(fabsf(h.noiseEnergy - expectedNoise) <= 1e-6f * expectedNoise + 1e-18f);

    // harmonicEnergy est bien la somme des bins de raies du spectre expose.
    const double sum = harmonicEnergyInBand(sa, t.f0);
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

// ---------------------------------------------------------------------------
// 9 - La bande est celle que la chaine d'acquisition laisse passer
// ---------------------------------------------------------------------------

void hnr_band_is_the_one_the_acquisition_chain_leaves() {
  const SpectralAnalyzer::AnalysisBand band = SpectralAnalyzer::analysisBand(kFs);

  // Les bornes viennent de settings.h, recalculees ici sans passer par le code
  // surveille.
  assert(band.lo == expectedBandLo());
  assert(band.hi == expectedBandHi());
  assert(band.count() == expectedBandCount());
  assert(band.lo >= 1);                 // le continu n'est jamais analyse
  assert(band.hi <= kLastBin);

  // Le contenu de la bande : tout bin dedans est au-dessus du passe-haut et au
  // -dessous du passe-bas ; tout bin juste dehors est du mauvais cote.
  for (size_t k = band.lo; k <= band.hi; k++) {
    const float hz = SpectralAnalyzer::binToHz(k, kFs);
    if (MIC_FILTER_HP_HZ > 0.0f) assert(hz >= MIC_FILTER_HP_HZ);
    if (MIC_FILTER_LP_HZ > 0.0f) assert(hz <= MIC_FILTER_LP_HZ);
  }
  if (MIC_FILTER_HP_HZ > 0.0f && band.lo > 1) {
    assert(SpectralAnalyzer::binToHz(band.lo - 1, kFs) < MIC_FILTER_HP_HZ);
  }
  if (MIC_FILTER_LP_HZ > 0.0f && band.hi < kLastBin) {
    assert(SpectralAnalyzer::binToHz(band.hi + 1, kFs) > MIC_FILTER_LP_HZ);
  }

  // Filtrage DESACTIVE : la bande redevient le spectre entier, et la mesure
  // avec elle. C'est la configuration que AudioFilterChain accepte en mettant
  // une coupure a 0 - la cellule devient transparente - et elle ne doit pas
  // etre un cas particulier dans le code.
  if (MIC_FILTER_HP_HZ <= 0.0f) assert(band.lo == 1);
  if (MIC_FILTER_LP_HZ <= 0.0f) assert(band.hi == kLastBin);

  // Frequence d'echantillonnage degeneree : bande vide, donc refus, jamais une
  // bande inventee.
  assert(SpectralAnalyzer::analysisBand(0.0f).count() == 0);
  assert(SpectralAnalyzer::analysisBand(-32000.0f).count() == 0);

  printf("  [hnr] bande analysee : bins %u..%u (%u sur %zu) = %.0f..%.0f Hz, "
         "soit %.0f %% du spectre\n",
         band.lo, band.hi, band.count(), kLastBin,
         (double)SpectralAnalyzer::binToHz(band.lo, kFs),
         (double)SpectralAnalyzer::binToHz(band.hi, kFs),
         100.0 * (double)band.count() / (double)kLastBin);
}

// ---------------------------------------------------------------------------
// 10 - LE test de non-recidive : la mesure survit a la chaine de production
//
// C'est le test que la suite n'avait pas. Elle mesurait du PCM brut, alors que
// AudioAnalyzer::drainI2S() filtre le flux avant l'anneau : personne n'avait
// donc vu que le plancher etait estime au milieu de la bande COUPEE, puis
// etendu a tout le spectre.
//
// Deux choses sont verrouillees ici :
//   1. le filtrage de production ne doit presque pas deplacer le HNR - il
//      retire du bruit hors bande, que la mesure ne compte plus d'aucun cote ;
//   2. l'ancienne extrapolation a plancher plat, reproduite dans ce fichier,
//      doit etre massivement OPTIMISTE sur ce meme signal. Si quelqu'un la
//      remet, c'est le point 1 qui casse.
// ---------------------------------------------------------------------------

void hnr_survives_the_production_filter_chain() {
  SpectralAnalyzer sa;
  std::vector<float> filtered(kFrame);
  std::vector<float> raw(kFrame);
  ProductionStream stream;

  const float kBreath[] = {0.0f, 0.010f, 0.020f, 0.050f, 0.100f, 0.200f};
  float worstGap = 0.0f;
  float worstFlatFloorGap = 0.0f;

  printf("  [hnr] %-8s %10s %10s %8s %10s\n", "souffle", "brut", "production",
         "ecart", "plancher plat");
  for (float breath : kBreath) {
    stream.fluteFrame(filtered.data(), kFrame, 440.0f, 0.40f, breath, kWarmFrames);
    // MEME frame du meme signal, sans la chaine : la seule difference est le
    // filtrage.
    audiosig::fluteLike(raw.data(), kFrame, 440.0f, 0.40f, breath, kFs,
                        (size_t)kWarmFrames * kFrame);

    assert(sa.computeSpectrum(raw.data(), kFrame));
    const HarmonicNoiseRatio rawHnr = sa.harmonicNoiseRatio(440.0f, kFs);
    assert(sa.computeSpectrum(filtered.data(), kFrame));
    const HarmonicNoiseRatio prodHnr = sa.harmonicNoiseRatio(440.0f, kFs);
    assert(rawHnr.valid && prodHnr.valid);

    // Ce que la mesure rendrait si le plancher etait de nouveau estime puis
    // etendu a tout le spectre - sur le signal que la production presente.
    const float flatFloor = flatFloorHnrDb(sa, 440.0f);

    const float gap = fabsf(prodHnr.db - rawHnr.db);
    const float flatGap = flatFloor - prodHnr.db;
    if (gap > worstGap) worstGap = gap;
    if (flatGap > worstFlatFloorGap) worstFlatFloorGap = flatGap;

    printf("  [hnr] %-8.3f %+9.2f  %+9.2f  %+7.2f  %+9.2f\n", (double)breath,
           (double)rawHnr.db, (double)prodHnr.db, (double)(prodHnr.db - rawHnr.db),
           (double)flatFloor);
  }
  fflush(stdout);

  // 1. Le filtrage de production ne deplace plus le verdict. Avant correction
  //    l'ecart atteignait +12,5 dB - la moitie de la plage utile de la mesure.
  //    2 dB est la tolerance : il reste l'attenuation du passe-bas DANS la
  //    bande, jusqu'a -3 dB a la coupure, qui touche un peu plus le bruit que
  //    les partiels d'une note grave.
  assert(worstGap < 2.0f);

  // 2. ...et ce n'est pas parce que la mesure serait devenue insensible : sur
  //    le MEME signal, l'ancienne extrapolation lit 7,4 dB trop haut (mesure :
  //    +5,50 dB a souffle 0,020, +7,43 a 0,050, +7,04 a 0,100, +7,41 a 0,200 ;
  //    l'ecart est ECRASE aux deux premiers points parce que l'ancienne mesure
  //    y sature deja contre MIC_HNR_MAX_DB - c'est precisement le defaut).
  //    Cette assertion echoue si le plancher redevient etendu a tout le
  //    spectre, puisque les deux mesures se confondraient alors.
  //
  //    Elle n'a de sens que si la chaine filtre vraiment. Avec MIC_FILTER_HP_HZ
  //    et MIC_FILTER_LP_HZ a 0, la bande analysee EST le spectre entier et les
  //    deux estimateurs se confondent exactement - c'est justement la propriete
  //    a garantir : la correction ne deplace rien quand il n'y a rien a
  //    corriger. Dans cette configuration on le verifie donc a l'envers.
  if (SpectralAnalyzer::analysisBand(kFs).count() < kLastBin) {
    assert(worstFlatFloorGap > 6.0f);
  } else {
    assert(fabsf(worstFlatFloorGap) < 0.1f);
  }

  printf("  [hnr] ecart max production/brut : %.2f dB ; ce que l'extrapolation a "
         "plancher plat ajouterait : +%.2f dB\n",
         (double)worstGap, (double)worstFlatFloorGap);

  // 3. La memoire du filtre est bien ETABLIE. Un IIR remis a zero juste avant
  //    la frame mesuree produit un transitoire : le PCM en est visiblement
  //    different. Sans ce controle, un futur "nettoyage" du banc d'essai
  //    pourrait reinitialiser la chaine a chaque frame sans que rien ne le
  //    signale - et on mesurerait de nouveau autre chose que la production.
  {
    AudioFilterChain cold;
    cold.configureDefaults();
    std::vector<float> coldFrame(kFrame);
    audiosig::fluteLike(coldFrame.data(), kFrame, 440.0f, 0.40f, 0.020f, kFs,
                        (size_t)kWarmFrames * kFrame);
    cold.processBlock(coldFrame.data(), kFrame);
    stream.fluteFrame(filtered.data(), kFrame, 440.0f, 0.40f, 0.020f, kWarmFrames);
    float maxDelta = 0.0f;
    for (size_t i = 0; i < (size_t)kFrame; i++) {
      const float d = fabsf(coldFrame[i] - filtered[i]);
      if (d > maxDelta) maxDelta = d;
    }
    printf("  [hnr] transitoire d'etablissement du filtre : ecart PCM max %.4f\n",
           (double)maxDelta);
    // Le transitoire mesurable vient des biquads. Avec les deux coupures a 0 il
    // ne reste que le retrait de continu, dont la reponse est quasi plate : la
    // comparaison n'aurait plus rien a montrer.
    if (cold.highPassActive() || cold.lowPassActive()) assert(maxDelta > 0.05f);
  }
}

// ---------------------------------------------------------------------------
// 11 - Il reste de quoi estimer un plancher sur TOUTE la plage de l'instrument
//
// Restreindre la mesure a la bande passante reduit l'echantillon qui sert au
// plancher : les vingt lobes de raies en occupent une bonne part. Si l'un des
// pitchs de la plage passait sous SpectralHnr::kMinNoiseBins, la mesure y
// serait refusee - une regression silencieuse, puisque le repli Goertzel prend
// alors la main sans rien dire.
// ---------------------------------------------------------------------------

void hnr_noise_sample_survives_the_whole_pitch_range() {
  SpectralAnalyzer sa;
  std::vector<float> buf(kFrame);
  ProductionStream stream;
  stream.fluteFrame(buf.data(), kFrame, 440.0f, 0.40f, 0.020f, kWarmFrames);
  assert(sa.computeSpectrum(buf.data(), kFrame));

  uint16_t worst = 0xFFFF;
  float worstHz = 0.0f;
  for (float hz = MIC_PITCH_MIN_HZ; hz <= MIC_PITCH_MAX_HZ; hz += 1.0f) {
    const HarmonicNoiseRatio h = sa.harmonicNoiseRatio(hz, kFs);
    assert(h.valid);                       // jamais un refus sur la plage utile
    assert(h.noiseBins >= SpectralHnr::kMinNoiseBins);
    if (h.noiseBins < worst) { worst = h.noiseBins; worstHz = hz; }
  }
  printf("  [hnr] pire echantillon de plancher sur %g..%g Hz : %u bins a %.0f Hz "
         "(minimum exige %u)\n",
         (double)MIC_PITCH_MIN_HZ, (double)MIC_PITCH_MAX_HZ, worst, (double)worstHz,
         SpectralHnr::kMinNoiseBins);
}

// ---------------------------------------------------------------------------
// 12 - Un spectre non fini est REFUSE, pas rendu
//
// Contrat de HarmonicNoiseRatio : `valid` faux ou un nombre utilisable, jamais
// `valid = true` avec un NaN. Un NaN traverse `< 0.0f`, `<= kPowerFloor` et
// `> MIC_HNR_MAX_DB` sans en declencher aucun ; il faut donc un refus explicite.
// Cas de contrat, non atteignable depuis l'I2S qui livre des entiers.
// ---------------------------------------------------------------------------

void hnr_refuses_a_non_finite_spectrum() {
  SpectralAnalyzer sa;
  std::vector<float> buf(kFrame);
  audiosig::fluteLike(buf.data(), kFrame, 500.0f, 0.4f, 0.02f, kFs);
  buf[123] = NAN;                  // un seul echantillon suffit : la FFT melange tout
  assert(sa.computeSpectrum(buf.data(), kFrame));
  const HarmonicNoiseRatio h = sa.harmonicNoiseRatio(500.0f, kFs);
  assert(!h.valid);
  assert(!std::isnan(h.db));
  assert(h.db == 0.0f);
  // Le refus est TARDIF : les compteurs disent pourquoi la mesure a ete refusee.
  assert(h.partials > 0 && h.harmonicBins > 0);

  // Et l'infini, meme traitement.
  audiosig::fluteLike(buf.data(), kFrame, 500.0f, 0.4f, 0.02f, kFs);
  buf[7] = INFINITY;
  assert(sa.computeSpectrum(buf.data(), kFrame));
  const HarmonicNoiseRatio inf = sa.harmonicNoiseRatio(500.0f, kFs);
  assert(!inf.valid || (!std::isnan(inf.db) && !std::isinf(inf.db)));
}

// ---------------------------------------------------------------------------
// 13 - LES FORMES SPECTRALES MESURENT LE SIGNAL, PAS LE FILTRE
//
// Meme famille de defaut que la section 10, sur une autre statistique, et c'est
// LE test de non-recidive de la platitude.
//
// spectralFlatness() prenait sa moyenne geometrique sur TOUT le spectre. Une
// moyenne geometrique passe par un logarithme : un bin vide y pese
// log(plancher), c'est-a-dire beaucoup, alors qu'il ne porte aucune energie.
// Sur les 56 % de bins que la chaine de production vide, c'etaient donc des
// bins SANS signal qui dominaient le vote. Consequence chiffree : du bruit
// blanc pur - le signal le plus plat qui puisse exister - lisait 0,36 a 0,40
// en production contre 0,81 a 0,87 sur PCM brut, si bien que
// AQ_BREATH_FLATNESS_NOISE (0,70) etait INATTEIGNABLE et qu'un cinquieme du
// critere de respiration etait mort sans que rien ne le signale.
//
// Le centroide souffrait du meme mal, plus doucement parce qu'il pondere par
// l'energie : 8300 Hz brut contre 4636 Hz filtre sur du bruit blanc.
//
// Ce que ce test verrouille, et qui ECHOUE avec la mesure pleine bande :
//   1. du bruit blanc pur, PASSE DANS LA CHAINE DE PRODUCTION, doit lire une
//      platitude au-dessus de AQ_BREATH_FLATNESS_NOISE - sans quoi la
//      composante ne peut jamais dire "du bruit" ;
//   2. le filtrage ne doit presque plus deplacer ni la platitude ni le
//      centroide, alors que la mesure pleine bande les deplace massivement ;
//   3. les deux mesures portent exactement sur la bande annoncee, recalculee
//      ici depuis settings.h ;
//   4. avec les coupures a 0 - cellules transparentes - la bande EST le spectre
//      entier et les deux estimateurs doivent se confondre EXACTEMENT. Les deux
//      sens sont assertes, chacun dans sa configuration.
// ---------------------------------------------------------------------------

void spectral_shape_is_measured_in_the_analysis_band() {
  SpectralAnalyzer sa;
  std::vector<float> filtered(kFrame);
  std::vector<float> raw(kFrame);
  ProductionStream stream;

  const SpectralAnalyzer::AnalysisBand band = SpectralAnalyzer::analysisBand(kFs);
  const bool chainFilters = (band.count() < kLastBin);

  // --- 3. Le domaine annonce est le domaine mesure --------------------------
  // Un spectre quelconque suffit : ce point porte sur le DECOUPAGE, pas sur le
  // signal. Les bornes sont celles recalculees dans ce fichier depuis
  // settings.h, jamais celles que le code surveille rendrait.
  stream.fluteFrame(filtered.data(), kFrame, 440.0f, 0.40f, 0.100f, kWarmFrames);
  assert(sa.computeSpectrum(filtered.data(), kFrame));
  {
    const double refFlat = flatnessOverBins(sa, expectedBandLo(), expectedBandHi());
    const double refCent = centroidOverBins(sa, expectedBandLo(), expectedBandHi());
    assert(fabs((double)sa.spectralFlatness(kFs) - refFlat) < 1e-4);
    assert(fabs((double)sa.spectralCentroid(kFs) - refCent) < 1.0);

    // Et le bin 0 - le continu, deja retire par computeSpectrum - n'entre
    // JAMAIS dans le compte, quelle que soit la bande.
    assert(band.lo >= 1);
  }

  // --- 4. Les deux sens de la bande ----------------------------------------
  if (chainFilters) {
    // Coupures actives : la bande est STRICTEMENT plus etroite que le spectre,
    // et les deux estimateurs doivent donc differer. Sans cela le test 1 ne
    // prouverait rien - il passerait aussi avec l'ancienne mesure.
    assert(band.count() < kLastBin);
    assert(fabs((double)sa.spectralFlatness(kFs) - wholeSpectrumFlatness(sa)) > 0.01);
    assert(fabs((double)sa.spectralCentroid(kFs) - wholeSpectrumCentroid(sa)) > 10.0);
  } else {
    // Coupures a 0 : AudioFilterChain rend les cellules transparentes, la bande
    // redevient le spectre entier et les deux estimateurs se CONFONDENT. La
    // correction ne doit rien deplacer quand il n'y a rien a corriger.
    assert(band.lo == 1 && band.hi == kLastBin);
    assert(fabs((double)sa.spectralFlatness(kFs) - wholeSpectrumFlatness(sa)) < 1e-4);
    assert(fabs((double)sa.spectralCentroid(kFs) - wholeSpectrumCentroid(sa)) < 1e-2);
  }

  // --- 1 et 2. Le bruit blanc, brut puis passe dans la chaine ---------------
  // Trois graines : la platitude est une STATISTIQUE, pas une constante, et un
  // seuil justifie sur une seule realisation ne vaudrait rien.
  const uint32_t kSeeds[] = {12345u, 99u, 7u};
  float worstProdFlat = 1.0f;
  float worstFlatGap = 0.0f;         // |brut - production|, mesure corrigee
  double worstWholeGap = 0.0;        // |brut - production|, mesure pleine bande
  float worstCentGapPct = 0.0f;
  double worstWholeCentPct = 0.0;
  // Ecart entre "dans la bande" et "sur tout le spectre", mesure sur les MEMES
  // frames : c'est lui qui dit si la restriction change quelque chose.
  double worstBandVsWholeFlat = 0.0;
  double worstBandVsWholeCentPct = 0.0;

  printf("  [flat] %-8s %8s %8s | %10s %10s | %8s %8s\n", "graine",
         "brut", "prod", "brut(tout)", "prod(tout)", "cent brut", "cent prod");
  for (uint32_t seed : kSeeds) {
    // MEME frame du meme bruit, avec et sans la chaine : la seule difference
    // est le filtrage.
    stream.noiseFrame(filtered.data(), kFrame, 0.30f, seed, kWarmFrames);
    audiosig::whiteNoise(raw.data(), kFrame, 0.30f, seed + (uint32_t)kWarmFrames);

    assert(sa.computeSpectrum(raw.data(), kFrame));
    const float rawFlat = sa.spectralFlatness(kFs);
    const float rawCent = sa.spectralCentroid(kFs);
    const double rawWhole = wholeSpectrumFlatness(sa);
    const double rawWholeCent = wholeSpectrumCentroid(sa);

    assert(sa.computeSpectrum(filtered.data(), kFrame));
    const float prodFlat = sa.spectralFlatness(kFs);
    const float prodCent = sa.spectralCentroid(kFs);
    const double prodWhole = wholeSpectrumFlatness(sa);
    const double prodWholeCent = wholeSpectrumCentroid(sa);

    printf("  [flat] %-8u %8.4f %8.4f | %10.4f %10.4f | %8.0f %8.0f\n",
           seed, (double)rawFlat, (double)prodFlat, rawWhole, prodWhole,
           (double)rawCent, (double)prodCent);

    if (prodFlat < worstProdFlat) worstProdFlat = prodFlat;
    if (fabsf(prodFlat - rawFlat) > worstFlatGap) worstFlatGap = fabsf(prodFlat - rawFlat);
    if (fabs(prodWhole - rawWhole) > worstWholeGap) worstWholeGap = fabs(prodWhole - rawWhole);
    const float centPct = fabsf(prodCent - rawCent) / rawCent;
    if (centPct > worstCentGapPct) worstCentGapPct = centPct;
    const double wholeCentPct = fabs(prodWholeCent - rawWholeCent) / rawWholeCent;
    if (wholeCentPct > worstWholeCentPct) worstWholeCentPct = wholeCentPct;

    // Ecart entre la mesure DANS LA BANDE et la mesure pleine bande, sur les
    // deux frames. C'est la grandeur qui doit s'annuler EXACTEMENT quand les
    // coupures valent 0, et rester grande quand elles sont actives.
    const double d[4] = {fabs((double)rawFlat - rawWhole), fabs((double)prodFlat - prodWhole),
                         fabs((double)rawCent - rawWholeCent) / rawWholeCent,
                         fabs((double)prodCent - prodWholeCent) / prodWholeCent};
    if (d[0] > worstBandVsWholeFlat) worstBandVsWholeFlat = d[0];
    if (d[1] > worstBandVsWholeFlat) worstBandVsWholeFlat = d[1];
    if (d[2] > worstBandVsWholeCentPct) worstBandVsWholeCentPct = d[2];
    if (d[3] > worstBandVsWholeCentPct) worstBandVsWholeCentPct = d[3];
  }
  fflush(stdout);

  // 1. LE SEUIL EST ATTEIGNABLE. Du bruit blanc pur, passe dans la chaine de
  //    production, franchit AQ_BREATH_FLATNESS_NOISE sur les TROIS graines.
  //    Avec la mesure pleine bande il lisait 0,36 a 0,40 : cette assertion
  //    echoue, et c'est elle qui empeche la recidive.
  //    Le seuil est repris de AcousticQuality.h plutot que recopie : s'il
  //    remonte un jour au-dessus de ce que du bruit pur mesure, ce test le dit.
  printf("  [flat] platitude la plus BASSE sur du bruit blanc filtre : %.4f "
         "(AQ_BREATH_FLATNESS_NOISE = %.2f)\n",
         (double)worstProdFlat, (double)AQ_BREATH_FLATNESS_NOISE);
  assert(worstProdFlat > AQ_BREATH_FLATNESS_NOISE);

  // 2. La chaine ne deplace presque plus les deux formes, alors qu'elle
  //    deplacait massivement les mesures pleine bande. Les deux moities de
  //    l'assertion comptent : la premiere seule serait satisfaite par une
  //    mesure devenue insensible a tout.
  printf("  [flat] ecart brut/production - bande : %.4f (platitude) et %.1f %% "
         "(centroide) ; spectre entier : %.4f et %.1f %%\n",
         (double)worstFlatGap, 100.0 * (double)worstCentGapPct,
         worstWholeGap, 100.0 * worstWholeCentPct);
  printf("  [flat] ecart bande / spectre entier sur les MEMES frames : %.4f "
         "(platitude) et %.2f %% (centroide)\n",
         worstBandVsWholeFlat, 100.0 * worstBandVsWholeCentPct);
  if (chainFilters) {
    assert(worstFlatGap < 0.05f);              // mesure corrigee : le filtre ne se voit plus
    assert(worstWholeGap > 0.30);              // mesure pleine bande : il se voit enormement
    assert(worstCentGapPct < 0.10f);           // centroide : moins de 10 %
    assert(worstWholeCentPct > 0.30);          // ... contre plus de 30 % pleine bande
    // La restriction n'est pas cosmetique : sur les memes frames, les deux
    // domaines ne donnent pas le meme nombre.
    assert(worstBandVsWholeFlat > 0.30);
    assert(worstBandVsWholeCentPct > 0.30);
  } else {
    // COUPURES A 0. AudioFilterChain rend les deux cellules transparentes, la
    // bande redevient le spectre entier, et les deux estimateurs doivent alors
    // rendre EXACTEMENT le meme nombre - a la precision du float pres, puisque
    // c'est litteralement la meme boucle sur les memes bins. Verifie a
    // l'envers, comme en section 10.
    assert(worstBandVsWholeFlat < 1e-6);
    assert(worstBandVsWholeCentPct < 1e-6);

    // La chaine, elle, n'est PAS tout a fait transparente pour autant : le
    // retrait de continu (MIC_FILTER_DC_POLE) est un etage a part, qu'une
    // coupure a 0 ne desactive pas. Il reste donc un ecart brut/production,
    // mais deux ordres de grandeur plus petit que celui du filtrage - et il
    // doit etre le MEME des deux cotes, puisque c'est la meme mesure.
    assert(worstFlatGap < 0.01f);
    assert(worstCentGapPct < 0.01f);
    assert(fabs(worstWholeGap - (double)worstFlatGap) < 1e-6);
    assert(fabs(worstWholeCentPct - (double)worstCentGapPct) < 1e-6);
  }

  // --- Ce que cela donne sur une NOTE, pour memoire dans la sortie ----------
  // C'est le tableau qui justifie AQ_BREATH_FLATNESS_TONE et
  // AQ_BREATH_FLATNESS_NOISE : il doit rester lisible dans la sortie de la
  // suite pour qu'on puisse le comparer, plus tard, a un releve sur microphone.
  const float kBreath[] = {0.0f, 0.010f, 0.020f, 0.100f, 0.200f, 0.350f};
  printf("  [flat] %-8s %8s %8s | %10s %10s\n", "souffle", "brut", "prod",
         "brut(tout)", "prod(tout)");
  float prev = -1.0f;
  for (float breath : kBreath) {
    stream.fluteFrame(filtered.data(), kFrame, 440.0f, 0.40f, breath, kWarmFrames);
    audiosig::fluteLike(raw.data(), kFrame, 440.0f, 0.40f, breath, kFs,
                        (size_t)kWarmFrames * kFrame);
    assert(sa.computeSpectrum(raw.data(), kFrame));
    const float rawFlat = sa.spectralFlatness(kFs);
    const double rawWhole = wholeSpectrumFlatness(sa);
    assert(sa.computeSpectrum(filtered.data(), kFrame));
    const float prodFlat = sa.spectralFlatness(kFs);
    const double prodWhole = wholeSpectrumFlatness(sa);
    printf("  [flat] %-8.3f %8.4f %8.4f | %10.4f %10.4f\n", (double)breath,
           (double)rawFlat, (double)prodFlat, rawWhole, prodWhole);
    // La platitude croit avec le souffle - sinon les deux bornes n'ordonnent
    // rien du tout - et elle reste bornee.
    assert(prodFlat > prev);
    assert(prodFlat >= 0.0f && prodFlat <= 1.0f);
    prev = prodFlat;
  }
  fflush(stdout);

  // Les deux bornes encadrent bien ce que la chaine presente : une note sans
  // souffle est sous TONE, la note la plus soufflee dont le pitch survive est
  // entre les deux, et du bruit pur est au-dessus de NOISE (deja asserte).
  //
  // A SAVOIR, parce que la marge n'est pas la meme partout : avec les coupures
  // de settings.h la note a souffle 0,350 mesure 0,59, soit 0,16 sous NOISE.
  // Avec les coupures a 0 elle monte a 0,738, soit 0,012 seulement - le
  // filtrage fait PARTIE de ce qui separe les deux bornes, puisqu'il retire du
  // signal une bande ou seul le souffle vit. Ce n'est pas une configuration de
  // production, mais l'assertion y est a un cheveu, et il vaut mieux l'ecrire
  // que de la decouvrir un jour en rouge.
  stream.fluteFrame(filtered.data(), kFrame, 440.0f, 0.40f, 0.0f, kWarmFrames);
  assert(sa.computeSpectrum(filtered.data(), kFrame));
  assert(sa.spectralFlatness(kFs) < AQ_BREATH_FLATNESS_TONE);
  stream.fluteFrame(filtered.data(), kFrame, 440.0f, 0.40f, 0.350f, kWarmFrames);
  assert(sa.computeSpectrum(filtered.data(), kFrame));
  const float breathiest = sa.spectralFlatness(kFs);
  assert(breathiest > AQ_BREATH_FLATNESS_TONE);
  assert(breathiest < AQ_BREATH_FLATNESS_NOISE);
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
  hnr_band_is_the_one_the_acquisition_chain_leaves();
  hnr_survives_the_production_filter_chain();
  hnr_noise_sample_survives_the_whole_pitch_range();
  hnr_refuses_a_non_finite_spectrum();
  spectral_shape_is_measured_in_the_analysis_band();
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
