#include "AcousticTiming.h"

#include <math.h>

#include "AcousticFeatures.h"
#include "PitchMath.h"

using namespace AcousticTimingCfg;

namespace {

// Comparaison d'instants SURE AU DEBORDEMENT. millis() se replie toutes les
// 49,7 jours ; sur ESP32 `unsigned long` fait 32 bits, sur l'hote 64. Le cast
// explicite en int32_t est la seule ecriture qui donne le meme resultat des
// deux cotes, parce qu'il force le calcul a se faire sur 32 bits signes - la
// difference de deux instants encadrant le repliement reste alors petite et
// positive, au lieu de valoir environ 4,29 milliards.
inline int32_t timeDelta(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
inline bool timeReached(uint32_t now, uint32_t deadline) { return timeDelta(now, deadline) >= 0; }

// Duree ecoulee. N'a de sens que si `now` suit bien `from` : l'appelant doit
// avoir verifie timeDelta(now, from) >= 0 auparavant.
inline uint32_t elapsed(uint32_t now, uint32_t from) { return (uint32_t)(now - from); }

// Remplit une mesure, ou la laisse INVALIDE si les deux instants ne sont pas
// dans l'ordre. Une duree negative n'est pas une duree : la rendre a zero
// serait exactement la valeur la plus trompeuse possible (elle se lirait comme
// une latence nulle).
inline void setMeasure(TimingMeasure& m, bool haveFrom, uint32_t from,
                       bool haveTo, uint32_t to) {
  if (!haveFrom || !haveTo) return;
  if (timeDelta(to, from) < 0) return;
  m.ms = elapsed(to, from);
  m.valid = true;
}

// Ecart entre deux frequences en cents. Les deux doivent etre strictement
// positives.
inline float centsBetween(float a, float b) {
  return PitchMath::kCentsPerOctave * log2f(a / b);
}

// Plus petite variation de niveau consideree comme une pente exploitable pour
// l'interpolation. En dessous, la droite qui relie les deux frames est
// horizontale a la precision du float et sa pente ne dit rien.
constexpr float kFlatSlopeDb = 1e-4f;

// Compteur de diagnostic SATURANT. Un compteur qui reboucle a zero apres 65535
// refus afficherait "tout va bien" au moment precis ou tout va mal.
inline void bump(uint16_t& counter) {
  if (counter < 0xFFFFu) counter++;
}

}  // namespace

/*----------------------------------------------------------------------------
 * Cycle de vie
 *--------------------------------------------------------------------------*/

void AcousticTiming::reset() {
  _state = TIMING_IDLE;
  _note.reset();
  _last.reset();
  _hasLast = false;
  _interpolate = true;
  _rejectedFrames = 0;
  _rejectedEvents = 0;
  _havePrev = false;
  _prevTs = 0;
  _prevDb = MIC_DBFS_FLOOR;
  _baselineCount = 0;
  _baselineIdx = 0;
  for (uint8_t i = 0; i < kBaselineFrames; i++) _baseline[i] = MIC_DBFS_FLOOR;
  clearNoteTracking();
}

void AcousticTiming::clearNoteTracking() {
  _confirmCount = 0;
  _candidateTs = 0;
  _plateauCount = 0;
  _plateauIdx = 0;
  _pitchCount = 0;
  _pitchIdx = 0;
  _maxSinceOnset = MIC_DBFS_FLOOR;
  _sustainMeanDb = MIC_DBFS_FLOOR;
  _sustainFrames = 0;
  _cutShort = false;
  _releaseThresholdDb = MIC_DBFS_FLOOR;
  for (uint8_t i = 0; i < kAttackStableFrames; i++) { _plateauDb[i] = MIC_DBFS_FLOOR; _plateauTs[i] = 0; }
  for (uint8_t i = 0; i < kPitchStableFrames; i++) { _pitchHz[i] = 0.0f; _pitchTs[i] = 0; }
}

void AcousticTiming::closeNote(TimingOutcome outcome) {
  _note.outcome = outcome;
  _last = _note;
  _hasLast = true;
  _note.reset();
  _state = TIMING_IDLE;
  clearNoteTracking();
}

/*----------------------------------------------------------------------------
 * Plancher d'avant-note
 *
 * Le plancher n'est alimente QUE hors suivi de note. Pendant une note, les
 * frames portent le son lui-meme : les laisser entrer ferait monter le
 * plancher de la note SUIVANTE jusqu'au niveau de la precedente, et son seuil
 * d'attaque avec lui. Le prix de ce choix est qu'en jeu enchaine, sans silence
 * entre deux notes, le plancher retenu date d'avant la note precedente. C'est
 * une mesure ancienne, mais c'est toujours une mesure de PLANCHER, jamais le
 * niveau d'une note - et un plancher trop haut fait echouer la detection de
 * facon visible (TIMING_NO_SOUND) au lieu de produire une mesure fausse.
 *--------------------------------------------------------------------------*/

void AcousticTiming::pushBaseline(float db) {
  _baseline[_baselineIdx] = db;
  _baselineIdx = (uint8_t)((_baselineIdx + 1) % kBaselineFrames);
  if (_baselineCount < kBaselineFrames) _baselineCount++;
}

float AcousticTiming::baselineMean() const {
  if (_baselineCount == 0) return MIC_DBFS_FLOOR;
  float sum = 0.0f;
  for (uint8_t i = 0; i < _baselineCount; i++) sum += _baseline[i];
  return sum / (float)_baselineCount;
}

/*----------------------------------------------------------------------------
 * Instant de franchissement d'un seuil
 *
 * Sans interpolation, l'instant retenu serait celui de la frame qui a franchi,
 * donc cale sur la grille de 16 ms. Les deux frames qui encadrent le
 * franchissement portent chacune un niveau en dB ; le seuil est lui aussi en
 * dB. Interpoler lineairement DANS CE DOMAINE place le franchissement a sa
 * place, et pas systematiquement a la fin de l'intervalle.
 *
 * Fonctionne dans les deux sens : montee (prev < seuil <= cur) comme descente
 * (prev > seuil >= cur), le signe de la pente s'annulant dans le rapport.
 *--------------------------------------------------------------------------*/

uint32_t AcousticTiming::crossingInstant(uint32_t curTs, float curDb, float thresholdDb) const {
  if (!_interpolate || !_havePrev) return curTs;

  const int32_t d = timeDelta(curTs, _prevTs);
  if (d <= 0) return curTs;
  const uint32_t dt = (uint32_t)d;
  // Trou dans le flux : la forme de l'enveloppe entre les deux frames est
  // inconnue. On ne l'invente pas, on retombe sur la quantification.
  if (dt > kMaxInterpolationGapMs) return curTs;

  const float slope = curDb - _prevDb;
  if (fabsf(slope) < kFlatSlopeDb) return curTs;

  float frac = (thresholdDb - _prevDb) / slope;
  if (!(frac > 0.0f)) return _prevTs;      // capture aussi NaN
  if (frac > 1.0f) return curTs;

  return _prevTs + (uint32_t)lroundf(frac * (float)dt);
}

/*----------------------------------------------------------------------------
 * Evenements d'ordre
 *--------------------------------------------------------------------------*/

bool AcousticTiming::frameStreamIsLive(uint32_t nowMs) const {
  // "Le flux est mort" n'est PAS redefini ici. C'est le plafond d'obsolescence
  // que le firmware possede deja - MIC_FRAME_STALE_MS - et c'est le meme test,
  // au sens de comparaison pres, que celui qui fait invalider les mesures dans
  // AudioAnalyzer::update() : une seule definition de la notion, pas deux.
  // La comparaison passe par timeDelta, donc elle survit au repliement ; un
  // ecart NEGATIF (une frame plus recente que l'ordre) dit que le flux est bien
  // vivant, pas qu'il est perime.
  if (!_havePrev) return false;
  return timeDelta(nowMs, _prevTs) <= (int32_t)MIC_FRAME_STALE_MS;
}

void AcousticTiming::noteCommanded(unsigned long nowMs) {
  // Conversion a l'entree : `unsigned long` fait 64 bits sur l'hote et 32 sur
  // ESP32. Replier ici, et non a la comparaison, garantit que le test natif
  // exerce le MEME repliement que la cible.
  const uint32_t t = (uint32_t)nowMs;

  // PERSONNE N'ECOUTE : on n'ouvre pas de cycle. L'analyseur est INACTIF par
  // defaut (moniteur micro eteint), les quatre hooks d'ordre tournent malgre
  // tout a chaque note, et aucune frame n'entre jamais. La machine concluait
  // quand meme - noteReleased() en attente de son clot la note en
  // TIMING_NO_SOUND - et /api/diagnostics publiait "l'instrument n'a pas
  // produit de son" pour chaque note jouee, alors que la verite est "personne
  // n'ecoutait". Aucune mesure n'etait fausse, mais le VERDICT, lui, l'etait.
  // Le refus est place ici, a l'ouverture, et non dans l'appelant : un
  // detachement cote NoteSequencer serait oublie a la premiere evolution,
  // alors que la machine, elle, sait toujours si des frames lui arrivent.
  // On compte le refus dans _rejectedEvents (un ordre refuse se diagnostique),
  // et on ne touche NI _last NI _hasLast : un refus ne fabrique pas de verdict,
  // et n'efface pas non plus celui d'une vraie note precedente. Une note deja
  // en cours reste en cours : la fermer ici serait une conclusion, alors qu'on
  // vient precisement de constater qu'on ne peut plus rien conclure.
  if (!frameStreamIsLive(t)) {
    bump(_rejectedEvents);
    return;
  }

  // Une note en remplace une autre, elle ne la prolonge pas. L'ancienne est
  // rangee telle quelle : ses mesures deja faites restent lisibles, celles qui
  // n'ont pas abouti restent invalides.
  if (_state != TIMING_IDLE) closeNote(TIMING_ABORTED);

  _note.reset();
  clearNoteTracking();
  _note.outcome = TIMING_IN_PROGRESS;
  _note.hasNoteCommand = true;
  _note.noteCommandTimestamp = t;

  // Le plancher est FIGE ici. S'il continuait a suivre les frames, il monterait
  // avec le son qui arrive et le seuil d'attaque fuirait devant lui : l'attaque
  // ne serait jamais franchie.
  _note.baselineValid = (_baselineCount > 0);
  _note.baselineDbFS = baselineMean();

  // Repli EXPLICITE quand aucune frame n'a ete vue avant l'ordre : le critere
  // relatif est alors impossible, seul le plancher absolu s'applique.
  const float floorDb = onsetFloorDbFS();
  const float relative = _note.baselineDbFS + kOnsetRiseDb;
  _note.onsetThresholdDbFS = _note.baselineValid ? ((relative > floorDb) ? relative : floorDb)
                                                 : floorDb;
  _state = TIMING_WAIT_ONSET;
}

bool AcousticTiming::airCommanded(unsigned long nowMs) {
  const uint32_t t = (uint32_t)nowMs;
  // Recevable uniquement entre l'ordre MIDI et l'apparition du son, et jamais
  // anterieur a l'ordre MIDI : une consigne d'air posee apres que le son sonne
  // n'est pas celle qui l'a fait sonner. Seule la PREMIERE compte, c'est elle
  // qui a lance l'air.
  if (_state != TIMING_WAIT_ONSET || _note.hasAirCommand ||
      timeDelta(t, _note.noteCommandTimestamp) < 0) {
    bump(_rejectedEvents);
    return false;
  }
  _note.hasAirCommand = true;
  _note.airCommandTimestamp = t;
  return true;
}

bool AcousticTiming::valveOpened(unsigned long nowMs) {
  const uint32_t t = (uint32_t)nowMs;
  if (_state != TIMING_WAIT_ONSET || _note.hasValveOpen ||
      timeDelta(t, _note.noteCommandTimestamp) < 0) {
    bump(_rejectedEvents);
    return false;
  }
  _note.hasValveOpen = true;
  _note.valveOpenTimestamp = t;
  return true;
}

bool AcousticTiming::noteReleased(unsigned long nowMs) {
  const uint32_t t = (uint32_t)nowMs;
  if (_state != TIMING_WAIT_ONSET && _state != TIMING_ATTACK && _state != TIMING_SUSTAIN) {
    bump(_rejectedEvents);
    return false;
  }
  if (timeDelta(t, _note.noteCommandTimestamp) < 0) {
    bump(_rejectedEvents);
    return false;
  }

  _note.hasReleaseCommand = true;
  _note.releaseCommandTimestamp = t;

  if (_state == TIMING_WAIT_ONSET) {
    // Coupee avant d'avoir sonne. Il n'y a pas de son a faire disparaitre :
    // releaseTime reste INVALIDE, ce qui est la seule reponse honnete.
    closeNote(TIMING_NO_SOUND);
    return true;
  }

  // Niveau de reference du critere de chute. En regime etabli, c'est la moyenne
  // du palier : plus robuste qu'un maximum, qu'un transitoire d'attaque
  // suffirait a tirer trop haut. Sans palier (note coupee en pleine attaque),
  // le maximum atteint depuis l'apparition est ce qui existe de mieux.
  _cutShort = (_state == TIMING_ATTACK);
  _note.referenceDbFS = (_sustainFrames > 0) ? _sustainMeanDb : _maxSinceOnset;
  _releaseThresholdDb = _note.referenceDbFS - kReleaseFallDb;
  _confirmCount = 0;
  _state = TIMING_RELEASING;
  return true;
}

/*----------------------------------------------------------------------------
 * Suivi du pitch
 *--------------------------------------------------------------------------*/

void AcousticTiming::trackPitch(const TimingFrame& f) {
  if (!_note.hasSoundOnset) return;

  // Plafond : un pitch qui n'a pas tenu kPitchStableFrames frames au bout de
  // kPitchTimeoutMs ne s'est pas stabilise. On cesse de chercher, et
  // pitchStabilizationTime reste invalide - aucune valeur de repli.
  if (!_note.hasPitchStable &&
      timeReached(f.timestampMs, _note.soundOnsetTimestamp + kPitchTimeoutMs)) {
    return;
  }

  if (!f.pitchValid || !(f.pitchHz > 0.0f)) {
    // La fenetre de stabilite exige des frames CONSECUTIVES : une frame sans
    // pitch fiable casse la serie. Tolerer les trous reviendrait a declarer
    // stable un pitch qui disparait et revient.
    _pitchCount = 0;
    _pitchIdx = 0;
    return;
  }

  // Premier pitch fiable de la note. Pas d'interpolation ici : la detection est
  // binaire (le pitch sort ou ne sort pas), il n'y a aucune grandeur continue a
  // interpoler. Cet instant reste donc quantifie a kFramePeriodMs, et c'est
  // dit.
  if (!_note.hasPitchDetected) {
    _note.hasPitchDetected = true;
    _note.pitchDetectedTimestamp = f.timestampMs;
  }

  if (_note.hasPitchStable) return;

  _pitchHz[_pitchIdx] = f.pitchHz;
  _pitchTs[_pitchIdx] = f.timestampMs;
  _pitchIdx = (uint8_t)((_pitchIdx + 1) % kPitchStableFrames);
  if (_pitchCount < kPitchStableFrames) _pitchCount++;
  if (_pitchCount < kPitchStableFrames) return;

  float lo = _pitchHz[0], hi = _pitchHz[0];
  for (uint8_t i = 1; i < kPitchStableFrames; i++) {
    if (_pitchHz[i] < lo) lo = _pitchHz[i];
    if (_pitchHz[i] > hi) hi = _pitchHz[i];
  }
  if (!(lo > 0.0f)) return;
  if (centsBetween(hi, lo) > kPitchStableCents) return;

  // L'anneau est plein : la case ou l'on ECRIRA ensuite contient la plus
  // ancienne valeur, donc le debut de la fenetre stable. C'est cet instant qui
  // est retenu, et non celui de la derniere frame : la stabilite a commence au
  // debut de la fenetre, pas a sa fin.
  _note.hasPitchStable = true;
  _note.pitchStableTimestamp = _pitchTs[_pitchIdx];
  setMeasure(_note.pitchStabilizationTime, true, _note.soundOnsetTimestamp,
             true, _note.pitchStableTimestamp);
}

/*----------------------------------------------------------------------------
 * Critere de plateau : le niveau est-il etabli ?
 *
 * Le niveau est dit etabli quand son amplitude crete-a-crete reste sous
 * kAttackSettleDb sur kAttackStableFrames frames consecutives. C'est un critere
 * CAUSAL : il ne demande pas de connaitre le niveau final de la note, ce qu'un
 * analyseur temps reel ne peut pas savoir. Son defaut connu est l'autre face de
 * cette qualite - une attaque plus lente que la tolerance sur la fenetre est
 * indiscernable d'un palier (voir kAttackStableFrames).
 *--------------------------------------------------------------------------*/

bool AcousticTiming::plateauReached(uint32_t& atTs) {
  if (_plateauCount < kAttackStableFrames) return false;
  float lo = _plateauDb[0], hi = _plateauDb[0];
  for (uint8_t i = 1; i < kAttackStableFrames; i++) {
    if (_plateauDb[i] < lo) lo = _plateauDb[i];
    if (_plateauDb[i] > hi) hi = _plateauDb[i];
  }
  if ((hi - lo) > kAttackSettleDb) return false;
  atTs = _plateauTs[_plateauIdx];   // plus ancienne case = debut du palier
  return true;
}

/*----------------------------------------------------------------------------
 * Flux d'analyse
 *--------------------------------------------------------------------------*/

bool AcousticTiming::update(const TimingFrame& f) {
  const uint32_t ts = f.timestampMs;

  // Le temps ne recule pas. Attention : un repliement de millis() n'est PAS un
  // recul - en arithmetique 32 bits, 0x00000010 suit bien 0xFFFFFFF0, et
  // timeDelta le voit positif. Seul un vrai desordre est rejete.
  if (_havePrev && timeDelta(ts, _prevTs) < 0) {
    bump(_rejectedFrames);
    // POURQUOI la reference est reprise sur la frame REFUSEE : une garde "le
    // temps ne recule pas" qui laisse sa reference figee transforme un
    // echantillon aberrant en panne durable. L'ecart etant lu sur 32 bits
    // SIGNES, tout ecart reel de 24,86 a 49,71 jours se presente comme un
    // recul : sans resynchronisation, la premiere frame d'apres une telle
    // pause - moniteur micro rallume trois semaines plus tard - faisait
    // rejeter TOUTES les suivantes jusqu'a ce que l'ecart cumule repasse sous
    // le seuil, environ 25 jours plus tard. On repart donc du present. La
    // frame reste REFUSEE (return false) : c'est son horodatage qui devient la
    // reference, jamais son contenu, qui n'alimente aucune mesure.
    // _prevDb suit _prevTs dans le meme geste : les deux forment le point bas
    // de l'interpolation, et les desapparier daterait le franchissement suivant
    // avec un niveau qui n'est plus celui de cet instant-la.
    _prevTs = ts;
    _prevDb = f.rmsDbFS;
    // La note en cours est ABANDONNEE, pas conclue. Apres un tel saut, plus
    // aucun instant deja date n'est comparable a ceux qui suivront : la garder
    // en l'etat produirait des durees calculees entre deux horloges
    // differentes. TIMING_ABORTED dit exactement cela - "suivi perdu" - la ou
    // TIMING_NO_SOUND ou TIMING_TIMEOUT affirmeraient quelque chose sur
    // l'instrument. Ce que la note avait deja mesure reste lisible dans last(),
    // chaque mesure avec son propre drapeau ; rien n'est fabrique.
    if (_state != TIMING_IDLE) closeNote(TIMING_ABORTED);
    return false;
  }

  // --- Plafonds : rien ne reste "en cours" indefiniment ---------------------
  if (_state != TIMING_IDLE) {
    if (_state == TIMING_WAIT_ONSET &&
        timeReached(ts, _note.noteCommandTimestamp + kOnsetTimeoutMs)) {
      // Le son n'est jamais apparu. Aucune latence n'est inventee.
      closeNote(TIMING_NO_SOUND);
    } else if (_state == TIMING_RELEASING &&
               timeReached(ts, _note.releaseCommandTimestamp + kReleaseTimeoutMs)) {
      // Le son n'a jamais disparu : valve bloquee, air non coupe. ABANDON
      // explicite, releaseTime reste invalide.
      closeNote(TIMING_TIMEOUT);
    } else if (timeReached(ts, _note.noteCommandTimestamp + kNoteMaxDurationMs)) {
      // Garde-fou de la machine a etats : un Note Off perdu ne doit pas la
      // laisser armee jusqu'au redemarrage.
      closeNote(TIMING_TIMEOUT);
    }
  }

  switch (_state) {
    case TIMING_IDLE:
      break;

    case TIMING_WAIT_ONSET: {
      // Une frame dont la fenetre s'est fermee AVANT l'ordre ne peut pas
      // contenir le son que l'ordre a provoque. Elle sert quand meme de point
      // bas a l'interpolation.
      if (timeDelta(ts, _note.noteCommandTimestamp) < 0) break;

      if (f.rmsDbFS >= _note.onsetThresholdDbFS) {
        if (_confirmCount == 0) {
          _candidateTs = crossingInstant(ts, f.rmsDbFS, _note.onsetThresholdDbFS);
          // L'interpolation peut placer le franchissement juste avant l'ordre
          // (la frame precedente lui est anterieure). Le son ne peut pas
          // preceder sa cause : on recale sur l'ordre.
          if (timeDelta(_candidateTs, _note.noteCommandTimestamp) < 0) {
            _candidateTs = _note.noteCommandTimestamp;
          }
        }
        _confirmCount++;
        if (_confirmCount >= kOnsetConfirmFrames) {
          // L'instant retenu est le PREMIER franchissement, pas la
          // confirmation : exiger deux frames ne doit rien couter en precision.
          _note.hasSoundOnset = true;
          _note.soundOnsetTimestamp = _candidateTs;
          setMeasure(_note.commandToSoundLatency, _note.hasNoteCommand,
                     _note.noteCommandTimestamp, true, _candidateTs);
          // Si aucune consigne d'air n'a ete declaree, cette latence n'a pas ete
          // mesuree : elle reste INVALIDE. Elle ne vaut surtout pas zero.
          setMeasure(_note.airToSoundLatency, _note.hasAirCommand,
                     _note.airCommandTimestamp, true, _candidateTs);
          _maxSinceOnset = f.rmsDbFS;
          _confirmCount = 0;
          _state = TIMING_ATTACK;
          // La frame qui confirme appartient deja a l'attaque.
          _plateauDb[_plateauIdx] = f.rmsDbFS;
          _plateauTs[_plateauIdx] = ts;
          _plateauIdx = (uint8_t)((_plateauIdx + 1) % kAttackStableFrames);
          if (_plateauCount < kAttackStableFrames) _plateauCount++;
          trackPitch(f);
        }
      } else {
        _confirmCount = 0;
      }
      break;
    }

    case TIMING_ATTACK: {
      if (f.rmsDbFS > _maxSinceOnset) _maxSinceOnset = f.rmsDbFS;

      _plateauDb[_plateauIdx] = f.rmsDbFS;
      _plateauTs[_plateauIdx] = ts;
      _plateauIdx = (uint8_t)((_plateauIdx + 1) % kAttackStableFrames);
      if (_plateauCount < kAttackStableFrames) _plateauCount++;

      uint32_t atTs = 0;
      if (plateauReached(atTs)) {
        _note.hasLevelEstablished = true;
        _note.levelEstablishedTimestamp = atTs;
        setMeasure(_note.attackTime, true, _note.soundOnsetTimestamp, true, atTs);
        _sustainMeanDb = f.rmsDbFS;
        _sustainFrames = 1;
        _state = TIMING_SUSTAIN;
      } else if (timeReached(ts, _note.soundOnsetTimestamp + kAttackTimeoutMs)) {
        // Le niveau n'a jamais fait de palier. attackTime reste INVALIDE, mais
        // la note continue d'etre suivie : son relachement, lui, est mesurable.
        _sustainMeanDb = f.rmsDbFS;
        _sustainFrames = 1;
        _state = TIMING_SUSTAIN;
      }
      trackPitch(f);
      break;
    }

    case TIMING_SUSTAIN: {
      if (f.rmsDbFS > _maxSinceOnset) _maxSinceOnset = f.rmsDbFS;
      // Moyenne incrementale : meme forme que NoiseModel, exacte a chaque pas
      // et sans somme qui grandirait sans fin sur une note tenue. Le compteur
      // sature : il DIVISE la moyenne, et repasser a zero la ferait exploser.
      // kNoteMaxDurationMs le borne deja bien avant, mais la moyenne ne doit
      // pas dependre d'un plafond regle ailleurs.
      if (_sustainFrames < 0xFFFFu) _sustainFrames++;
      _sustainMeanDb += (f.rmsDbFS - _sustainMeanDb) / (float)_sustainFrames;
      trackPitch(f);
      break;
    }

    case TIMING_RELEASING: {
      if (f.rmsDbFS <= _releaseThresholdDb) {
        if (_confirmCount == 0) {
          _candidateTs = crossingInstant(ts, f.rmsDbFS, _releaseThresholdDb);
        }
        _confirmCount++;
        if (_confirmCount >= kReleaseConfirmFrames) {
          _note.hasSoundRelease = true;
          _note.soundReleaseTimestamp = _candidateTs;
          // Un franchissement anterieur a l'ordre signifie que le son s'etait
          // deja tu avant qu'on le coupe. Ce n'est pas une latence de
          // relachement : la mesure reste invalide plutot que d'etre ramenee a
          // zero.
          setMeasure(_note.releaseTime, true, _note.releaseCommandTimestamp,
                     true, _candidateTs);
          closeNote(_cutShort ? TIMING_CUT_SHORT : TIMING_COMPLETE);
        }
      } else if (f.rmsDbFS >= _releaseThresholdDb + kReleaseHysteresisDb) {
        // HYSTERESIS : seule une remontee FRANCHE annule le comptage. Une frame
        // qui repasse de quelques dixiemes de dB au-dessus du seuil tombe dans
        // la bande morte ci-dessous, ou elle ne confirme ni n'annule - sans
        // quoi une simple ondulation relancerait le comptage sans fin et le
        // relachement ne serait jamais declare.
        _confirmCount = 0;
      }
      break;
    }
  }

  _havePrev = true;
  _prevTs = ts;
  _prevDb = f.rmsDbFS;
  if (_state == TIMING_IDLE) pushBaseline(f.rmsDbFS);
  return true;
}

/*----------------------------------------------------------------------------
 * Adaptateur
 *
 * FLUX SPECTRAL - ou il se brancherait, et pourquoi il n'est PAS la
 * -----------------------------------------------------------------
 * Le critere d'attaque est evalue en un seul point, dans update(), etat
 * TIMING_WAIT_ONSET : c'est la, et nulle part ailleurs, qu'un flux spectral
 * viendrait s'ajouter au critere de montee de RMS.
 *
 * Il n'est pas implemente, et la raison est MESURABLE, pas une preference : le
 * chemin spectral ne tourne qu'une frame sur MIC_SPECTRAL_DECIMATION (4), soit
 * une mesure toutes les 64 ms, la ou l'enveloppe en donne une toutes les 16 ms.
 * Un onset detecte par flux spectral serait donc quantifie QUATRE FOIS plus
 * grossierement que celui qu'il est cense ameliorer. Le cahier des charges dit
 * "si cela apporte un gain reel" : ici il apporterait une perte chiffrable.
 *
 * Ce qui devrait changer AVANT d'y revenir : MIC_SPECTRAL_DECIMATION ramene a
 * 1, ce qui multiplie par quatre le cout spectral par seconde, et une mesure
 * sur vraie flute montrant des attaques que la montee de RMS manque - par
 * exemple une attaque tres douce ou le timbre bouge avant le niveau.
 *--------------------------------------------------------------------------*/

TimingFrame AcousticTiming::fromFeatures(const AcousticFeatures& f) {
  TimingFrame out;
  out.timestampMs = f.timestamp;
  out.rmsDbFS = f.rmsDbFS;
  // Le verdict du DETECTEUR, propage tel quel. fillPitch() renseigne desormais
  // `pitchValid` depuis PitchResult::valid, ce qui retire d'ici le critere qui
  // y avait ete reconstruit faute de mieux.
  //
  // LES DEUX CRITERES NE SONT PAS EQUIVALENTS, et c'est pour cela qu'on garde
  // celui-ci. La reconstruction disait `hz > 0 && confiance >= seuil`. Elle
  // reposait sur un INVARIANT de runYin() - `hz` n'est renseigne que dans la
  // plage [MIC_PITCH_MIN_HZ, MIC_PITCH_MAX_HZ] - au lieu de l'exprimer :
  // presentee une frequence repliee hors plage accompagnee d'une bonne
  // confiance, elle la declarait valide, la ou le detecteur l'a rejetee. Sur la
  // chaine reelle les deux coincident exactement (verifie frame a frame sur le
  // flux complet d'une note) ; partout ailleurs - descripteurs assembles a la
  // main, futur detecteur, mesure importee - c'est le drapeau qui a raison,
  // pour la meme raison qu'AcousticQuality::pitchIsUsable teste la plage
  // explicitement.
  //
  // Ce que le critere protege reste le meme : une frequence dans la plage
  // accompagnee d'une confiance insuffisante est exactement ce que YIN produit
  // sur un transitoire d'attaque. La prendre pour un pitch daterait
  // pitchDetectedTimestamp sur du souffle.
  out.pitchValid = f.pitchValid;
  out.pitchHz = f.pitchHz;
  return out;
}
