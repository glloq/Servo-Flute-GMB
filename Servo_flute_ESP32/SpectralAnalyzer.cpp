#include "SpectralAnalyzer.h"

#include <math.h>

namespace {
constexpr float kTwoPi = 6.28318530717958647692f;

// Moyenne d'une frame. Le continu est retire partout : un decalage de
// polarisation du microphone n'est pas du signal.
inline float meanOf(const float* x, size_t n) {
  float m = 0.0f;
  for (size_t i = 0; i < n; i++) m += x[i];
  return m / (float)n;
}
}  // namespace

// ------------------------------------------------------------------ Goertzel --

float SpectralAnalyzer::goertzelPower(const float* x, size_t n, float targetHz,
                                      float sampleRate) {
  if (x == nullptr || n < 2 || sampleRate <= 0.0f) return 0.0f;
  // Au-dessus de Nyquist la mesure n'a aucun sens : elle rendrait l'energie
  // d'une frequence repliee, donc d'une autre frequence.
  if (targetHz <= 0.0f || targetHz >= sampleRate * 0.5f) return 0.0f;

  const float omega = kTwoPi * targetHz / sampleRate;
  const float coeff = 2.0f * cosf(omega);
  const float mean = meanOf(x, n);

  float s1 = 0.0f, s2 = 0.0f;
  for (size_t i = 0; i < n; i++) {
    const float s0 = (x[i] - mean) + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  // Puissance normalisee par la longueur pour que le resultat ne depende pas
  // de la taille de frame.
  const float power = (s1 * s1 + s2 * s2 - coeff * s1 * s2) / ((float)n * (float)n);
  return power > 0.0f ? power : 0.0f;
}

float SpectralAnalyzer::totalPower(const float* x, size_t n) {
  if (x == nullptr || n == 0) return 0.0f;
  const float mean = meanOf(x, n);
  float sum = 0.0f;
  for (size_t i = 0; i < n; i++) {
    const float c = x[i] - mean;
    sum += c * c;
  }
  return sum / (float)n;
}

HarmonicEnergies SpectralAnalyzer::harmonics(const float* x, size_t n, float f0,
                                             float sampleRate) {
  HarmonicEnergies out;
  if (x == nullptr || n < 2 || f0 <= 0.0f || sampleRate <= 0.0f) return out;
  const float nyquist = sampleRate * 0.5f;
  if (f0 >= nyquist) return out;

  float* slots[4] = {&out.fundamental, &out.h2, &out.h3, &out.h4};
  for (int k = 0; k < 4; k++) {
    const float hz = f0 * (float)(k + 1);
    // Une note aigue n'a tout simplement pas quatre harmoniques sous Nyquist.
    // On s'arrete plutot que de mesurer une frequence repliee.
    if (hz >= nyquist) break;
    *slots[k] = goertzelPower(x, n, hz, sampleRate);
    out.measured = (uint8_t)(k + 1);
  }

  out.harmonicTotal = out.fundamental + out.h2 + out.h3 + out.h4;
  if (out.fundamental > 0.0f) {
    out.h2Ratio = out.h2 / out.fundamental;
    out.h3Ratio = out.h3 / out.fundamental;
    out.h4Ratio = out.h4 / out.fundamental;
  }
  out.valid = (out.measured > 0);
  return out;
}

// ----------------------------------------------------------------------- FFT --

#if MIC_FFT_ENABLED

static_assert((MIC_FFT_SIZE & (MIC_FFT_SIZE - 1)) == 0,
              "MIC_FFT_SIZE doit etre une puissance de deux (FFT radix-2)");
static_assert(MIC_FFT_SIZE >= 64, "MIC_FFT_SIZE trop petite pour etre utile");
static_assert(MIC_FFT_SIZE <= MIC_ANALYSIS_FRAME_SIZE,
              "MIC_FFT_SIZE ne peut pas depasser la taille de frame analysee");

void SpectralAnalyzer::buildWindow() {
  // Hann, demi-table : w[i] = w[N-1-i].
  const int N = MIC_FFT_SIZE;
  for (int i = 0; i < N / 2; i++) {
    _window[i] = 0.5f * (1.0f - cosf(kTwoPi * (float)i / (float)(N - 1)));
  }
  _spectrumValid = false;
}

void SpectralAnalyzer::fftInPlace() {
  const int N = MIC_FFT_SIZE;

  // 1. Permutation par inversion de bits, calculee a la volee (pas de table).
  for (int i = 1, j = 0; i < N; i++) {
    int bit = N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = _re[i]; _re[i] = _re[j]; _re[j] = t;
      t = _im[i]; _im[i] = _im[j]; _im[j] = t;
    }
  }

  // 2. Papillons Cooley-Tukey. Les facteurs de rotation sont produits par la
  //    recurrence stable de Numerical Recipes : deux appels trigonometriques
  //    par etage (18 au total pour 512 points) au lieu de N/2*log2(N) = 2304.
  for (int len = 2; len <= N; len <<= 1) {
    const float theta = -kTwoPi / (float)len;
    const float wpr = -2.0f * sinf(0.5f * theta) * sinf(0.5f * theta);
    const float wpi = sinf(theta);
    for (int i = 0; i < N; i += len) {
      float wr = 1.0f, wi = 0.0f;
      for (int k = 0; k < len / 2; k++) {
        const int a = i + k;
        const int b = a + len / 2;
        const float tr = _re[b] * wr - _im[b] * wi;
        const float ti = _re[b] * wi + _im[b] * wr;
        _re[b] = _re[a] - tr;
        _im[b] = _im[a] - ti;
        _re[a] += tr;
        _im[a] += ti;
        const float wrTmp = wr;
        wr += wr * wpr - wi * wpi;
        wi += wi * wpr + wrTmp * wpi;
      }
    }
  }
}

bool SpectralAnalyzer::computeSpectrum(const float* x, size_t n) {
  _spectrumValid = false;
  if (x == nullptr || n < (size_t)MIC_FFT_SIZE) return false;

  // Continu retire puis fenetre de Hann. Ici la fenetre est LEGITIME : elle
  // limite les fuites entre bins (voir l'en-tete du fichier).
  const float mean = meanOf(x, MIC_FFT_SIZE);
  for (int i = 0; i < MIC_FFT_SIZE; i++) {
    _re[i] = (x[i] - mean) * windowAt((size_t)i);
    _im[i] = 0.0f;
  }

  fftInPlace();

  // Amplitudes, normalisees par la taille pour ne pas dependre de MIC_FFT_SIZE.
  const float norm = 1.0f / (float)MIC_FFT_SIZE;
  const size_t bins = binCount();
  for (size_t k = 0; k < bins; k++) {
    _mag[k] = sqrtf(_re[k] * _re[k] + _im[k] * _im[k]) * norm;
  }
  _spectrumValid = true;
  return true;
}

float SpectralAnalyzer::spectralCentroid(float sampleRate) const {
  if (!_spectrumValid) return 0.0f;
  // MESURE DANS LA BANDE QUE LA CHAINE LAISSE PASSER, et pas sur tout le
  // spectre. Le centroide est une moyenne ponderee : ou la chaine a vide le
  // haut du spectre, elle le tire vers le bas sans qu'aucun son n'ait change.
  // Releve : bruit blanc a 8300 Hz sur PCM brut contre 4636 Hz apres
  // AudioFilterChain - 44 % d'ecart pour le meme signal. Dans la bande,
  // 3582 Hz contre 3444 Hz : 4 %. Ce qui reste vient du flanc a -3 dB des
  // cellules, qui est DANS la bande par construction.
  const AnalysisBand band = analysisBand(sampleRate);
  // Bande vide (frequence d'echantillonnage absurde) : aucune mesure n'est
  // possible et 0 est le refus, comme sans spectre calcule.
  if (band.count() == 0) return 0.0f;
  float weighted = 0.0f, total = 0.0f;
  // Le bin 0 (continu) reste exclu : band.lo vaut 1 au minimum. Il n'est pas du
  // son et fausserait le centre.
  for (size_t k = band.lo; k <= (size_t)band.hi; k++) {
    weighted += binToHz(k, sampleRate) * _mag[k];
    total += _mag[k];
  }
  return (total > 1e-12f) ? (weighted / total) : 0.0f;
}

float SpectralAnalyzer::spectralFlatness(float sampleRate) const {
  if (!_spectrumValid) return 0.0f;
  // MEME BANDE QUE LE HNR, et pour une raison plus forte encore que la sienne.
  // La moyenne geometrique passe par un logarithme : un bin vide y compte pour
  // log(plancher), c'est-a-dire beaucoup, alors qu'il ne porte aucune energie.
  // Sur les 56 % de bins que AudioAnalyzer::drainI2S() vide avant l'anneau, la
  // platitude mesurait donc surtout la pente du filtre. Du bruit blanc pur -
  // le signal le plus plat qui existe - lisait 0,39 en production contre 0,85
  // sur PCM brut, si bien que AQ_BREATH_FLATNESS_NOISE (0,70) etait devenu
  // INATTEIGNABLE : la composante platitude de computeBreathiness() ne pouvait
  // plus jamais declarer "du bruit", et rien ne le signalait.
  const AnalysisBand band = analysisBand(sampleRate);
  if (band.count() == 0) return 0.0f;
  // Moyenne geometrique calculee par somme de logarithmes : le produit direct
  // de 256 amplitudes sous-deborderait immediatement en simple precision.
  // Le plancher evite log(0) sur un bin vide.
  const float kFloor = 1e-12f;
  double logSum = 0.0;
  double arithSum = 0.0;
  size_t count = 0;
  for (size_t k = band.lo; k <= (size_t)band.hi; k++) {
    const float m = (_mag[k] > kFloor) ? _mag[k] : kFloor;
    logSum += log((double)m);
    arithSum += (double)m;
    count++;
  }
  if (count == 0 || arithSum <= 0.0) return 0.0f;
  const double geo = exp(logSum / (double)count);
  const double arith = arithSum / (double)count;
  const double flat = geo / arith;
  if (flat < 0.0) return 0.0f;
  if (flat > 1.0) return 1.0f;
  return (float)flat;
}

float SpectralAnalyzer::bandEnergy(float loHz, float hiHz, float sampleRate) const {
  if (!_spectrumValid || hiHz <= loHz) return 0.0f;
  // PAS de restriction a analysisBand ici, et ce n'est pas un oubli : la bande
  // est celle que l'APPELANT nomme, et ceci est une somme, pas une moyenne. Un
  // bin vide y ajoute zero, ce qui decrit exactement le signal recu ; rien
  // n'est extrapole a un domaine dont on a retire le contenu. Restreindre
  // rendrait une autre grandeur que celle demandee. Voir le commentaire de
  // declaration pour ce que cela implique d'une bande a cheval sur une coupure.
  const size_t bins = binCount();
  float sum = 0.0f;
  for (size_t k = 1; k < bins; k++) {
    const float hz = binToHz(k, sampleRate);
    if (hz >= loHz && hz <= hiHz) sum += _mag[k] * _mag[k];
  }
  return sum;
}

// ------------------------------------------------- Rapport harmonique / bruit --

namespace {

// Grille des raies d'une note : f0 exprimee en bins fractionnaires et nombre de
// rangs retenus. Regroupee ici pour que l'appartenance d'un bin soit definie a
// UN SEUL endroit - la somme d'energie et l'estimation du plancher doivent
// partager exactement le meme decoupage, sinon l'energie d'une raie serait
// comptee des deux cotes.
struct HarmonicGrid {
  float f0Bins = 0.0f;
  float invF0Bins = 0.0f;   // pre-calcule : la bissection reevalue chaque bin
  uint8_t partials = 0;

  HarmonicGrid(float bins, uint8_t n)
      : f0Bins(bins), invF0Bins(bins > 0.0f ? 1.0f / bins : 0.0f), partials(n) {}

  // Le bin 0 est le continu, deja retire par computeSpectrum : il n'appartient
  // ni aux raies ni au bruit.
  bool contains(size_t bin) const {
    if (bin == 0 || partials == 0 || f0Bins <= 0.0f) return false;
    // La grille est reguliere : le rang le plus proche s'obtient d'une division,
    // sans parcourir les rangs un par un. Le borner plutot que le rejeter evite
    // de classer en bruit un bin qui tombe dans la fenetre du dernier rang.
    int h = (int)((float)bin * invF0Bins + 0.5f);
    if (h < 1) h = 1;
    if (h > (int)partials) h = (int)partials;
    const float center = (float)h * f0Bins;
    const float d = (float)bin - center;
    return (d < 0.0f ? -d : d) <= SpectralHnr::kHarmonicHalfWidthBins;
  }
};

}  // namespace

SpectralAnalyzer::AnalysisBand SpectralAnalyzer::analysisBand(float sampleRate) {
  AnalysisBand band;
  const uint16_t lastBin = (uint16_t)(MIC_FFT_SIZE / 2);
  band.lo = 1;            // le bin 0 est le continu, deja retire par computeSpectrum
  band.hi = lastBin;
  if (!(sampleRate > 0.0f)) {
    band.hi = 0;          // bande vide : le refus est laisse a l'appelant
    return band;
  }

  // Passe-haut : premier bin dont le CENTRE atteint la coupure. Un bin dont le
  // centre est sous la coupure porte surtout l'attenuation du filtre.
  if (MIC_FILTER_HP_HZ > 0.0f) {
    const float b = hzToBin(MIC_FILTER_HP_HZ, sampleRate);
    if (b > (float)band.lo) {
      const float up = ceilf(b);
      band.lo = (up >= (float)lastBin) ? lastBin : (uint16_t)up;
    }
  }
  // Passe-bas : dernier bin dont le centre reste sous la coupure. Une coupure
  // au-dela de Nyquist ne retire rien, la borne reste le dernier bin.
  if (MIC_FILTER_LP_HZ > 0.0f) {
    const float b = hzToBin(MIC_FILTER_LP_HZ, sampleRate);
    const float down = floorf(b);
    if (down < (float)band.hi) band.hi = (down < 0.0f) ? 0u : (uint16_t)down;
  }
  return band;
}

uint8_t SpectralAnalyzer::usablePartials(float f0, float sampleRate) {
  if (f0 <= 0.0f || sampleRate <= 0.0f) return 0;
  const float f0Bins = hzToBin(f0, sampleRate);
  // Trop grave : la raie se confond avec le continu retire.
  if (f0Bins < SpectralHnr::kMinF0Bins) return 0;
  const float nyquistBin = (float)(MIC_FFT_SIZE / 2);
  // Au-dessus de Nyquist il n'y a pas de fondamentale, seulement un repli.
  if (f0Bins >= nyquistBin) return 0;
  int n = (int)(nyquistBin / f0Bins);
  if (n < 1) return 0;
  if (n > (int)SpectralHnr::kMaxPartials) n = (int)SpectralHnr::kMaxPartials;
  return (uint8_t)n;
}

bool SpectralAnalyzer::isHarmonicBin(size_t bin, float f0, float sampleRate) {
  const uint8_t partials = usablePartials(f0, sampleRate);
  if (partials == 0) return false;
  if (bin >= (size_t)(MIC_FFT_SIZE / 2 + 1)) return false;
  return HarmonicGrid(hzToBin(f0, sampleRate), partials).contains(bin);
}

float SpectralAnalyzer::medianNoisePower(const AnalysisBand& band, float f0Bins,
                                         uint8_t partials, uint16_t noiseBins,
                                         float minNoise, float maxNoise) const {
  if (noiseBins == 0) return 0.0f;
  if (maxNoise <= SpectralHnr::kPowerFloor) return SpectralHnr::kPowerFloor;
  if (minNoise < SpectralHnr::kPowerFloor) minNoise = SpectralHnr::kPowerFloor;

  const HarmonicGrid grid(f0Bins, partials);
  // Rang de la mediane basse, en numerotation 1.
  const uint16_t target = (uint16_t)((noiseBins + 1u) / 2u);

  // Bissection sur la VALEUR, menee dans le domaine logarithmique : un plancher
  // peut se trouver dix decades sous le maximum et une bissection lineaire y
  // perdrait toute precision des les premieres iterations. Trier serait exact
  // mais demanderait une copie des bins ; ici le seul cout est de relire le
  // spectre, qui est deja en RAM.
  //
  // Invariant : le predicat "au moins `target` bins sont sous le seuil" est
  // faux en `lo` et vrai en `hi`. S'il est deja vrai en `lo` - plus de la
  // moitie des bins au minimum, donc mediane confondue avec lui - `hi` converge
  // vers `lo`, ce qui est la reponse correcte.
  float lo = logf(minNoise);
  float hi = logf(maxNoise);
  for (uint8_t it = 0; it < SpectralHnr::kMedianIterations; it++) {
    const float mid = 0.5f * (lo + hi);
    const float threshold = expf(mid);
    uint16_t count = 0;
    // MEME parcours que le comptage de `noiseBins` dans harmonicNoiseRatio :
    // meme bande, meme grille. Un parcours plus large ferait chercher un rang
    // dans un echantillon qui n'est pas celui qu'on a compte.
    for (size_t k = band.lo; k <= (size_t)band.hi; k++) {
      if (grid.contains(k)) continue;
      if (_mag[k] * _mag[k] <= threshold) count++;
    }
    if (count >= target) hi = mid; else lo = mid;
  }
  return expf(hi);
}

HarmonicNoiseRatio SpectralAnalyzer::harmonicNoiseRatio(float f0,
                                                        float sampleRate) const {
  HarmonicNoiseRatio out;
  // Sans spectre calcule il n'y a rien a mesurer, et un HNR plausible serait
  // une invention pure.
  if (!_spectrumValid || sampleRate <= 0.0f) return out;

  const uint8_t partials = usablePartials(f0, sampleRate);
  if (partials == 0) return out;   // f0 absente, trop grave, ou hors Nyquist

  const HarmonicGrid grid(hzToBin(f0, sampleRate), partials);
  // TOUT ce qui suit se compte dans la seule bande que la chaine laisse
  // passer : raies, bruit et plancher. Comparer une energie de raies prise sur
  // tout le spectre a un plancher mesure dans la bande utile - ou l'inverse -
  // rapporterait deux grandeurs qui ne decrivent pas la meme partie du signal.
  const AnalysisBand band = analysisBand(sampleRate);
  if (band.count() == 0) return out;

  float harmonicEnergy = 0.0f;
  float maxNoise = 0.0f;
  float minNoise = 0.0f;
  uint16_t harmonicBins = 0;
  uint16_t noiseBins = 0;
  for (size_t k = band.lo; k <= (size_t)band.hi; k++) {
    const float p = _mag[k] * _mag[k];
    if (grid.contains(k)) {
      harmonicEnergy += p;
      harmonicBins++;
    } else {
      if (noiseBins == 0 || p < minNoise) minNoise = p;
      if (p > maxNoise) maxNoise = p;
      noiseBins++;
    }
  }

  out.partials = partials;
  out.harmonicBins = harmonicBins;
  out.noiseBins = noiseBins;
  out.harmonicEnergy = harmonicEnergy;

  // Un NaN venu du PCM contamine TOUT le spectre - chaque papillon melange tous
  // les echantillons - et traverse ensuite `< 0.0f`, `<= kPowerFloor` et
  // `> MIC_HNR_MAX_DB` sans en declencher aucun : sans ce refus la fonction
  // rendrait `valid = true, db = NaN`, ce que l'en-tete de HarmonicNoiseRatio
  // interdit explicitement. Le refus est TARDIF a dessein : les compteurs
  // ci-dessus restent renseignes, ils disent pourquoi.
  // Non atteignable depuis l'I2S, qui livre des entiers ; c'est un contrat, pas
  // un cas de terrain.
  if (!(harmonicEnergy >= 0.0f)) return out;

  // Silence numerique : ni raie ni bruit. Le rapport de deux riens n'existe
  // pas, et -40 dB serait un verdict sur une note qui n'a pas ete jouee.
  if (harmonicEnergy <= SpectralHnr::kPowerFloor &&
      maxNoise <= SpectralHnr::kPowerFloor) {
    return out;
  }

  // Trop peu de bins restants pour que la mediane veuille dire quelque chose.
  if (noiseBins < SpectralHnr::kMinNoiseBins) return out;

  float floorPerBin =
      medianNoisePower(band, grid.f0Bins, partials, noiseBins, minNoise, maxNoise) *
      SpectralHnr::kMedianToMeanPower;
  if (floorPerBin < SpectralHnr::kPowerFloor) floorPerBin = SpectralHnr::kPowerFloor;
  out.noiseFloorPerBin = floorPerBin;

  // Le bruit occupe toute la BANDE ANALYSEE, y compris sous les raies, qui le
  // masquent sans le supprimer : l'energie de bruit de la frame est le plancher
  // etendu a tous les bins de cette bande, et non aux seuls bins ou on a pu le
  // mesurer.
  //
  // L'extrapolation s'arrete LA. Au-dela de MIC_FILTER_LP_HZ et sous
  // MIC_FILTER_HP_HZ il n'y a pas de bruit a compter : le firmware l'a retire
  // lui-meme, et pretendre l'y retrouver reviendrait a mesurer son propre
  // filtre. Le HNR rendu est donc un rapport DANS LA BANDE UTILE - ce que les
  // harmoniques hors bande auraient apporte n'est compte d'aucun des deux
  // cotes, ce qui laisse le rapport interpretable.
  out.noiseEnergy = floorPerBin * (float)(harmonicBins + noiseBins);

  // Les bins d'une raie contiennent eux aussi du bruit. Le retirer evite de le
  // compter des deux cotes du rapport, ce qui flatterait les notes faibles.
  float harmonicNet = harmonicEnergy - floorPerBin * (float)harmonicBins;
  if (harmonicNet < 0.0f) harmonicNet = 0.0f;

  if (harmonicNet <= SpectralHnr::kPowerFloor) {
    // Rien ne depasse le plancher. C'est un resultat, pas une absence de
    // resultat : le signal n'a pas de contenu harmonique a cette f0.
    out.db = -MIC_HNR_MAX_DB;
  } else {
    out.db = 10.0f * log10f(harmonicNet / out.noiseEnergy);
    // Un signal synthetique sans bruit donnerait l'infini : on borne.
    if (out.db > MIC_HNR_MAX_DB) out.db = MIC_HNR_MAX_DB;
    if (out.db < -MIC_HNR_MAX_DB) out.db = -MIC_HNR_MAX_DB;
  }
  out.valid = true;
  return out;
}

#endif  // MIC_FFT_ENABLED
