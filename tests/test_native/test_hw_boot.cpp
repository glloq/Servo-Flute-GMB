/***********************************************************************************************
 * test_hw_boot - Surete materielle : range finder, persistance de configuration,
 *                autorisation de piloter au demarrage, conversion angle -> PWM.
 *
 * Chaque test verrouille un defaut qui a ete REPRODUIT en executant le code de
 * production, et son commentaire dit quel materiel il protege.
 *
 * Point d'entree : hw_boot_run_all_tests().
 ***********************************************************************************************/
#include <cassert>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "Arduino.h"
#include "ConfigStorage.h"
#include "ServoMath.h"
#include "FingerController.h"
#include "AirflowController.h"
#include "IAudioSource.h"
#include "ICalibrationAirSupply.h"
#include "PitchMath.h"
// Le resultat du range finder et sa fenetre de balayage sont des details prives ;
// les tester directement evite de dependre d'un chemin web pour les atteindre.
#define private public
#include "AutoCalibrator.h"
#undef private

// Stub de persistance partage avec les autres unites de test natives.
extern bool __config_save_result;
extern int __config_save_calls;

namespace {

// --- Sources simulees (locales a cette unite : aucune collision d'ODR) --------

struct HwFakeAudio : public IAudioSource {
  bool micDetected = true, active = false;
  float rms = 0, hz = 0, cents = 0, conf = 0; int midi = 0; bool valid = false, snd = false;
  uint32_t seq = 0; unsigned long ts = 0;
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

struct HwFakeAirSupply : public ICalibrationAirSupply {
  bool prepared = false;
  void prepare() override { prepared = true; }
  bool isReady() override { return prepared; }
  void setDemandPercent(uint8_t) override {}
  void stopSafe() override {}
  CalAirSupplyError getError() const override { return CAL_AIR_OK; }
};

// Configuration minimale VALIDE (la validation la confirme dans hw_boot_config_base_is_valid).
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

int middleNoteMidi() { return cfg.notes[cfg.numNotes / 2].midiNote; }

void feedNote(HwFakeAudio& f, int midi) {
  f.rms = 0.06f; f.valid = true; f.conf = 0.95f; f.midi = midi;
  f.hz = PitchMath::midiToHz(midi); f.cents = -3.0f; f.snd = true;
}
void feedSilence(HwFakeAudio& f) {
  f.rms = 0.003f; f.valid = false; f.conf = 0.0f; f.midi = 0; f.hz = 0; f.cents = 0; f.snd = false;
}

// Fait tourner un range finder jusqu'a son terme en enregistrant tous les angles
// COMMANDES par le calibrateur (et non ceux finalement ecrits par le controleur :
// c'est bien la consigne du calibrateur que ce lot borne).
// `soundLo`/`soundHi` : plage d'angles ou la note se fait entendre.
// `stepMs` : avance du temps simule par tour, donc duree reelle d'une position.
std::vector<int> driveRangeFinder(AutoCalibrator& cal, HwFakeAudio& fa,
                                  int soundLo, int soundHi, unsigned long stepMs = 25) {
  std::vector<int> angles;
  int expMidi = middleNoteMidi();
  int it = 0;
  while (cal.isRunning() && it++ < 200000) {
    int ang = cal.getCurrentAngle();
    angles.push_back(ang);
    if (ang >= soundLo && ang <= soundHi) feedNote(fa, expMidi);
    else feedSilence(fa);
    fa.seq++; fa.ts = __test_millis;
    __test_millis += stepMs;
    cal.update();
  }
  assert(!cal.isRunning());
  return angles;
}

// --- Lecture des sources de production ---------------------------------------
// ConfigStorage.cpp et le sketch ne sont pas dans la compilation hote (LittleFS,
// ArduinoJson, esp_task_wdt). La ligne corrigee y est donc verrouillee par une
// verification textuelle, faute de pouvoir l'executer ici.
std::string readProductionSource(const std::string& relative) {
  static const char* prefixes[] = { "", "../", "../../", "../../../", "../../../../" };
  for (const char* prefix : prefixes) {
    std::ifstream in(std::string(prefix) + relative);
    if (in) {
      std::ostringstream ss;
      ss << in.rdbuf();
      return ss.str();
    }
  }
  return std::string();
}

std::string sliceBetween(const std::string& text, const std::string& from, const std::string& to) {
  size_t a = text.find(from);
  assert(a != std::string::npos);
  size_t b = text.find(to, a);
  assert(b != std::string::npos);
  return text.substr(a, b - a);
}

// =============================================================================
// DEFAUT 1 - le range finder balayait au-dela de la plage configuree
// =============================================================================

// La configuration de base doit etre acceptee par la validation, sinon les tests
// qui suivent prouveraient un refus pour la mauvaise raison.
void hw_boot_config_base_is_valid() {
  baseCfg();
  RuntimeConfig work;
  ConfigValidationResult r = validateCandidateConfig(cfg, work);
  assert(r.valid);
}

// La fenetre de balayage sort de la CONFIGURATION, pas d'un #define.
// Elle valait AUTOCAL_RF_MIN_SAFE_ANGLE..AUTOCAL_RF_MAX_SAFE_ANGLE (30..150)
// quelle que soit la mecanique installee.
void hw_boot_range_window_follows_configuration() {
  baseCfg();
  __test_millis = 0;
  HwFakeAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  HwFakeAirSupply as;
  AutoCalibrator cal(fc, ac, fa, as);
  cal.start(ACAL_MODE_RANGE_FIND);
  driveRangeFinder(cal, fa, 60, 100);
  assert(cal.getRangeSweepStart() == 60 - AUTOCAL_RF_EXPLORE_MARGIN_DEG);
  assert(cal.getRangeSweepEnd() == 100 + AUTOCAL_RF_EXPLORE_MARGIN_DEG);

  // Une plage large reste rognee par les bornes absolues : la marge elargit, elle
  // ne desactive pas le garde-fou.
  baseCfg();
  cfg.servoAirflowMin = 40; cfg.servoAirflowMax = 140;
  __test_millis = 0;
  HwFakeAudio fa2;
  AutoCalibrator cal2(fc, ac, fa2, as);
  cal2.start(ACAL_MODE_RANGE_FIND);
  driveRangeFinder(cal2, fa2, 40, 140);
  assert(cal2.getRangeSweepStart() == AUTOCAL_RF_MIN_SAFE_ANGLE);
  assert(cal2.getRangeSweepEnd() == AUTOCAL_RF_MAX_SAFE_ANGLE);
}

// Aucun angle commande ne sort de la fenetre declaree + marge.
// Avant : sur une plage 60..100, le balayage montait a 150 - 27 positions sur 41
// hors plage, dont plusieurs secondes au-dessus de servoAirflowMax, servo
// alimente en continu par la session de calibration, donc potentiellement en
// appui contre une butee tout ce temps.
void hw_boot_range_sweep_never_leaves_the_declared_travel() {
  baseCfg();
  __test_millis = 0;
  HwFakeAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  HwFakeAirSupply as;
  AutoCalibrator cal(fc, ac, fa, as);
  cal.start(ACAL_MODE_RANGE_FIND);
  std::vector<int> angles = driveRangeFinder(cal, fa, 60, 100);

  const int lo = 60 - AUTOCAL_RF_EXPLORE_MARGIN_DEG;
  const int hi = 100 + AUTOCAL_RF_EXPLORE_MARGIN_DEG;
  int maxSeen = 0, sweepSeen = 0;
  for (int a : angles) {
    // La position de repos (servoAirflowOff) encadre le balayage : elle n'en fait
    // pas partie et le materiel l'atteint a chaque note off.
    if (a == (int)cfg.servoAirflowOff) continue;
    assert(a >= lo && a <= hi);
    if (a > maxSeen) maxSeen = a;
    sweepSeen++;
  }
  assert(sweepSeen > 0);
  // Et surtout : l'ancienne borne fixe n'est jamais atteinte.
  assert(maxSeen < AUTOCAL_RF_MAX_SAFE_ANGLE);
  // Le balayage reste utile : il trouve bien la plage jouee.
  assert(cal.isRangeFinderComplete());
  assert(cal.getRangeFinderMin() >= 0 && cal.getRangeFinderMax() >= 0);
  assert(cal.getRangeFinderMin() <= 60 && cal.getRangeFinderMax() >= 100);
}

// Servo arrive en butee : il ne bouge plus, donc le son ne change plus, donc
// aucune perte n'est detectee. Le balayage allait au bout de la fenetre et
// ECRIVAIT ce bout dans servoAirflowMax ; ensuite chaque note forte du jeu normal
// commandait cet angle, c'est-a-dire la butee, a chaque note.
// Une limite haute non mesuree n'est plus inventee : la passe echoue.
void hw_boot_range_without_confirmed_loss_writes_nothing() {
  baseCfg();
  const uint16_t oldMin = cfg.servoAirflowMin, oldMax = cfg.servoAirflowMax;
  __test_millis = 0;
  HwFakeAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  HwFakeAirSupply as;
  AutoCalibrator cal(fc, ac, fa, as);
  cal.start(ACAL_MODE_RANGE_FIND);
  // La note se fait entendre a partir de 60 et ne s'arrete JAMAIS ensuite.
  driveRangeFinder(cal, fa, 60, 1000);

  assert(cal.isRangeFinderComplete());
  assert(!cal.isComplete());
  assert(cal.getRangeFailureReason() == ACAL_FAIL_RANGE_NOT_BOUNDED);
  assert(cal.getRangeFinderMin() < 0 && cal.getRangeFinderMax() < 0);

  int savesBefore = __config_save_calls;
  RangeApplyResult r = cal.applyRangeResults();
  assert(!r.applied && !r.saved);
  assert(__config_save_calls == savesBefore);          // rien n'est meme tente
  assert(cfg.servoAirflowMin == oldMin && cfg.servoAirflowMax == oldMax);
}

// Le temps passe hors plage est borne, pas seulement le nombre de pas : une
// position dont l'audio ralentit maintient le servo au-dela de la course declaree
// sans que le compte de positions ne bouge.
void hw_boot_range_exposure_budget_stops_the_sweep() {
  baseCfg();
  __test_millis = 0;
  HwFakeAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  HwFakeAirSupply as;
  AutoCalibrator cal(fc, ac, fa, as);
  cal.start(ACAL_MODE_RANGE_FIND);
  // Chaque position coute ici ~7x son cout nominal (trames espacees de 200 ms au
  // lieu de AUTOCAL_FRAME_SAMPLE_MS) : la source repond, mais lentement.
  std::vector<int> angles = driveRangeFinder(cal, fa, 60, 100, /*stepMs=*/200);

  assert(cal.isRangeFinderComplete());
  assert(cal.getRangeFailureReason() == ACAL_FAIL_RANGE_EXPOSURE);
  assert(cal.getRangeFinderMin() < 0 && cal.getRangeFinderMax() < 0);
  // Le balayage a ete coupe AVANT d'avoir parcouru toute la fenetre.
  int maxSeen = 0;
  for (int a : angles) if (a != (int)cfg.servoAirflowOff && a > maxSeen) maxSeen = a;
  assert(maxSeen < cal.getRangeSweepEnd());
}

// Un balayage sain reste TRES loin du budget : le garde-fou ne coute pas la
// fonctionnalite. (Sans cette assertion, un budget trop serre passerait inapercu.)
void hw_boot_range_healthy_sweep_stays_under_budget() {
  baseCfg();
  __test_millis = 0;
  HwFakeAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  HwFakeAirSupply as;
  AutoCalibrator cal(fc, ac, fa, as);
  cal.start(ACAL_MODE_RANGE_FIND);
  driveRangeFinder(cal, fa, 60, 100);
  assert(cal.isRangeFinderComplete());
  assert(cal.getRangeFailureReason() == ACAL_FAIL_NONE);
  assert(cal._rfOutOfRangeMs * 2 < (unsigned long)AUTOCAL_RF_OUT_OF_RANGE_BUDGET_MS);
}

// Les angles decouverts passent par la VALIDATION avant d'etre ecrits.
// Le seul controle etait min >= 0, max >= 0, max >= min : un couple hors course
// servo partait en configuration active puis en flash, et commandait ensuite le
// servo de souffle a chaque note.
void hw_boot_range_apply_refuses_angles_validation_rejects() {
  baseCfg();
  const uint16_t oldMin = cfg.servoAirflowMin, oldMax = cfg.servoAirflowMax;
  HwFakeAudio fa;
  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {}); ac.begin();
  HwFakeAirSupply as;
  AutoCalibrator cal(fc, ac, fa, as);

  // Hors course mecanique (> 180) : l'ancien controle les acceptait.
  cal._rfMinAngle = 200; cal._rfMaxAngle = 210;
  int savesBefore = __config_save_calls;
  RangeApplyResult r1 = cal.applyRangeResults();
  assert(!r1.applied && !r1.saved);
  assert(__config_save_calls == savesBefore);
  assert(cfg.servoAirflowMin == oldMin && cfg.servoAirflowMax == oldMax);
  assert(cal.getRangeFailureReason() == ACAL_FAIL_RANGE_INVALID);

  // Plage vide (min == max) : la validation exige min < max.
  cal._rfMinAngle = 90; cal._rfMaxAngle = 90;
  savesBefore = __config_save_calls;
  RangeApplyResult r2 = cal.applyRangeResults();
  assert(!r2.applied && !r2.saved);
  assert(__config_save_calls == savesBefore);
  assert(cfg.servoAirflowMin == oldMin && cfg.servoAirflowMax == oldMax);

  // Un resultat plausible reste accepte : la validation ne tue pas la fonction.
  cal._rfMinAngle = 55; cal._rfMaxAngle = 118;
  __config_save_result = true;
  RangeApplyResult r3 = cal.applyRangeResults();
  assert(r3.applied && r3.saved);
  assert(cfg.servoAirflowMin == 55 && cfg.servoAirflowMax == 118);
}

// =============================================================================
// DEFAUT 2 - saveFrom() validait cfg au lieu de son argument
// =============================================================================

// Valider un candidat ne doit RIEN ecrire dans la configuration active : la
// validation normalise sur place, et elle tournait sur `cfg` hors du verrou de
// configuration, depuis la tache qui persiste, pendant que la tache web peut lire
// `cfg` sous ce verrou.
void hw_boot_candidate_validation_never_touches_active_config() {
  baseCfg();
  // Configuration active deliberement NON normalisee : si la validation portait
  // sur elle, elle la corrigerait, et la copie ci-dessous le verrait.
  cfg.pumpCascadeThreshold = 200;   // hors 0..99, sera normalise... sur la COPIE
  RuntimeConfig before;
  memcpy(&before, &cfg, sizeof(cfg));

  RuntimeConfig candidate;
  memcpy(&candidate, &cfg, sizeof(cfg));
  candidate.servoAirflowMin = 70; candidate.servoAirflowMax = 110;

  RuntimeConfig work;
  ConfigValidationResult r = validateCandidateConfig(candidate, work);
  assert(r.valid);
  assert(memcmp(&before, &cfg, sizeof(cfg)) == 0);       // cfg intact
  assert(cfg.pumpCascadeThreshold == 200);               // pas normalise en place
  assert(work.pumpCascadeThreshold == 99);               // normalise sur la copie
  assert(work.servoAirflowMin == 70 && work.servoAirflowMax == 110);
}

// LE scenario prouve : un /config.json semantiquement invalide met l'instrument en
// recovery - correct - mais laisse `cfg` invalide en RAM. L'utilisateur corrige
// alors la configuration depuis l'interface web : le candidat est valide, puis la
// persistance revalidait l'ANCIENNE `cfg` toujours invalide, echouait, et rendait
// storage_failed. Une configuration cassee n'etait plus reparable depuis le seul
// outil dont dispose l'utilisateur.
void hw_boot_candidate_is_accepted_while_active_config_is_invalid() {
  baseCfg();
  // Deux doigts sur le meme canal PCA : invalide, et la validation ne peut pas le
  // corriger (elle ne peut pas deviner le bon canal).
  cfg.numFingers = 2;
  cfg.fingers[0].pcaChannel = 0; cfg.fingers[0].closedAngle = 90; cfg.fingers[0].direction = 1;
  cfg.fingers[1].pcaChannel = 0; cfg.fingers[1].closedAngle = 90; cfg.fingers[1].direction = 1;
  {
    RuntimeConfig check;
    ConfigValidationResult bad = validateCandidateConfig(cfg, check);
    assert(!bad.valid);                                  // l'etat de depart EST invalide
  }

  // Le candidat repare le conflit de canal.
  RuntimeConfig candidate;
  memcpy(&candidate, &cfg, sizeof(cfg));
  candidate.fingers[1].pcaChannel = 1;

  RuntimeConfig work;
  ConfigValidationResult r = validateCandidateConfig(candidate, work);
  assert(r.valid);                                       // la reparation est acceptee
  assert(cfg.fingers[1].pcaChannel == 0);                // cfg invalide, mais intact
}

// La ligne corrigee elle-meme : ConfigStorage.cpp n'est pas compilable sur hote
// (LittleFS + ArduinoJson), donc elle est verrouillee textuellement.
void hw_boot_save_from_validates_its_argument_not_the_global() {
  std::string src = readProductionSource("Servo_flute_ESP32/ConfigStorage.cpp");
  assert(!src.empty());   // source introuvable : le test ne prouverait rien
  std::string body = sliceBetween(src, "bool ConfigStorage::saveFrom(",
                                       "bool ConfigStorage::save()");
  assert(body.find("validateCandidateConfig(source") != std::string::npos);
  assert(body.find("validateAndNormalizeConfig(cfg") == std::string::npos);
}

// =============================================================================
// DEFAUT 3 - la configuration d'USINE etait traitee comme sure pour piloter
// =============================================================================

// Sur une carte vierge (LittleFS monte, /config.json absent), sept servos
// partaient ensemble vers les angles d'un preset arbitraire, a pleine vitesse,
// sur une mecanique qui n'est pas forcement celle-la.
void hw_boot_factory_defaults_are_not_safe_to_drive() {
  // LE cas corrige.
  assert(!bootConfigMayDriveActuators(true, CONFIG_DEFAULTS, true));
  // Une configuration reellement ecrite et valide reste seule a autoriser.
  assert(bootConfigMayDriveActuators(true, CONFIG_LOADED, true));
  assert(!bootConfigMayDriveActuators(true, CONFIG_LOADED, false));
  // Les refus deja en place ne bougent pas.
  assert(!bootConfigMayDriveActuators(true, CONFIG_INVALID_FALLBACK, true));
  assert(!bootConfigMayDriveActuators(true, CONFIG_STORAGE_ERROR, true));
  assert(!bootConfigMayDriveActuators(false, CONFIG_LOADED, true));
  assert(!bootConfigMayDriveActuators(false, CONFIG_DEFAULTS, true));
}

// Le sketch n'est pas compilable sur hote : on verrouille qu'il passe bien par le
// predicat, et qu'il explique la situation sur le port serie plutot que de rester
// muet (un instrument neuf serait indiscernable d'un instrument casse).
void hw_boot_sketch_uses_the_boot_predicate() {
  std::string ino = readProductionSource("Servo_flute_ESP32/Servo_flute_ESP32.ino");
  assert(!ino.empty());
  assert(ino.find("bootConfigMayDriveActuators(") != std::string::npos);
  size_t gate = ino.find("bool bootConfigSafe = fsMounted &&");
  assert(gate != std::string::npos);
  size_t build = ino.find("instrument = new InstrumentManager();");
  assert(build != std::string::npos && gate < build);
  // Le diagnostic est imprime hors de tout if (DEBUG).
  assert(ino.find("actionneurs DESACTIVES") != std::string::npos);
  assert(ino.find("/config.json absent") != std::string::npos);
}

// =============================================================================
// DEFAUT 4 - servoAngleToPWM bornait un angle negatif vers le MAXIMUM
// =============================================================================

// Le parametre etait uint16_t : `if (angle < SERVO_MIN_ANGLE)` etait du code mort,
// et un angle negatif ressortait a 180 degres - la course maximale - au lieu du
// bas de course. Le garde-fou de dernier recours echouait vers l'extremite haute.
void hw_boot_servo_angle_to_pwm_fails_low() {
  const uint16_t atMin = servoAngleToPWM(SERVO_MIN_ANGLE);
  const uint16_t atMax = servoAngleToPWM(SERVO_MAX_ANGLE);
  assert(atMin != atMax);

  // Angle negatif : retombe en bas de course, comme la ligne l'a toujours voulu.
  assert(servoAngleToPWM(-5) == atMin);
  assert(servoAngleToPWM(-1000) == atMin);

  // Negatif deja enroule par une conversion non signee chez l'appelant : c'est la
  // mesure citee par l'audit, qui rendait 180 degres.
  assert(servoAngleToPWM((uint16_t)-5) == atMin);
  assert(servoAngleToPWM((uint16_t)-5) != atMax);

  // Debordement MODESTE d'une arithmetique d'angle : toujours sature en haut
  // (comportement historique, conserve volontairement).
  assert(servoAngleToPWM(200) == atMax);
  assert(servoAngleToPWM(SERVO_ANGLE_SANE_LIMIT) == atMax);
  assert(servoAngleToPWM(SERVO_ANGLE_SANE_LIMIT + 1) == atMin);

  // La conversion nominale n'a pas bouge.
  assert(servoAngleToPWM(0) == atMin);
  assert(atMin < atMax);
  uint16_t expMin = (uint16_t)((float)SERVO_PULSE_MIN / 1000000.0 * SERVO_FREQUENCY * 4096.0 + 0.5f);
  uint16_t expMax = (uint16_t)((float)SERVO_PULSE_MAX / 1000000.0 * SERVO_FREQUENCY * 4096.0 + 0.5f);
  assert(atMin == expMin && atMax == expMax);
  uint16_t previous = 0;
  for (int a = SERVO_MIN_ANGLE; a <= SERVO_MAX_ANGLE; a++) {
    uint16_t v = servoAngleToPWM(a);
    assert(v >= previous);
    previous = v;
  }
}

}  // namespace

void hw_boot_run_all_tests() {
  hw_boot_config_base_is_valid();
  hw_boot_range_window_follows_configuration();
  hw_boot_range_sweep_never_leaves_the_declared_travel();
  hw_boot_range_without_confirmed_loss_writes_nothing();
  hw_boot_range_exposure_budget_stops_the_sweep();
  hw_boot_range_healthy_sweep_stays_under_budget();
  hw_boot_range_apply_refuses_angles_validation_rejects();
  hw_boot_candidate_validation_never_touches_active_config();
  hw_boot_candidate_is_accepted_while_active_config_is_invalid();
  hw_boot_save_from_validates_its_argument_not_the_global();
  hw_boot_factory_defaults_are_not_safe_to_drive();
  hw_boot_sketch_uses_the_boot_predicate();
  hw_boot_servo_angle_to_pwm_fails_low();
  std::cout << "hw boot tests passed\n";
}
