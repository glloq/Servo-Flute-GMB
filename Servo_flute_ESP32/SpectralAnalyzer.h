/***********************************************************************************************
 * SpectralAnalyzer - Analyse frequentielle legere (Goertzel + FFT optionnelle)
 *
 * Aucune dependance materielle : la classe se teste entierement sur hote avec
 * des signaux synthetiques. Aucune allocation dynamique.
 *
 * DEUX CHEMINS, DEUX ROLES
 * ------------------------
 * Goertzel repond a la question "combien d'energie y a-t-il EXACTEMENT a cette
 * frequence ?". Quand la note attendue est connue, c'est tout ce qu'il faut pour
 * mesurer f0, 2f0, 3f0, 4f0 : le cout est de n operations par cible, soit
 * ~4 % du cout de YIN pour quatre harmoniques. C'est le chemin par defaut.
 *
 * La FFT repond a "a quoi ressemble tout le spectre ?" : centre de gravite,
 * platitude, energie hors harmoniques. Elle coute bien plus cher et n'est pas
 * necessaire a chaque frame - d'ou MIC_SPECTRAL_DECIMATION.
 *
 * "TOUT LE SPECTRE" S'ARRETE OU LA CHAINE S'ARRETE
 * ------------------------------------------------
 * Une FORME spectrale - centroide, platitude - est une moyenne sur des bins.
 * AudioAnalyzer::drainI2S() filtre le FLUX avant l'anneau, donc plus de la
 * moitie des bins que cette moyenne parcourrait ont ete vides par le firmware
 * lui-meme : moyenner dessus, c'est moyenner l'effet du filtre. Ces deux
 * mesures se font donc dans analysisBand(), exactement comme le plancher de
 * bruit du HNR. Les trois statistiques decrivent ainsi la MEME partie du
 * signal, et aucune ne bouge quand on change une coupure sans changer le son.
 *
 * bandEnergy() echappe a cette regle, et pour une raison de fond : c'est une
 * SOMME sur une bande que l'APPELANT nomme. Un bin vide y pese zero, ce qui est
 * une description exacte du signal recu ; et restreindre la bande demandee
 * repondrait a une autre question que celle posee. Voir son commentaire.
 *
 * FENETRAGE
 * ---------
 * Ici, contrairement a YIN, la fenetre de Hann est LEGITIME et necessaire : elle
 * limite les fuites entre bins d'un signal non periodique sur la fenetre. C'est
 * la distinction que l'audit PHASE 0 avait manquee - le fenetrage appartient au
 * domaine frequentiel, pas a un estimateur temporel de periode.
 *
 * Goertzel est applique SANS fenetre : on mesure une puissance a une frequence
 * connue, pas un spectre, et la fenetre ne ferait qu'attenuer le resultat d'un
 * facteur dependant de l'alignement.
 *
 * RAPPORT HARMONIQUE / BRUIT
 * --------------------------
 * harmonicNoiseRatio() mesure le HNR sur le spectre COMPLET, et non sur les
 * quatre raies de Goertzel. La difference n'est pas cosmetique : compter comme
 * "bruit" tout ce que quatre raies ne captent pas revient a compter les
 * harmoniques de rang superieur comme du souffle, donc a punir une note timbree
 * exactement comme une note soufflee. Les deux mesures sont alors du meme ordre
 * de grandeur et peuvent meme se croiser.
 *
 * Quatre decisions portent cette mesure :
 *
 *   1. Les bins d'une raie sont ceux du LOBE PRINCIPAL de la fenetre de Hann,
 *      soit +-2 bins autour de la raie. Une raie n'occupe jamais un seul bin ;
 *      l'ignorer reverserait ses flancs dans le bruit.
 *   2. Le plancher est une MEDIANE, jamais une moyenne. Une moyenne est tiree
 *      vers le haut par le moindre partiel residuel ou lobe secondaire ; la
 *      mediane, elle, ne bouge pas tant que moins de la moitie des bins sont
 *      contamines. C'est ce qui autorise a plafonner le nombre de rangs
 *      harmoniques traites sans fausser le resultat.
 *   3. La mediane est trouvee par BISSECTION sur la valeur, pas par tri : trier
 *      demanderait une copie des 257 bins (1 ko de pile) alors que la
 *      bissection ne demande rien du tout.
 *   4. Tout se mesure DANS LA BANDE QUE LA CHAINE LAISSE PASSER, jamais sur le
 *      spectre entier. AudioAnalyzer::drainI2S() filtre le FLUX avant l'anneau
 *      (AudioFilterChain, MIC_FILTER_HP_HZ..MIC_FILTER_LP_HZ) : la FFT ne voit
 *      donc jamais qu'un signal deja filtre, et a 32 kHz sur 512 points les
 *      bins 112 a 256 - 56 % du spectre - sont dans la bande coupee. Une
 *      mediane prise sur tous les bins non harmoniques y tombe (ils sont
 *      majoritaires), et l'etendre a la totalite du spectre sous-estime le
 *      bruit de plus de 12 dB, donc surestime le HNR d'autant. On ne peut pas
 *      extrapoler un plancher a une bande dont on a soi-meme retire le
 *      contenu : voir SpectralAnalyzer::analysisBand.
 *
 * Et une regle : sans spectre, sans f0 fiable ou sans assez de bins restants,
 * le resultat est explicitement INVALIDE. Aucun nombre plausible n'est rendu a
 * la place d'une mesure qui n'a pas pu etre faite.
 *
 * POURQUOI PAS ESP-DSP
 * --------------------
 * ESP-DSP offrirait une FFT plus rapide, mais rendrait cette classe
 * incompilable sur hote, donc non testable. Une FFT radix-2 de 512 points coute
 * ~4 600 papillons, ce qui reste modeste face aux 92 160 operations de YIN. Si
 * une mesure sur materiel montre que la FFT domine, elle pourra etre remplacee
 * derriere cette meme interface, sans toucher aux appelants ni aux tests.
 ***********************************************************************************************/
#ifndef SPECTRAL_ANALYZER_H
#define SPECTRAL_ANALYZER_H

#include <stddef.h>
#include <stdint.h>
#include "settings.h"

/***********************************************************************************************
 * Parametres de la mesure harmonique / bruit
 *
 * Aucune de ces valeurs n'est un reglage a l'oreille : chacune est fixee par
 * une propriete de la fenetre de Hann, de la chaine d'acquisition ou de la
 * statistique employee, et la raison est ecrite en face.
 ***********************************************************************************************/
namespace SpectralHnr {

// Demi-largeur, en bins FRACTIONNAIRES, de la fenetre attribuee a une raie.
// Le lobe principal d'une fenetre de Hann s'etend exactement sur +-2 bins
// autour de la raie : ses premiers zeros y tombent, et au-dela on est dans les
// lobes secondaires, 31 dB plus bas. Prendre moins laisserait les flancs de la
// raie dans le lot de bruit, c'est-a-dire commettre en petit l'erreur que
// cette mesure corrige en grand.
constexpr float kHarmonicHalfWidthBins = 2.0f;

// Rang harmonique maximal pris en compte. La chaine d'acquisition coupe deja a
// MIC_FILTER_LP_HZ (7 kHz) et un partiel de flute au-dela du 20e rang est sous
// le plancher. Continuer a exclure des bins plus haut ne retirerait plus
// d'harmonique et ne ferait que reduire l'echantillon servant au plancher. Les
// rares partiels eleves qui restent dans le lot de bruit sont sans effet : la
// mediane les ignore.
// Les rangs dont la raie tombe HORS de la bande passante de la chaine ne
// coutent plus rien depuis que la mesure se limite a cette bande : leurs bins
// n'y sont tout simplement pas.
constexpr uint8_t kMaxPartials = 20;

// Fondamentale minimale, exprimee en bins. En dessous, le lobe principal de la
// fondamentale se confond avec la region continue que computeSpectrum retire,
// et la mesure porterait sur autre chose que la note. A 32 kHz sur 512 points,
// 3 bins valent 187,5 Hz, soit sous MIC_PITCH_MIN_HZ : toute la plage utile de
// l'instrument passe.
constexpr float kMinF0Bins = 3.0f;

// Nombre minimal de bins restants pour estimer un plancher. Sous une quinzaine
// d'echantillons une mediane n'est plus une statistique stable ; on refuse la
// mesure plutot que de rendre un plancher tire de trois bins.
// Le comptage porte sur les bins non harmoniques DE LA BANDE PASSANTE, qui est
// nettement plus etroite que le spectre : a 32 kHz sur 512 points elle vaut
// 111 bins. Le pire cas de la plage de l'instrument - f0 = 375 Hz, ou vingt
// lobes de +-2 bins en recouvrent presque toute la largeur - laisse encore
// 20 bins. La marge est MINCE : un test la releve sur toute la plage
// MIC_PITCH_MIN_HZ..MIC_PITCH_MAX_HZ et echouerait avant que la mesure ne se
// mette a refuser silencieusement des notes.
constexpr uint16_t kMinNoiseBins = 16;

// Iterations de la bissection qui trouve la mediane sans trier ni copier.
// Elle opere sur le LOGARITHME de la puissance, car un plancher peut se
// trouver dix decades sous le maximum. L'intervalle de depart est l'ecart reel
// entre le bin de bruit le plus faible et le plus fort - de l'ordre de 60 dB
// sur un signal reel, 200 dB dans le pire cas synthetique. 12 halvings le
// ramenent a 0,015 dB, respectivement 0,05 dB : deux ordres de grandeur sous
// l'incertitude de la mesure elle-meme. C'est aussi le poste de calcul
// dominant de la fonction, d'ou l'interet de ne pas en faire plus.
constexpr uint8_t kMedianIterations = 12;

// La mediane d'un periodogramme de bruit ne vaut pas sa moyenne : les
// puissances par bin suivent une loi exponentielle, dont la mediane vaut ln(2)
// fois la moyenne. Ce facteur 1/ln(2) ramene la mediane mesuree a la PUISSANCE
// MOYENNE par bin, seule grandeur sommable sur le spectre. Sans lui le
// plancher serait systematiquement sous-estime de 1,6 dB, donc le HNR
// surestime d'autant.
constexpr float kMedianToMeanPower = 1.4426950409f;

// Plancher numerique commun. Un spectre synthetique contient des bins
// exactement nuls, et ni log(0) ni la division par zero n'existent. Cette
// valeur est tres en dessous de tout ce qu'une chaine 24 bits peut produire.
constexpr float kPowerFloor = 1e-20f;

}  // namespace SpectralHnr

// Energies harmoniques mesurees par Goertzel autour d'une fondamentale connue.
struct HarmonicEnergies {
  bool valid = false;         // f0 exploitable et harmoniques sous Nyquist
  float fundamental = 0.0f;   // puissance a f0
  float h2 = 0.0f;
  float h3 = 0.0f;
  float h4 = 0.0f;
  float harmonicTotal = 0.0f; // f0 + h2 + h3 + h4
  // Rapports a la fondamentale. 0 si la fondamentale est nulle.
  float h2Ratio = 0.0f;
  float h3Ratio = 0.0f;
  float h4Ratio = 0.0f;
  // Combien d'harmoniques sont reellement tombees sous Nyquist (1 a 4). Une
  // note aigue n'a pas quatre harmoniques mesurables a 32 kHz.
  uint8_t measured = 0;
};

// Rapport harmonique / bruit mesure sur le spectre COMPLET - c'est-a-dire sur
// tous les rangs harmoniques, et non sur quatre raies - mais DANS LA SEULE
// BANDE que la chaine d'acquisition laisse passer (voir analysisBand).
//
// `valid` faux signifie que la mesure n'a PAS pu etre faite - pas de spectre,
// pas de fondamentale exploitable, spectre numeriquement vide, spectre non
// fini, ou trop peu de bins restants pour un plancher. `db`,
// `noiseFloorPerBin` et `noiseEnergy`
// restent alors a zero et ne doivent pas etre lus : lire `db` sans regarder
// `valid` revient a prendre un zero pour un rapport de 0 dB. Les compteurs
// (`partials`, `harmonicBins`, `noiseBins`) et `harmonicEnergy`, eux, peuvent
// etre renseignes meme sur un refus tardif - ils servent justement a savoir
// POURQUOI la mesure a ete refusee.
//
// Les etapes intermediaires sont exposees parce qu'un HNR bas ne dit pas, a
// lui seul, s'il vient d'un plancher haut ou d'une energie harmonique faible.
// La structure est definie meme quand la FFT est compilee hors du binaire :
// un appelant peut ainsi en detenir une, restee invalide, sans compilation
// conditionnelle chez lui.
struct HarmonicNoiseRatio {
  bool valid = false;
  float db = 0.0f;                  // borne a +-MIC_HNR_MAX_DB
  float harmonicEnergy = 0.0f;      // somme des puissances des bins de raies
  float noiseFloorPerBin = 0.0f;    // puissance moyenne estimee d'UN bin de bruit
  float noiseEnergy = 0.0f;         // plancher etendu aux bins de la bande analysee
  uint16_t harmonicBins = 0;        // bins de raies DANS la bande analysee
  uint16_t noiseBins = 0;           // bins ayant servi a estimer le plancher
  uint8_t partials = 0;             // rangs harmoniques pris en compte
};

class SpectralAnalyzer {
public:
  SpectralAnalyzer() { buildWindow(); }

  // --- Goertzel (chemin par defaut) ----------------------------------------

  // Puissance du signal a `targetHz`. Fonction PURE : ne modifie rien.
  // Retourne 0 si la cible est hors de ]0, Nyquist[.
  static float goertzelPower(const float* x, size_t n, float targetHz,
                             float sampleRate = (float)MIC_SAMPLE_RATE);

  // Energies de f0, 2f0, 3f0, 4f0. Les harmoniques au-dessus de Nyquist sont
  // ignorees et `measured` dit combien ont reellement ete mesurees.
  static HarmonicEnergies harmonics(const float* x, size_t n, float f0,
                                    float sampleRate = (float)MIC_SAMPLE_RATE);

  // Energie totale de la frame (somme des carres, continu retire). Sert de
  // denominateur aux rapports harmonique / bruit.
  static float totalPower(const float* x, size_t n);

#if MIC_FFT_ENABLED
  // --- FFT (chemin spectral complet) ---------------------------------------

  // Calcule le spectre d'amplitude des MIC_FFT_SIZE premiers echantillons
  // (fenetre de Hann appliquee). Retourne false si n < MIC_FFT_SIZE.
  bool computeSpectrum(const float* x, size_t n);

  bool hasSpectrum() const { return _spectrumValid; }
  size_t binCount() const { return MIC_FFT_SIZE / 2 + 1; }
  const float* magnitudes() const { return _mag; }
  static float binToHz(size_t bin, float sampleRate = (float)MIC_SAMPLE_RATE) {
    return (float)bin * sampleRate / (float)MIC_FFT_SIZE;
  }

  // Centre de gravite spectral (Hz) : ou se situe "en moyenne" l'energie. Un
  // son souffle le fait monter, une note pleine le garde bas.
  //
  // Mesure DANS LA BANDE ANALYSEE (voir analysisBand). Le centroide est une
  // moyenne ponderee par l'energie, donc un bin vide n'y pese presque rien -
  // le biais est plus doux que sur la platitude, mais il est du meme signe et
  // il est grand : sur du bruit blanc, le centroide vaut 8300 Hz sur le PCM
  // brut et 4636 Hz apres AudioFilterChain, soit -44 % pour un signal
  // inchange. Le filtre a RETIRE cette energie-la, donc il n'y a rien a en
  // deduire sur la source ; dans la bande, les memes signaux donnent 3582 Hz
  // brut et 3444 Hz filtre, soit -4 %.
  // CONSEQUENCE D'ECHELLE, assumee : la valeur rendue vit desormais dans
  // [MIC_FILTER_HP_HZ, MIC_FILTER_LP_HZ] et non dans [0, Nyquist]. Un son dont
  // toute l'energie serait au-dessus du passe-bas rend le centroide de ce qui
  // reste dans la bande, et non sa propre frequence - c'est le prix a payer
  // pour que la mesure ne depende plus du filtre, et la plage utile de
  // l'instrument (MIC_PITCH_MIN_HZ..MIC_PITCH_MAX_HZ) est entierement dedans.
  float spectralCentroid(float sampleRate = (float)MIC_SAMPLE_RATE) const;

  // Platitude spectrale 0..1 (moyenne geometrique / moyenne arithmetique).
  // Proche de 1 = bruit large bande, proche de 0 = spectre a raies.
  //
  // Mesure DANS LA BANDE ANALYSEE (voir analysisBand), et c'est ici que cela
  // compte le plus : la moyenne geometrique passe par un LOGARITHME, donc un
  // bin vide y pese enormement - log(plancher) est un grand nombre negatif -
  // alors qu'il ne porte aucune energie. Sur les 56 % de bins que la chaine
  // vide, ce sont donc des bins SANS signal qui dominaient le vote.
  // Consequence mesuree : du bruit blanc pur - le signal le plus plat qui
  // existe - lisait 0,85 sur PCM brut et 0,39 apres AudioFilterChain. Le seuil
  // AQ_BREATH_FLATNESS_NOISE, place a 0,70, etait donc devenu inatteignable et
  // la composante platitude de computeBreathiness() ne pouvait plus jamais
  // declarer "du bruit". Dans la bande, le meme bruit lit 0,84 a 0,87 filtre
  // contre 0,85 a 0,88 brut : la mesure ne voit plus le filtre.
  float spectralFlatness(float sampleRate = (float)MIC_SAMPLE_RATE) const;

  // Energie dans une bande [loHz, hiHz].
  //
  // PAS de restriction a analysisBand, DELIBEREMENT. Les deux mesures
  // ci-dessus sont des moyennes sur un domaine que la fonction choisit
  // elle-meme, et ce domaine etait faux ; ici le domaine est un ARGUMENT, et
  // c'est une somme : un bin vide y ajoute zero, ce qui decrit exactement le
  // signal recu, sans rien extrapoler. Restreindre repondrait a une question
  // que l'appelant n'a pas posee, et effacerait silencieusement la partie
  // hors bande d'une bande demandee a cheval sur une coupure.
  // A SAVOIR quand meme : une bande a cheval sur MIC_FILTER_HP_HZ ou
  // MIC_FILTER_LP_HZ rend une energie amputee du flanc du filtre. DEUX des six
  // bandes de NoiseModel sont dans ce cas, la premiere et la derniere. Ecart
  // brut / production sur du bruit blanc, pire cas sur six realisations :
  //
  //     100-250 Hz  -56 %   |  250-500  -7 %   |  500-1000  -2 %
  //     1k-2k       +1 %    |  2k-4k    -2 %   |  4k-8k     -38 %
  //
  // La derniere (4k-8k) perd regulierement 30 a 38 % : c'est le passe-bas. La
  // premiere (100-250) est plus traitre - elle ne compte que TROIS bins a
  // 32 kHz sur 512 points, si bien que son ecart va de -56 % a +24 % selon la
  // realisation : c'est le flanc du passe-haut ET un echantillon minuscule.
  // Comparer deux profils de bruit reste juste - ils traversent la MEME chaine
  // - mais lire l'une de ces deux bandes comme une energie acoustique absolue
  // ne l'est pas, et la premiere ne devrait pas etre lue seule du tout.
  float bandEnergy(float loHz, float hiHz,
                   float sampleRate = (float)MIC_SAMPLE_RATE) const;

  // --- Rapport harmonique / bruit ------------------------------------------

  // Position FRACTIONNAIRE d'une frequence dans le spectre : l'inverse exact de
  // binToHz. Une raie ne tombe pratiquement jamais sur un bin entier, et
  // arrondir avant de mesurer decalerait la fenetre d'une demi-largeur.
  static float hzToBin(float hz, float sampleRate = (float)MIC_SAMPLE_RATE) {
    return hz * (float)MIC_FFT_SIZE / sampleRate;
  }

  // Bornes INCLUSIVES, en bins, de la bande sur laquelle le HNR, le centroide
  // et la platitude sont mesures.
  //
  // POURQUOI CETTE BANDE EXISTE. Le HNR compare une energie de raies a un
  // plancher de bruit EXTRAPOLE : on mesure le plancher sur les bins ou aucune
  // raie ne se trouve, puis on l'etend aux bins de raies, ou il est invisible.
  // Cette extrapolation suppose que les bins sur lesquels on mesure et les bins
  // auxquels on etend portent le MEME bruit. La chaine d'acquisition rend cette
  // hypothese fausse hors de sa bande passante : AudioAnalyzer::drainI2S()
  // filtre le flux (MIC_FILTER_HP_HZ..MIC_FILTER_LP_HZ) avant l'anneau, donc la
  // FFT ne voit au-dela qu'un bruit que le firmware a lui-meme retire. Y
  // mesurer le plancher, c'est mesurer l'effet du filtre ; l'etendre au reste
  // du spectre, c'est pretendre que la bande utile est aussi vide.
  //
  // Le meme raisonnement vaut, mot pour mot, pour toute STATISTIQUE prise en
  // moyenne sur les bins : la platitude (moyenne geometrique) et le centroide
  // (moyenne ponderee) decrivaient eux aussi un domaine dont le firmware avait
  // retire le contenu. Ils utilisent donc cette meme bande.
  //
  // La bande est donc celle du filtre, bornes a -3 dB comprises, et elle se
  // DERIVE de settings.h : aucune frequence n'est ecrite ici. Une coupure a 0
  // desactive l'etage correspondant (AudioFilterChain::configure : la cellule
  // devient transparente), et la borne correspondante revient alors au spectre
  // entier - la mesure reste donc exacte sans filtrage.
  //
  // CE QUI CASSERAIT CETTE BANDE. Elle suppose que la chaine reellement
  // appliquee au flux est celle de settings.h. AudioAnalyzer::begin() appelle
  // configureDefaults(), donc c'est vrai en production ; cela cesserait de
  // l'etre si quelqu'un configurait AudioFilterChain avec d'autres coupures a
  // l'execution, ou analysait a une frequence d'echantillonnage differente de
  // celle a laquelle le filtre a ete regle. Dans ces deux cas les mesures
  // redeviendraient un melange de signal et de filtre, SANS que rien ne le
  // signale - c'est le point faible connu de cette correction.
  //
  // `sampleRate` est celle des echantillons analyses ; la chaine de filtrage
  // etant configuree a MIC_SAMPLE_RATE sur le meme flux, les deux coincident en
  // production (AudioAnalyzer::begin -> configureDefaults()).
  struct AnalysisBand {
    uint16_t lo = 1;     // premier bin analyse (jamais 0 : le continu est retire)
    uint16_t hi = 0;     // dernier bin analyse, INCLUS ; hi < lo = bande vide
    uint16_t count() const { return (hi >= lo) ? (uint16_t)(hi - lo + 1u) : 0u; }
    bool contains(size_t bin) const { return bin >= lo && bin <= hi; }
  };
  static AnalysisBand analysisBand(float sampleRate = (float)MIC_SAMPLE_RATE);

  // Rangs harmoniques exploitables pour cette f0 : ceux dont la raie tombe sous
  // Nyquist, plafonnes a SpectralHnr::kMaxPartials. Rend 0 quand f0 est
  // inutilisable, ce qui est aussi la condition de refus du HNR.
  static uint8_t usablePartials(float f0,
                                float sampleRate = (float)MIC_SAMPLE_RATE);

  // Vrai si `bin` appartient au lobe principal d'une des raies de f0. Expose
  // pour que l'attribution des bins - l'etape ou une erreur de largeur passe le
  // plus facilement inapercue - se teste seule, sans spectre calcule.
  static bool isHarmonicBin(size_t bin, float f0,
                            float sampleRate = (float)MIC_SAMPLE_RATE);

  // HNR mesure sur le spectre complet deja calcule par computeSpectrum().
  // Fonction CONSTANTE : elle ne modifie pas le spectre et peut donc etre
  // appelee plusieurs fois, avec des f0 differentes, sur la meme frame.
  HarmonicNoiseRatio harmonicNoiseRatio(
      float f0, float sampleRate = (float)MIC_SAMPLE_RATE) const;
#endif  // MIC_FFT_ENABLED

private:
#if MIC_FFT_ENABLED
  // Mediane des puissances des bins NON harmoniques DE LA BANDE ANALYSEE, par
  // bissection sur la valeur : aucun tampon de travail, aucune copie du
  // spectre. `minNoise` et `maxNoise` encadrent la recherche - la mediane est
  // par definition entre les deux, ce qui evite de bissecter sur un intervalle
  // imaginaire. `band` doit etre exactement celle qui a servi a compter
  // `noiseBins`, sinon le rang cherche ne correspond plus a l'echantillon.
  float medianNoisePower(const AnalysisBand& band, float f0Bins, uint8_t partials,
                         uint16_t noiseBins, float minNoise, float maxNoise) const;

  float _re[MIC_FFT_SIZE];
  float _im[MIC_FFT_SIZE];
  float _mag[MIC_FFT_SIZE / 2 + 1];
  // Demi-table de Hann : w[i] = w[N-1-i], donc N/2 valeurs suffisent.
  float _window[MIC_FFT_SIZE / 2];
  bool _spectrumValid = false;

  void buildWindow();
  void fftInPlace();
  float windowAt(size_t i) const {
    return (i < MIC_FFT_SIZE / 2) ? _window[i] : _window[MIC_FFT_SIZE - 1 - i];
  }
#else
  void buildWindow() {}
#endif
};

#endif  // SPECTRAL_ANALYZER_H
