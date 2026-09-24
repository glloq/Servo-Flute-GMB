// LOT D - Le calibrateur automatique doit muter `cfg` par la MEME discipline que
// le reste du firmware : CANDIDAT -> VALIDATION -> PERSISTANCE -> ACTIVATION.
//
// CE QUI A ETE REPRODUIT, en executant le code de production, pas en le lisant :
//
//   1. applyResults() ne validait RIEN. Un resultat de calibration aberrant
//      (airMin=90 > airMax=10, nominal=95) partait directement dans `cfg`,
//      champ par champ, puis en flash :
//          avant  : notes[0] min=0  max=100 nominal=50
//          apres  : notes[0] min=90 max=10  nominal=95   applied=true saved=true
//      Cette configuration est exactement celle que ConfigValidator refuse
//      ("note airflow min > max at index 0") et que le prochain demarrage
//      rejetterait. Entre l'ecriture et la fin de la sauvegarde LittleFS, le
//      sequenceur lit ces memes champs a chaque note : l'angle de souffle
//      commande est alors calcule sur une fenetre inversee.
//
//   2. Ce qui partait en flash etait la configuration ACTIVE deja modifiee
//      (ConfigStorage::save(), c'est-a-dire saveFrom(cfg)), pas un candidat.
//      L'ordre etait donc "muter, puis essayer de persister, puis defaire" et
//      non "construire, valider, persister, activer". La reparation par
//      restauration champ a champ existait - elle ne couvrait que les trois
//      champs de note, et seulement sur le chemin "echec de sauvegarde".
//
//   3. applyResults() n'allouait pas de candidat, donc aucun chemin d'abandon
//      propre n'existait quand la memoire manque : il ecrivait `cfg` quoi qu'il
//      arrive. applyRangeResults(), lui, allouait deja et verifiait le nul.
//
//   4. applyRangeResults() validait bien un candidat - puis le JETAIT et
//      persistait `cfg`. La configuration validee/normalisee et la configuration
//      persistee n'etaient donc pas le meme objet.
//
// Ce que ce fichier verrouille : a chaque etape qui echoue, `cfg` est IDENTIQUE
// BIT A BIT a ce qu'il etait avant ; en cas de succes, ce qui est actif en RAM
// est exactement ce qui est parti en flash.
#include <cassert>
#include <cstring>
#include <cstddef>
#include <iostream>
#include <new>
#include "Arduino.h"
#include "ConfigStorage.h"
#include "FingerController.h"
#include "AirflowController.h"
#include "IAudioSource.h"
#include "ICalibrationAirSupply.h"
#include "PitchMath.h"
// Les resultats par note, le compte de notes et les angles du range finder sont
// des details prives ; les forcer directement evite de dependre d'un chemin web
// pour fabriquer un resultat aberrant (meme procede que test_hw_boot.cpp).
#define private public
#include "AutoCalibrator.h"
#undef private

// Bouchon de persistance partage (lib/native_stubs/src/config_test_stub.cpp).
// __config_last_saved enregistre le CANDIDAT reellement offert a la flash : il
// permet d'asserter ce qui est parti, pas seulement qu'un appel a eu lieu.
extern bool __config_save_result;
extern int __config_save_calls;
extern RuntimeConfig __config_last_saved;

namespace {

// --- Echec d'allocation a la demande -----------------------------------------
// Le candidat est alloue par `new (std::nothrow) RuntimeConfig()` (convention du
// depot, voir ConfigStorage.cpp) : RuntimeConfig fait ~5 Ko et deux exemplaires
// sur la pile d'une tache FreeRTOS la font deborder. Une allocation NON VERIFIEE
// etant elle-meme un defaut, il faut pouvoir prouver le chemin nul.
//
// On remplace la fonction d'allocation globale nothrow - elle est remplacable
// par la norme - plutot que d'ouvrir une trappe `#ifdef UNIT_TEST` dans le code
// de production. Elle n'est armee que le temps d'une assertion, et `new
// (std::nothrow)` n'apparait nulle part ailleurs dans la compilation hote.
bool g_nothrowNewFails = false;

}  // namespace

void* operator new(std::size_t sz, const std::nothrow_t&) noexcept {
  if (g_nothrowNewFails) return nullptr;
  try {
    return ::operator new(sz);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }

namespace {

// --- Sources simulees (locales a cette unite : aucune collision d'ODR) --------

struct CalAudio : public IAudioSource {
  bool micDetected = true, active = false;
  float rms = 0, hz = 0, cents = 0, conf = 0;
  int midi = 0;
  bool valid = false, snd = false;
  uint32_t seq = 0;
  unsigned long ts = 0;
  bool isMicDetected() const override { return micDetected; }
  void setActive(bool a) override { active = a; }
  bool isActive() const override { return active; }
  float getRMS() const override { return rms; }
  bool isSoundDetected() const override { return snd; }
  float getPitchHz() const override { return hz; }
  int getPitchMidi() const override { return midi; }
  float getPitchCents() const override { return cents; }
  float getPitchConfidence() const override { return conf; }
  bool isPitchValid() const override { return valid; }
  uint32_t getFrameSequence() const override { return seq; }
  unsigned long getFrameTimestamp() const override { return ts; }
};

struct CalAir : public ICalibrationAirSupply {
  bool prepared = false, stopped = false;
  bool neverReady = false;
  void prepare() override { prepared = true; stopped = false; }
  bool isReady() override { return prepared && !neverReady; }
  void setDemandPercent(uint8_t) override {}
  void stopSafe() override { stopped = true; }
  CalAirSupplyError getError() const override {
    return neverReady ? CAL_AIR_TIMEOUT : CAL_AIR_OK;
  }
};

// Configuration minimale VALIDE (confirmee par base_config_is_valid ci-dessous).
void baseCfg() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.numFingers = 1;
  cfg.fingers[0].pcaChannel = 0; cfg.fingers[0].closedAngle = 90; cfg.fingers[0].direction = 1;
  cfg.numPumps = 1; cfg.pumpPins[0] = 25; cfg.pumpMinPwm[0] = 80; cfg.pumpMaxPwm[0] = 200;
  cfg.motorType = MOTOR_TYPE_PWM; cfg.airMode = AIR_MODE_PUMP_VALVE;
  cfg.sensorType = SENSOR_TYPE_HALL_KY024; cfg.hallPin = 36;
  cfg.hallThresholdLow = 1000; cfg.hallThresholdHigh = 2000;
  cfg.pidKp = 10; cfg.pidKi = 0;
  cfg.servoToSolenoidDelayMs = 10; cfg.minNoteDurationMs = 100;
  cfg.airflowPcaChannel = 10; cfg.solenoidPin = 13;
  cfg.solenoidActivationTimeMs = 50; cfg.solenoidPwmActivation = 255; cfg.solenoidPwmHolding = 128;
  cfg.servoAirflowOff = 20; cfg.servoAirflowMin = 60; cfg.servoAirflowMax = 100;
  cfg.servoAngleOff = 90; cfg.servoAngleMin = 45; cfg.servoAngleMax = 135;
  cfg.ccVolumeDefault = 127; cfg.ccExpressionDefault = 127;
  cfg.ccBreathDefault = 127; cfg.ccBrightnessDefault = 64; cfg.airVelocityResponse = 100;
  strcpy(cfg.embouchure, "bec");
  cfg.numNotes = 3;
  cfg.notes[0].midiNote = 60; cfg.notes[1].midiNote = 62; cfg.notes[2].midiNote = 64;
  for (int i = 0; i < 3; i++) {
    cfg.notes[i].airflowMinPercent = 0;
    cfg.notes[i].airflowMaxPercent = 100;
    cfg.notes[i].airflowNominalPercent = 50;
  }
}

void feedNote(CalAudio& f, int midi) {
  f.rms = 0.06f; f.valid = true; f.conf = 0.95f; f.midi = midi;
  f.hz = PitchMath::midiToHz(midi); f.cents = -3.0f; f.snd = true;
}
void feedSilence(CalAudio& f) {
  f.rms = 0.003f; f.valid = false; f.conf = 0.0f; f.midi = 0; f.hz = 0; f.cents = 0; f.snd = false;
}

// La note se fait entendre dans [lo,hi] %, l'octave au-dessus au-dela.
void bandModel(CalAudio& f, int pct, int expectedMidi, int lo, int hi) {
  if (pct >= lo && pct <= hi) feedNote(f, expectedMidi);
  else if (pct > hi) feedNote(f, expectedMidi + 12);
  else feedSilence(f);
}

// Les copies de reference et les comparaisons passent par memcmp : baseCfg()
// memset la structure entiere avant de la remplir, et les copies sont des
// memcpy, donc le remplissage (padding) est deterministe des deux cotes.
RuntimeConfig g_before;
void snapshotCfg() { memcpy(&g_before, &cfg, sizeof(cfg)); }
bool cfgUnchanged() { return memcmp(&g_before, &cfg, sizeof(cfg)) == 0; }

// Fait tourner une calibration airflow jusqu'a son terme.
// `watchCfg` : verifie a CHAQUE tour que la configuration active n'a pas bouge.
void driveAirflow(AutoCalibrator& cal, CalAudio& fa, int lo, int hi,
                  bool watchCfg = false, int* checkPoints = nullptr) {
  int it = 0;
  while (cal.isRunning() && it++ < 200000) {
    int note = cal.getCurrentNoteIndex();
    int expected = (note >= 0 && note < (int)cfg.numNotes) ? cfg.notes[note].midiNote : 60;
    bandModel(fa, cal.getCurrentAirPercent(), expected, lo, hi);
    fa.seq++; fa.ts = __test_millis;
    __test_millis += 25;
    cal.update();
    if (watchCfg) {
      // Point intermediaire : pendant la calibration, RIEN ne doit etre ecrit.
      assert(cfgUnchanged());
      if (checkPoints) (*checkPoints)++;
    }
  }
  assert(!cal.isRunning());
}

// Fait tourner un range finder jusqu'a son terme (la note s'entend entre
// `soundLo` et `soundHi` degres).
void driveRange(AutoCalibrator& cal, CalAudio& fa, int soundLo, int soundHi,
                bool watchCfg = false, int* checkPoints = nullptr) {
  int expMidi = cfg.notes[cfg.numNotes / 2].midiNote;
  int it = 0;
  while (cal.isRunning() && it++ < 200000) {
    int ang = cal.getCurrentAngle();
    if (ang >= soundLo && ang <= soundHi) feedNote(fa, expMidi);
    else feedSilence(fa);
    fa.seq++; fa.ts = __test_millis;
    __test_millis += 25;
    cal.update();
    if (watchCfg) {
      assert(cfgUnchanged());
      if (checkPoints) (*checkPoints)++;
    }
  }
  assert(!cal.isRunning());
}

// =============================================================================
// Garde-fou : si la configuration de depart etait refusee par la validation, les
// tests qui suivent prouveraient un refus pour la mauvaise raison.
// =============================================================================
void base_config_is_valid() {
  baseCfg();
  RuntimeConfig work;
  assert(validateCandidateConfig(cfg, work).valid);
}

// =============================================================================
// 1. Un echec, a n'importe quelle etape, laisse `cfg` IDENTIQUE BIT A BIT
// =============================================================================

// Aucun resultat valide (note muette), source audio figee, source d'air qui
// n'arrive jamais : trois chemins d'echec distincts, une seule exigence.
void a_failed_run_leaves_the_active_config_bit_identical() {
  // (a) la note ne sonne jamais
  baseCfg();
  cfg.numNotes = 1;
  __test_millis = 0;
  snapshotCfg();
  {
    CalAudio fa;
    FingerController fc([](uint8_t, uint16_t, uint16_t) {});
    AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
    CalAir as;
    AutoCalibrator cal(fc, ac, fa, as);
    cal.start(ACAL_MODE_AIRFLOW);
    driveAirflow(cal, fa, 900, 901);          // hors de toute plage : silence total
    assert(cal.isComplete());
    assert(!cal.getResult(0).valid);
    int savesBefore = __config_save_calls;
    AutoCalApplyResult ap = cal.applyResults();
    assert(!ap.applied && !ap.saved && ap.validCount == 0);
    assert(__config_save_calls == savesBefore);   // rien n'est meme offert a la flash
    assert(cfgUnchanged());
  }

  // (b) source audio figee (la sequence de trames n'avance plus)
  baseCfg();
  cfg.numNotes = 1;
  __test_millis = 0;
  snapshotCfg();
  {
    CalAudio fa; fa.seq = 7;
    FingerController fc([](uint8_t, uint16_t, uint16_t) {});
    AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
    CalAir as;
    AutoCalibrator cal(fc, ac, fa, as);
    cal.start(ACAL_MODE_AIRFLOW);
    int it = 0;
    while (cal.isRunning() && it++ < 200000) {
      feedNote(fa, 60);                        // "sonne" juste, mais jamais frais
      __test_millis += 25;
      cal.update();
      assert(cfgUnchanged());
    }
    assert(cal.isComplete());
    assert(cal.getResult(0).failureReason == ACAL_FAIL_AUDIO_STALE);
    int savesBefore = __config_save_calls;
    AutoCalApplyResult ap = cal.applyResults();
    assert(!ap.applied && !ap.saved);
    assert(__config_save_calls == savesBefore);
    assert(cfgUnchanged());
  }

  // (c) la source d'air n'est jamais prete : toutes les notes echouent
  baseCfg();
  __test_millis = 0;
  snapshotCfg();
  {
    CalAudio fa;
    FingerController fc([](uint8_t, uint16_t, uint16_t) {});
    AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
    CalAir as; as.neverReady = true;
    AutoCalibrator cal(fc, ac, fa, as);
    cal.start(ACAL_MODE_AIRFLOW);
    driveAirflow(cal, fa, 20, 60);
    assert(cal.isComplete());
    assert(cal.getResult(0).failureReason == ACAL_FAIL_AIR_SUPPLY);
    int savesBefore = __config_save_calls;
    AutoCalApplyResult ap = cal.applyResults();
    assert(!ap.applied && !ap.saved);
    assert(__config_save_calls == savesBefore);
    assert(cfgUnchanged());
  }
}

// LE test du defaut principal. Un resultat marque valide mais aberrant
// (min > max, nominal hors [min,max]) doit etre refuse par ConfigValidator AVANT
// d'atteindre la flash, et ne jamais transiter par la configuration active.
//
// AVANT : les trois champs partaient dans `cfg` tels quels, ConfigStorage::save()
// etait appele, et applyResults() rendait applied=true saved=true.
void an_aberrant_result_is_refused_before_it_reaches_the_flash() {
  baseCfg();
  snapshotCfg();

  CalAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  CalAir as;
  AutoCalibrator cal(fc, ac, fa, as);

  // Resultat "reussi" mais incoherent : exactement ce que ConfigValidator refuse
  // ("note airflow min > max", "note airflow nominal > max").
  cal._numNotes = cfg.numNotes;
  cal._results[0].valid = true;
  cal._results[0].airMin = 90;
  cal._results[0].airMax = 10;
  cal._results[0].airNominal = 95;
  cal._results[0].confidence = 100;

  int savesBefore = __config_save_calls;
  AutoCalApplyResult ap = cal.applyResults();

  assert(!ap.applied);
  assert(!ap.saved);
  assert(ap.validCount == 1);                   // le comptage reste honnete
  assert(__config_save_calls == savesBefore);   // rien n'est meme tente
  assert(cfgUnchanged());                       // et surtout : rien n'a transite
}

// Une valeur que la validation NORMALISE n'est pas ce qui a ete mesure : un
// airMin a 200 deviendrait 100 en silence. Le candidat doit etre refuse, pas
// corrige a l'insu de l'operateur.
void a_result_the_validator_would_move_is_refused() {
  baseCfg();
  snapshotCfg();

  CalAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  CalAir as;
  AutoCalibrator cal(fc, ac, fa, as);

  cal._numNotes = cfg.numNotes;
  cal._results[1].valid = true;
  cal._results[1].airMin = 10;
  cal._results[1].airMax = 200;   // hors 0..100 : normalise a 100
  cal._results[1].airNominal = 50;

  int savesBefore = __config_save_calls;
  AutoCalApplyResult ap = cal.applyResults();
  assert(!ap.applied && !ap.saved);
  assert(__config_save_calls == savesBefore);
  assert(cfgUnchanged());
}

// =============================================================================
// 2. Echec de persistance : `cfg` inchange ET rien d'active
// =============================================================================
void a_storage_failure_leaves_the_active_config_bit_identical() {
  baseCfg();
  cfg.numNotes = 1;
  cfg.notes[0].airflowMinPercent = 1;
  cfg.notes[0].airflowMaxPercent = 99;
  cfg.notes[0].airflowNominalPercent = 50;
  __test_millis = 0;

  CalAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  CalAir as;
  AutoCalibrator cal(fc, ac, fa, as);
  cal.start(ACAL_MODE_AIRFLOW);
  driveAirflow(cal, fa, 20, 60);
  assert(cal.isComplete());
  AutoCalNoteResult r = cal.getResult(0);
  assert(r.valid);

  snapshotCfg();
  memset(&__config_last_saved, 0, sizeof(__config_last_saved));
  __config_save_result = false;                // panne LittleFS
  int savesBefore = __config_save_calls;
  AutoCalApplyResult ap = cal.applyResults();
  __config_save_result = true;

  assert(!ap.applied && !ap.saved && ap.validCount == 1);
  assert(cfgUnchanged());                      // bit a bit, pas seulement 3 champs
  // La tentative a bien eu lieu, et ce qui a ete offert a la flash est un
  // CANDIDAT porteur du nouveau resultat - pas la configuration active.
  assert(__config_save_calls == savesBefore + 1);
  assert(__config_last_saved.notes[0].airflowNominalPercent == r.airNominal);
  assert(__config_last_saved.notes[0].airflowMinPercent == r.airMin);
  assert(__config_last_saved.notes[0].airflowMaxPercent == r.airMax);
  // Le candidat differe donc de la configuration active au moment de l'ecriture.
  assert(memcmp(&__config_last_saved, &cfg, sizeof(cfg)) != 0);
}

// =============================================================================
// 3. Succes complet : la RAM et ce qui est parti en flash ne divergent pas
// =============================================================================
void a_successful_apply_activates_exactly_what_was_persisted() {
  baseCfg();
  cfg.numNotes = 1;
  __test_millis = 0;

  CalAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  CalAir as;
  AutoCalibrator cal(fc, ac, fa, as);
  cal.start(ACAL_MODE_AIRFLOW);
  driveAirflow(cal, fa, 20, 60);
  assert(cal.isComplete());
  AutoCalNoteResult r = cal.getResult(0);
  assert(r.valid);

  memset(&__config_last_saved, 0, sizeof(__config_last_saved));
  __config_save_result = true;
  AutoCalApplyResult ap = cal.applyResults();
  assert(ap.applied && ap.saved && ap.validCount == 1);

  // Le resultat est bien en vigueur...
  assert(cfg.notes[0].airflowMinPercent == r.airMin);
  assert(cfg.notes[0].airflowMaxPercent == r.airMax);
  assert(cfg.notes[0].airflowNominalPercent == r.airNominal);
  // ... et la configuration active est EXACTEMENT celle qui est partie en flash.
  assert(memcmp(&__config_last_saved, &cfg, sizeof(cfg)) == 0);
}

// Une note qui echoue ne doit pas ecraser sa calibration precedente, et le
// candidat doit refleter ce choix (protection existante, verrouillee ici cote
// candidat et plus seulement cote `cfg`).
void a_failed_note_keeps_its_previous_calibration_in_the_candidate() {
  baseCfg();
  cfg.numNotes = 2;
  cfg.notes[0].airflowMinPercent = 0;  cfg.notes[0].airflowMaxPercent = 100; cfg.notes[0].airflowNominalPercent = 50;
  cfg.notes[1].airflowMinPercent = 11; cfg.notes[1].airflowMaxPercent = 77;  cfg.notes[1].airflowNominalPercent = 33;
  __test_millis = 0;

  CalAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  CalAir as;
  AutoCalibrator cal(fc, ac, fa, as);
  cal.start(ACAL_MODE_AIRFLOW);
  // Seule la note 0 sonne ; la note 1 reste muette et echoue.
  int it = 0;
  while (cal.isRunning() && it++ < 200000) {
    int note = cal.getCurrentNoteIndex();
    if (note == 0) bandModel(fa, cal.getCurrentAirPercent(), 60, 20, 60);
    else feedSilence(fa);
    fa.seq++; fa.ts = __test_millis;
    __test_millis += 25;
    cal.update();
  }
  assert(cal.isComplete());
  assert(cal.getResult(0).valid && !cal.getResult(1).valid);

  memset(&__config_last_saved, 0, sizeof(__config_last_saved));
  AutoCalApplyResult ap = cal.applyResults();
  assert(ap.applied && ap.saved && ap.validCount == 1 && ap.failedCount == 1);
  // La note ratee garde ses valeurs, dans la RAM ET dans ce qui est parti en flash.
  assert(cfg.notes[1].airflowMinPercent == 11 && cfg.notes[1].airflowMaxPercent == 77 &&
         cfg.notes[1].airflowNominalPercent == 33);
  assert(__config_last_saved.notes[1].airflowMinPercent == 11 &&
         __config_last_saved.notes[1].airflowMaxPercent == 77 &&
         __config_last_saved.notes[1].airflowNominalPercent == 33);
  assert(memcmp(&__config_last_saved, &cfg, sizeof(cfg)) == 0);
}

// =============================================================================
// 4. Pendant toute la calibration, `cfg` n'est jamais modifie, meme transitoirement
// =============================================================================
void the_active_config_is_untouched_for_the_whole_run() {
  baseCfg();
  cfg.numNotes = 2;
  __test_millis = 0;
  snapshotCfg();

  CalAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  CalAir as;
  AutoCalibrator cal(fc, ac, fa, as);
  cal.start(ACAL_MODE_AIRFLOW);
  assert(cfgUnchanged());                      // start() non plus n'ecrit rien

  int checks = 0;
  driveAirflow(cal, fa, 20, 60, /*watchCfg=*/true, &checks);
  assert(checks > 50);                         // beaucoup de points intermediaires
  assert(cal.isComplete());
  assert(cfgUnchanged());                      // encore vrai juste avant l'activation

  AutoCalApplyResult ap = cal.applyResults();
  assert(ap.applied);
  assert(!cfgUnchanged());                     // l'activation, elle, a bien eu lieu
}

// =============================================================================
// 5. Meme couverture pour le chercheur d'etendue (chemin distinct)
// =============================================================================

void a_range_run_never_touches_the_active_config() {
  baseCfg();
  __test_millis = 0;
  snapshotCfg();

  CalAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  CalAir as;
  AutoCalibrator cal(fc, ac, fa, as);
  cal.start(ACAL_MODE_RANGE_FIND);
  assert(cfgUnchanged());

  int checks = 0;
  driveRange(cal, fa, 60, 100, /*watchCfg=*/true, &checks);
  assert(checks > 20);
  assert(cal.isRangeFinderComplete());
  assert(cfgUnchanged());
}

void a_range_storage_failure_leaves_the_active_config_bit_identical() {
  baseCfg();
  __test_millis = 0;

  CalAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  CalAir as;
  AutoCalibrator cal(fc, ac, fa, as);
  cal.start(ACAL_MODE_RANGE_FIND);
  driveRange(cal, fa, 60, 100);
  assert(cal.isRangeFinderComplete());
  int mn = cal.getRangeFinderMin(), mx = cal.getRangeFinderMax();
  assert(mn >= 0 && mx > mn);

  snapshotCfg();
  memset(&__config_last_saved, 0, sizeof(__config_last_saved));
  __config_save_result = false;
  int savesBefore = __config_save_calls;
  RangeApplyResult ra = cal.applyRangeResults();
  __config_save_result = true;

  assert(!ra.applied && !ra.saved);
  assert(cfgUnchanged());                      // bit a bit, pas seulement 2 champs
  assert(__config_save_calls == savesBefore + 1);
  // Le candidat offert a la flash portait bien les angles decouverts.
  assert((int)__config_last_saved.servoAirflowMin == mn);
  assert((int)__config_last_saved.servoAirflowMax == mx);
  assert(memcmp(&__config_last_saved, &cfg, sizeof(cfg)) != 0);
}

void a_range_apply_activates_exactly_what_was_persisted() {
  baseCfg();
  __test_millis = 0;

  CalAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  CalAir as;
  AutoCalibrator cal(fc, ac, fa, as);
  cal.start(ACAL_MODE_RANGE_FIND);
  driveRange(cal, fa, 60, 100);
  assert(cal.isRangeFinderComplete());
  int mn = cal.getRangeFinderMin(), mx = cal.getRangeFinderMax();
  assert(mn >= 0 && mx > mn);

  memset(&__config_last_saved, 0, sizeof(__config_last_saved));
  __config_save_result = true;
  RangeApplyResult ra = cal.applyRangeResults();
  assert(ra.applied && ra.saved);
  assert(ra.minAngle == mn && ra.maxAngle == mx);
  assert((int)cfg.servoAirflowMin == mn && (int)cfg.servoAirflowMax == mx);
  assert(memcmp(&__config_last_saved, &cfg, sizeof(cfg)) == 0);
}

// Angles que la validation refuse : rien n'est tente, rien ne transite.
// (La protection existe deja ; ce qui est ajoute ici est la comparaison BIT A BIT
// de toute la structure, la ou l'ancienne assertion ne regardait que deux champs.)
void a_range_result_the_validator_refuses_changes_nothing() {
  baseCfg();
  snapshotCfg();

  CalAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  CalAir as;
  AutoCalibrator cal(fc, ac, fa, as);

  cal._rfMinAngle = 200; cal._rfMaxAngle = 210;      // hors course servo
  int savesBefore = __config_save_calls;
  RangeApplyResult r1 = cal.applyRangeResults();
  assert(!r1.applied && !r1.saved);
  assert(__config_save_calls == savesBefore);
  assert(cfgUnchanged());
  assert(cal.getRangeFailureReason() == ACAL_FAIL_RANGE_INVALID);

  cal._rfMinAngle = 90; cal._rfMaxAngle = 90;        // plage vide
  savesBefore = __config_save_calls;
  RangeApplyResult r2 = cal.applyRangeResults();
  assert(!r2.applied && !r2.saved);
  assert(__config_save_calls == savesBefore);
  assert(cfgUnchanged());

  cal._rfMinAngle = -1; cal._rfMaxAngle = -1;        // aucun resultat
  savesBefore = __config_save_calls;
  RangeApplyResult r3 = cal.applyRangeResults();
  assert(!r3.applied && !r3.saved);
  assert(__config_save_calls == savesBefore);
  assert(cfgUnchanged());
}

// =============================================================================
// 6. Allocation du candidat impossible : abandon propre, aucun dereferencement nul
// =============================================================================
void a_failed_candidate_allocation_aborts_without_touching_the_config() {
  baseCfg();
  cfg.numNotes = 1;
  __test_millis = 0;

  CalAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  CalAir as;
  AutoCalibrator cal(fc, ac, fa, as);
  cal.start(ACAL_MODE_AIRFLOW);
  driveAirflow(cal, fa, 20, 60);
  assert(cal.isComplete());
  assert(cal.getResult(0).valid);

  snapshotCfg();
  int savesBefore = __config_save_calls;
  g_nothrowNewFails = true;
  AutoCalApplyResult ap = cal.applyResults();
  g_nothrowNewFails = false;
  assert(!ap.applied && !ap.saved);
  assert(ap.validCount == 1);                  // le comptage reste honnete
  assert(__config_save_calls == savesBefore);  // rien n'est offert a la flash
  assert(cfgUnchanged());

  // Meme exigence pour le chercheur d'etendue.
  cal._rfMinAngle = 55; cal._rfMaxAngle = 118;
  savesBefore = __config_save_calls;
  g_nothrowNewFails = true;
  RangeApplyResult ra = cal.applyRangeResults();
  g_nothrowNewFails = false;
  assert(!ra.applied && !ra.saved);
  assert(__config_save_calls == savesBefore);
  assert(cfgUnchanged());

  // Et la memoire revenue, le meme resultat s'applique normalement : le garde-fou
  // ne coute pas la fonctionnalite.
  RangeApplyResult ok = cal.applyRangeResults();
  assert(ok.applied && ok.saved);
  assert(cfg.servoAirflowMin == 55 && cfg.servoAirflowMax == 118);
}

}  // namespace

void harden_autocal_run_all_tests() {
  base_config_is_valid();
  a_failed_run_leaves_the_active_config_bit_identical();
  an_aberrant_result_is_refused_before_it_reaches_the_flash();
  a_result_the_validator_would_move_is_refused();
  a_storage_failure_leaves_the_active_config_bit_identical();
  a_successful_apply_activates_exactly_what_was_persisted();
  a_failed_note_keeps_its_previous_calibration_in_the_candidate();
  the_active_config_is_untouched_for_the_whole_run();
  a_range_run_never_touches_the_active_config();
  a_range_storage_failure_leaves_the_active_config_bit_identical();
  a_range_apply_activates_exactly_what_was_persisted();
  a_range_result_the_validator_refuses_changes_nothing();
  a_failed_candidate_allocation_aborts_without_touching_the_config();
  std::cout << "harden autocal tests passed\n";
}
