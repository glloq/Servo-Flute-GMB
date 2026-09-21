// Tests de non-regression de l'audit P0/P1.
//
// Chaque test cible un defaut precis corrige dans le firmware, de facon que la
// correction ne soit pas seulement declarative :
//   - EventQueue atomique (lecture + retrait sous le meme verrou),
//   - panic concurrent et absence d'execution apres clear/panic,
//   - protection centrale hardware_not_ready,
//   - table sinus SIGNEE du vibrato,
//   - commit transactionnel de configuration et rollback de sauvegarde,
//   - CC2 de silence jamais jete, CC120/121/123-127 jamais limites,
//   - sessions web (jetons, expiration, revocation),
//   - propriete des actionneurs pendant l'auto-calibration.
#include <cassert>
#include <iostream>
#include <map>
#include <cmath>
#include <string>
#include "Arduino.h"
#include "Wire.h"
#include "ConfigStorage.h"
#include "ConfigCommit.h"
#include "CommandQueue.h"
#include "EventQueue.h"
#include "FingerController.h"
#include "AirflowController.h"
#include "VibratoMath.h"
#include "NoteSequencer.h"
#include "InstrumentManager.h"
#include "WebAuth.h"
#include "Sha256.h"
#include "IAudioSource.h"
#include "ICalibrationAirSupply.h"
#include "CalibrationAirSupply.h"
#include "PitchMath.h"
#include "AutoCalMath.h"
#include "AutoCalibrator.h"

extern std::map<uint8_t, int> __analog_writes, __digital_writes, __analog_reads, __digital_reads;
extern int __pwm_write_count;

namespace {

// Configuration minimale et valide, commune aux tests de ce fichier.
void auditResetCfg() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.numFingers = 1;
  cfg.fingers[0].pcaChannel = 0;
  cfg.fingers[0].closedAngle = 90;
  cfg.fingers[0].direction = 1;
  cfg.numPumps = 1;
  cfg.pumpPins[0] = 25;
  cfg.pumpMinPwm[0] = 80;
  cfg.pumpMaxPwm[0] = 200;
  cfg.motorType = MOTOR_TYPE_PWM;
  cfg.airMode = AIR_MODE_SOLENOID_SERVO;
  cfg.servoToSolenoidDelayMs = 10;
  cfg.minNoteDurationMs = 0;
  cfg.minNoteIntervalForValveCloseMs = 0;
  cfg.airflowPcaChannel = 10;
  cfg.solenoidPin = 13;
  cfg.solenoidActivationTimeMs = 50;
  cfg.solenoidPwmActivation = 255;
  cfg.solenoidPwmHolding = 128;
  cfg.servoAirflowOff = 20;
  cfg.servoAirflowMin = 60;
  cfg.servoAirflowMax = 100;
  cfg.servoAngleOff = 90;
  cfg.servoAngleMin = 45;
  cfg.servoAngleMax = 135;
  cfg.ccVolumeDefault = 127;
  cfg.ccExpressionDefault = 127;
  cfg.ccBreathDefault = 127;
  cfg.ccBrightnessDefault = 64;
  cfg.airVelocityResponse = 100;
  cfg.cc2Enabled = true;
  cfg.cc2SilenceThreshold = 10;
  cfg.cc2ResponseCurve = 1.4f;
  cfg.cc2TimeoutMs = 1000;
  cfg.vibratoFrequencyHz = 6.0f;
  cfg.vibratoMaxAmplitudeDeg = 8.0f;
  cfg.midiStorageLimitKb = 500;
  strcpy(cfg.embouchure, "bec");
  strcpy(cfg.resFormat, "balloon");
  strcpy(cfg.instrumentColor, "#D4B044");
  cfg.numNotes = 3;
  cfg.notes[0].midiNote = 60;
  cfg.notes[1].midiNote = 61;
  cfg.notes[2].midiNote = 62;
  for (int i = 0; i < 3; i++) {
    cfg.notes[i].airflowMinPercent = 0;
    cfg.notes[i].airflowMaxPercent = 100;
    cfg.notes[i].airflowNominalPercent = 40;
  }
}

InstrumentManager* makeReadyInstrument() {
  Wire.clear();
  Wire.setPresent(PCA_ADDR_BOARD0, true);
  InstrumentManager* im = new InstrumentManager();
  assert(im->beginSafe());
  assert(im->isHardwareReady());
  return im;
}

}  // namespace

// --- 1. EventQueue : lecture et retrait du MEME evenement sous le MEME verrou --
//
// L'ancien NoteSequencer faisait peek() -> lecture du pointeur -> dequeue(). Entre
// les deux, une autre tache pouvait vider la file, inserer un Note Off force (qui
// evince la tete quand la file est pleine) ou deplacer _tail : le sequenceur
// executait alors un evenement obsolete ou en perdait un.
static void eventqueue_atomic_pop_is_indivisible() {
  EventQueue q(4);
  __test_millis = 1000;
  cfg.servoToSolenoidDelayMs = 0;

  // Rien n'est du tant que l'echeance n'est pas atteinte : aucun retrait.
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 60, 100, 2000));
  MidiEvent ev;
  assert(!q.tryPopDueEvent(__test_millis, 0, ev));
  assert(q.getCount() == 1);

  // Une fois due, un seul appel lit ET retire le meme evenement.
  __test_millis = 2000;
  assert(q.tryPopDueEvent(__test_millis, 0, ev));
  assert(ev.type == EVENT_NOTE_ON && ev.midiNote == 60);
  assert(q.isEmpty());
  assert(!q.tryPopDueEvent(__test_millis, 0, ev));

  // peekCopy() rend une COPIE : la file peut evoluer ensuite sans invalider rien.
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 61, 90, 2000));
  MidiEvent copy;
  assert(q.peekCopy(copy) && copy.midiNote == 61);
  q.clear();
  assert(copy.midiNote == 61);   // la copie reste valide apres le clear()
  assert(q.isEmpty());
}

// L'avance appliquee aux NOTE_ON est faite DANS le verrou, a partir du meme
// evenement que celui qui sera retire.
static void eventqueue_note_on_lead_is_applied_atomically() {
  EventQueue q(4);
  __test_millis = 1000;
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 60, 100, 1100));
  MidiEvent ev;
  assert(!q.tryPopDueEvent(1000, 50, ev));    // du a 1050, pas encore atteint
  assert(q.tryPopDueEvent(1050, 50, ev));     // avance de 50 ms appliquee
  assert(ev.midiNote == 60);

  // Un NOTE_OFF n'est jamais avance.
  assert(q.enqueueScheduledEvent(EVENT_NOTE_OFF, 60, 0, 1100));
  assert(!q.tryPopDueEvent(1050, 50, ev));
  assert(q.tryPopDueEvent(1100, 50, ev));
  assert(ev.type == EVENT_NOTE_OFF);
}

// Soustraction saturante : une avance superieure a l'horodatage rend l'evenement
// du immediatement et ne produit jamais un futur lointain par rollover.
static void eventqueue_lead_does_not_underflow() {
  EventQueue q(2);
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 60, 100, 10));
  MidiEvent ev;
  assert(q.tryPopDueEvent(20, 1000, ev));
  assert(ev.midiNote == 60);
}

// Rollover de millis() : la comparaison d'echeance reste correcte de part et
// d'autre du passage 0xFFFFFFFF -> 0.
static void eventqueue_millis_rollover() {
  EventQueue q(4);
  const unsigned long nearMax = 0xFFFFFFF0UL;
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 60, 100, nearMax));
  MidiEvent ev;
  assert(!q.tryPopDueEvent(nearMax - 5, 0, ev));
  assert(q.tryPopDueEvent(nearMax, 0, ev));

  // Evenement programme APRES le rollover, horloge encore avant : pas du.
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 62, 100, 10));
  assert(!q.tryPopDueEvent(nearMax, 0, ev));
  // Horloge passee de l'autre cote : du.
  assert(q.tryPopDueEvent(20, 0, ev));
  assert(ev.midiNote == 62);
}

// Un clear() concurrent incremente l'epoque : un consommateur qui traitait une
// salve le detecte et n'execute plus rien de l'ancien contexte.
static void eventqueue_clear_invalidates_burst() {
  EventQueue q(4);
  uint32_t before = q.epoch();
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 60, 100, 0));
  uint32_t popEpoch = 0;
  MidiEvent ev;
  assert(q.tryPopDueEvent(10, 0, ev, &popEpoch));
  assert(popEpoch == before);
  q.clear();
  assert(q.epoch() != before);
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 61, 100, 0));
  assert(q.tryPopDueEvent(10, 0, ev, &popEpoch));
  assert(popEpoch != before);   // le consommateur voit que le contexte a change
}

// Note Off force sur file pleine : il entre toujours (en evincant le plus ancien)
// car le perdre laisserait la valve et le souffle ouverts.
static void eventqueue_forced_note_off_on_full_queue() {
  EventQueue q(2);
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 60, 100, 0));
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 61, 100, 0));
  assert(q.isFull());
  assert(!q.enqueueScheduledEvent(EVENT_NOTE_OFF, 61, 0, 0));
  assert(q.enqueueScheduledEventForced(EVENT_NOTE_OFF, 61, 0, 0));
  assert(q.getCount() == 2);
  MidiEvent a, b;
  assert(q.tryPopDueEvent(10, 0, a) && a.midiNote == 61 && a.type == EVENT_NOTE_ON);
  assert(q.tryPopDueEvent(10, 0, b) && b.type == EVENT_NOTE_OFF);
  assert(q.isEmpty());
}

// Un clear() pendant le traitement d'une salve (panic simule par un
// AirflowController qui vide la file au premier mouvement) ne doit laisser
// passer aucun evenement de l'ancien contexte.
static void sequencer_no_event_executed_after_clear() {
  auditResetCfg();
  __test_millis = 0;
  EventQueue q(8);
  int fingerWrites = 0;
  FingerController fc([&](uint8_t, uint16_t, uint16_t) { fingerWrites++; });
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {});
  NoteSequencer ns(q, fc, ac);
  ns.begin();

  q.enqueueScheduledEvent(EVENT_NOTE_ON, 60, 100, 0);
  q.enqueueScheduledEvent(EVENT_NOTE_ON, 61, 100, 0);
  q.enqueueScheduledEvent(EVENT_NOTE_ON, 62, 100, 0);
  // Panic AVANT le traitement : plus rien ne doit etre joue.
  q.clear();
  fingerWrites = 0;
  ns.update();
  assert(fingerWrites == 0);
  assert(ns.getState() == STATE_IDLE);
  assert(q.isEmpty());
}

// Production concurrente pendant la consommation. Le scenario reproduit
// exactement ce que l'ancien code ne survivait pas : la tete est LUE, puis un
// producteur insere un Note Off force (qui evince la tete quand la file est
// pleine) avant que le consommateur ne "retire". Avec tryPopDueEvent(), lecture
// et retrait portent sur le meme evenement, donc l'eviction ne peut plus
// transformer le retrait en suppression d'un AUTRE evenement.
static void eventqueue_producer_interleaved_with_consumer() {
  EventQueue q(3);
  __test_millis = 100;
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 60, 100, 0));
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 61, 100, 0));
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 62, 100, 0));
  assert(q.isFull());

  // Le consommateur prend le premier evenement de facon indivisible.
  MidiEvent first;
  assert(q.tryPopDueEvent(__test_millis, 0, first));
  assert(first.midiNote == 60);
  assert(q.getCount() == 2);

  // Un producteur insere ensuite deux evenements, dont un Note Off force sur une
  // file redevenue pleine : il evince la tete (note 61).
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 63, 100, 0));
  assert(q.isFull());
  assert(q.enqueueScheduledEventForced(EVENT_NOTE_OFF, 63, 0, 0));

  // La consommation reste coherente : chaque retrait rend l'evenement qui etait
  // reellement en tete, sans doublon ni saut.
  MidiEvent e;
  assert(q.tryPopDueEvent(__test_millis, 0, e) && e.midiNote == 62 && e.type == EVENT_NOTE_ON);
  assert(q.tryPopDueEvent(__test_millis, 0, e) && e.midiNote == 63 && e.type == EVENT_NOTE_ON);
  assert(q.tryPopDueEvent(__test_millis, 0, e) && e.midiNote == 63 && e.type == EVENT_NOTE_OFF);
  assert(q.isEmpty());
  // Le Note Off a survecu a la saturation : c'est lui qui relache la note.
}

// Un panic declenche pendant qu'une note joue doit tout relacher, y compris la
// source d'air. C'est le chemin emprunte par forceAP() / la perte de transport
// reseau pendant une note tenue.
static void panic_during_active_note_releases_everything() {
  auditResetCfg();
  cfg.airMode = AIR_MODE_PUMP_VALVE;
  cfg.pumpFollowAirflow = true;
  cfg.pumpDirectMaxPercent = 100;
  cfg.pumpDirectIdlePercent = 10;
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  im->noteOn(60, 120);
  for (int i = 0; i < 4; i++) { __test_millis += 20; im->update(); }
  assert(im->getSequencer().getState() != STATE_IDLE);
  assert(im->getPressureCtrl().getTargetPercent() > cfg.pumpDirectIdlePercent);

  // Panic poste depuis une autre tache (deconnexion BLE, bascule reseau...).
  im->requestPanic();
  __test_millis += 20;
  im->update();

  assert(im->getSequencer().getState() == STATE_IDLE);
  assert(!im->getAirflowCtrl().isValveOpen());
  assert(im->getPressureCtrl().getTargetPercent() == 0);
  assert(!im->commandQueue().panicPending());

  // Le panic TIENT : un tour de boucle supplementaire ne doit pas remettre la
  // pompe a sa demande de repos (le retour force a STATE_IDLE n'est pas une fin
  // de note normale).
  for (int i = 0; i < 3; i++) { __test_millis += 20; im->update(); }
  assert(im->getPressureCtrl().getTargetPercent() == 0);

  // Une VRAIE note suivante reapplique bien la demande de jeu.
  im->noteOn(61, 120);
  for (int i = 0; i < 4; i++) { __test_millis += 20; im->update(); }
  assert(im->getPressureCtrl().getTargetPercent() > cfg.pumpDirectIdlePercent);
  im->allSoundOff();
  delete im;
}

// --- 2. CommandQueue : panic prioritaire, jamais perdu ------------------------
static void commandqueue_panic_never_dropped_and_cancels_pending() {
  CommandQueue q(2);
  assert(q.push(ActuatorCommand(ACMD_TEST_AIRFLOW_ANGLE, 0, 0, 120)));
  assert(q.push(ActuatorCommand(ACMD_PUMP_TARGET, 0, 80)));
  assert(!q.push(ActuatorCommand(ACMD_FAN_TARGET, 0, 50)));   // pleine
  assert(q.droppedCount() == 1);

  // Le panic ne prend pas de place dans l'anneau et annule les commandes en
  // attente : un test de pompe emis avant l'arret d'urgence ne doit jamais
  // s'appliquer apres lui.
  q.requestPanic();
  assert(q.panicPending());
  assert(q.count() == 0);
  ActuatorCommand c;
  assert(!q.pop(c));
  assert(q.takePanicRequest());
  assert(!q.takePanicRequest());   // consomme une seule fois
}

// --- 3. Protection centrale hardware_not_ready -------------------------------
//
// Apres un echec PCA0, AUCUNE commande actionneur ne doit aboutir, quel que soit
// le chemin : note, CC, test de doigt, test de souffle, solenoide, pompe,
// ventilateur, angle de servo, ouverture des doigts.
static void hardware_not_ready_refuses_every_actuator_command() {
  auditResetCfg();
  Wire.clear();   // PCA0 absent
  __test_millis = 0;
  __pwm_write_count = 0;
  __digital_writes.clear();
  __analog_writes.clear();
  cfg.airMode = AIR_MODE_PUMP_VALVE;

  InstrumentManager im;
  assert(!im.beginSafe());
  assert(!im.isHardwareReady());

  const uint8_t commands[] = {
    ACMD_NOTE_ON, ACMD_NOTE_OFF, ACMD_CONTROL_CHANGE, ACMD_TEST_FINGER,
    ACMD_TEST_AIRFLOW_ANGLE, ACMD_TEST_ANGLE_SERVO, ACMD_AIR_LIVE_PERCENT,
    ACMD_ANGLE_LIVE_PERCENT, ACMD_TEST_SOLENOID, ACMD_PUMP_TARGET,
    ACMD_PUMP_SINGLE_TEST, ACMD_PUMP_ENABLE, ACMD_FAN_TARGET, ACMD_OPEN_ALL_FINGERS
  };
  for (uint8_t type : commands) {
    assert(InstrumentManager::commandDrivesActuators(type));
    im.postCommand(type, 60, 100, 120);
  }
  for (int i = 0; i < 5; i++) { __test_millis += 20; im.update(); }

  assert(__pwm_write_count == 0);                       // aucun servo pilote
  assert(__digital_writes[PIN_SERVOS_OFF] == HIGH);     // OE jamais active
  assert(__analog_writes.find(25) == __analog_writes.end() || __analog_writes[25] == 0);
  assert(im.getSequencer().getState() == STATE_IDLE);
}

// Les commandes NON physiques (arrets, reset de controleurs) restent acceptees :
// elles ne peuvent que remettre en securite.
static void hardware_not_ready_still_allows_safe_commands() {
  assert(!InstrumentManager::commandDrivesActuators(ACMD_ALL_SOUND_OFF));
  assert(!InstrumentManager::commandDrivesActuators(ACMD_RESET_CONTROLLERS));
  assert(!InstrumentManager::commandDrivesActuators(ACMD_PUMP_STOP));
  assert(!InstrumentManager::commandDrivesActuators(ACMD_FAN_STOP));
  assert(!InstrumentManager::commandDrivesActuators(ACMD_SET_ACTUATOR_SESSION));
  assert(InstrumentManager::commandDrivesActuators(ACMD_NOTE_ON));
}

// Un panic reste consomme meme sans hardware : la file ne se remplit pas et
// allSoundOff() ne touche aucun GPIO non initialise.
static void hardware_not_ready_panic_is_consumed() {
  auditResetCfg();
  Wire.clear();
  InstrumentManager im;
  assert(!im.beginSafe());
  im.requestPanic();
  __test_millis += 10;
  im.update();
  assert(!im.commandQueue().panicPending());
}

// Une fois le hardware pret, la MEME commande est bien appliquee (le refus vient
// de l'etat hardware, pas d'une commande devenue inerte).
static void hardware_ready_applies_actuator_command() {
  auditResetCfg();
  __pwm_write_count = 0;
  InstrumentManager* im = makeReadyInstrument();
  int before = __pwm_write_count;
  im->postCommand(ACMD_TEST_AIRFLOW_ANGLE, 0, 0, 120);
  __test_millis += 10;
  im->update();
  assert(__pwm_write_count > before);
  delete im;
}

// --- 4. Vibrato : la table sinus est SIGNEE ----------------------------------
//
// SIN_LUT est en int8_t mais etait lue avec pgm_read_byte(), qui rend un octet
// NON signe : la demi-periode negative remontait entre +129 et +255. Le vibrato
// etait unipolaire (le souffle n'oscillait jamais sous la valeur nominale).
static void vibrato_lut_is_signed() {
  // Bornes : toujours dans [-1, +1].
  float minV = 2.0f, maxV = -2.0f;
  for (int i = 0; i < SIN_LUT_SIZE; i++) {
    float v = VibratoMath::sinLutAt((uint8_t)i);
    assert(v >= -1.0f && v <= 1.0f);
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }
  // La moitie negative existe reellement.
  assert(minV < -0.99f);
  assert(maxV > 0.99f);

  // Quadrants : 0 deg ~ 0, 90 deg ~ +1, 180 deg ~ 0, 270 deg ~ -1.
  assert(fabsf(VibratoMath::sinLutAt(0)) < 0.02f);
  assert(VibratoMath::sinLutAt(64) > 0.99f);
  assert(fabsf(VibratoMath::sinLutAt(128)) < 0.02f);
  assert(VibratoMath::sinLutAt(192) < -0.99f);
}

static void vibrato_fast_sin_quadrants_and_guards() {
  const float freq = 1.0f;          // periode de 1000 ms -> 1 ms par 0,36 deg
  assert(fabsf(VibratoMath::fastSin(0, freq)) < 0.02f);          // 0 deg
  assert(VibratoMath::fastSin(250, freq) > 0.99f);               // 90 deg
  assert(fabsf(VibratoMath::fastSin(500, freq)) < 0.02f);        // 180 deg
  assert(VibratoMath::fastSin(750, freq) < -0.99f);              // 270 deg

  // Toutes les valeurs restent bornees, sur une periode complete.
  for (unsigned long t = 0; t < 1000; t += 7) {
    float v = VibratoMath::fastSin(t, freq);
    assert(v >= -1.0f && v <= 1.0f);
  }
  // Et au moins une valeur strictement negative est produite (test du bug).
  bool sawNegative = false;
  for (unsigned long t = 0; t < 1000; t += 7) {
    if (VibratoMath::fastSin(t, freq) < -0.5f) sawNegative = true;
  }
  assert(sawNegative);

  // Gardes : frequence nulle, negative, non finie ou trop haute -> 0 (jamais de
  // modulo par zero).
  assert(VibratoMath::fastSin(123, 0.0f) == 0.0f);
  assert(VibratoMath::fastSin(123, -3.0f) == 0.0f);
  assert(VibratoMath::fastSin(123, 5000.0f) == 0.0f);
  assert(VibratoMath::fastSin(123, NAN) == 0.0f);
  assert(VibratoMath::fastSin(123, INFINITY) == 0.0f);
}

// --- 5. Validation de configuration : flottants et bornes --------------------
static void config_float_validation() {
  auditResetCfg();
  cfg.vibratoFrequencyHz = NAN;
  cfg.vibratoMaxAmplitudeDeg = INFINITY;
  cfg.cc2ResponseCurve = 0.0f;
  ConfigValidationResult r = validateAndNormalizeConfig(cfg, nullptr);
  assert(r.valid);
  assert(r.corrected);
  assert(isfinite(cfg.vibratoFrequencyHz) && cfg.vibratoFrequencyHz >= CONFIG_MIN_VIBRATO_HZ);
  assert(isfinite(cfg.vibratoMaxAmplitudeDeg) && cfg.vibratoMaxAmplitudeDeg <= CONFIG_MAX_VIBRATO_DEG);
  assert(cfg.cc2ResponseCurve >= CONFIG_MIN_CC2_CURVE);
  // Une frequence de vibrato valide produit toujours une periode non nulle.
  assert(VibratoMath::fastSin(10, cfg.vibratoFrequencyHz) >= -1.0f);

  // Bornes hautes.
  auditResetCfg();
  cfg.vibratoFrequencyHz = 1e6f;
  cfg.cc2ResponseCurve = 1e6f;
  validateAndNormalizeConfig(cfg, nullptr);
  assert(cfg.vibratoFrequencyHz <= CONFIG_MAX_VIBRATO_HZ);
  assert(cfg.cc2ResponseCurve <= CONFIG_MAX_CC2_CURVE);
}

static void config_string_and_timing_validation() {
  auditResetCfg();
  strcpy(cfg.embouchure, "zzz");
  strcpy(cfg.resFormat, "\"evil");
  strcpy(cfg.instrumentColor, "nope");
  cfg.ccVolumeDefault = 200;
  cfg.cc2SilenceThreshold = 250;
  cfg.airAttackMs = 5;
  cfg.midiStorageLimitKb = 1;
  cfg.kbdMode = 9;
  ConfigValidationResult r = validateAndNormalizeConfig(cfg, nullptr);
  assert(r.valid && r.corrected);
  assert(strcmp(cfg.embouchure, "trav") == 0);
  assert(strcmp(cfg.resFormat, "balloon") == 0);
  assert(strcmp(cfg.instrumentColor, "#D4B044") == 0);
  assert(cfg.ccVolumeDefault <= MIDI_CC_MAX);
  assert(cfg.cc2SilenceThreshold <= MIDI_CC_MAX);
  assert(cfg.airAttackMs >= CONFIG_MIN_ATTACK_MS);
  assert(cfg.midiStorageLimitKb >= CONFIG_MIN_MIDI_LIMIT_KB);
  assert(cfg.kbdMode <= 1);
}

// --- 6. Commit transactionnel de configuration -------------------------------

static bool g_saveOk = true;
static int g_saveCalls = 0;
static RuntimeConfig g_savedConfig;
static bool auditSave(const RuntimeConfig& c) {
  g_saveCalls++;
  if (!g_saveOk) return false;
  g_savedConfig = c;
  return true;
}

// Succes : le candidat est sauvegarde PUIS active en une seule affectation.
static void config_commit_activates_only_after_save() {
  auditResetCfg();
  RuntimeConfig active = cfg;
  RuntimeConfig candidate = active;
  candidate.ccVolumeDefault = 64;
  g_saveOk = true; g_saveCalls = 0;

  ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &auditSave);
  assert(r.valid && r.saved && r.applied && r.activated && !r.restartRequired);
  assert(g_saveCalls == 1);
  assert(active.ccVolumeDefault == 64);
  assert(g_savedConfig.ccVolumeDefault == 64);
}

// Candidat invalide : rien n'est sauvegarde, rien n'est active.
static void config_commit_rejects_invalid_candidate_without_touching_active() {
  auditResetCfg();
  RuntimeConfig active = cfg;
  RuntimeConfig candidate = active;
  candidate.numNotes = 2;
  candidate.notes[0].midiNote = 60;
  candidate.notes[1].midiNote = 60;   // doublon -> invalide
  g_saveOk = true; g_saveCalls = 0;

  ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &auditSave);
  assert(!r.valid && !r.saved && !r.applied && !r.activated);
  assert(g_saveCalls == 0);
  assert(r.error.length() > 0);
  assert(active.numNotes == cfg.numNotes);   // configuration active intacte
}

// Sauvegarde impossible : la configuration active et les controleurs restent sur
// la configuration precedemment persistee (aucun etat applique-mais-non-sauve).
static void config_commit_rolls_back_on_storage_failure() {
  auditResetCfg();
  RuntimeConfig active = cfg;
  uint8_t before = active.ccVolumeDefault;
  RuntimeConfig candidate = active;
  candidate.ccVolumeDefault = 11;
  g_saveOk = false; g_saveCalls = 0;

  ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &auditSave);
  assert(r.valid && !r.saved && !r.applied && !r.activated);
  assert(g_saveCalls == 1);
  assert(r.error == "storage_failed");
  assert(active.ccVolumeDefault == before);
}

// Changement demandant une re-init hardware : sauvegarde mais PAS active. La
// configuration active reste celle qui correspond au hardware initialise.
static void config_commit_keeps_active_config_when_restart_required() {
  auditResetCfg();
  RuntimeConfig active = cfg;
  RuntimeConfig candidate = active;
  candidate.solenoidPin = 27;          // changement de GPIO -> re-init requise
  g_saveOk = true; g_saveCalls = 0;

  ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &auditSave);
  assert(r.valid && r.saved);
  assert(r.restartRequired);
  assert(!r.applied && !r.activated);
  assert(active.solenoidPin == cfg.solenoidPin);   // encore l'ancienne broche
  assert(g_savedConfig.solenoidPin == 27);         // mais la nouvelle est persistee
}

// Aucun etat intermediaire : meme si le candidat est modifie champ par champ par
// l'appelant, la configuration active ne bouge qu'au commit.
static void config_commit_no_intermediate_state_visible() {
  auditResetCfg();
  RuntimeConfig active = cfg;
  RuntimeConfig candidate = active;
  // Modifications successives sur le CANDIDAT.
  candidate.ccVolumeDefault = 1;
  assert(active.ccVolumeDefault != 1);
  candidate.servoAirflowMin = 70;
  assert(active.servoAirflowMin != 70);
  candidate.servoAirflowMax = 130;
  assert(active.servoAirflowMax != 130);

  g_saveOk = true;
  ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &auditSave);
  assert(r.applied);
  assert(active.ccVolumeDefault == 1 && active.servoAirflowMin == 70 && active.servoAirflowMax == 130);
}

// --- 7. Limiteur de debit MIDI ----------------------------------------------

// Les CC de mode canal (120-127) ne sont JAMAIS jetes par le limiteur.
// CC120 / CC123 / CC124-127 arretent le son ; CC121 remet les controleurs a leurs
// valeurs par defaut (sans arreter la note, conformement a la norme MIDI). Dans
// les deux cas le message doit passer meme fenetre de debit saturee : l'ancien
// code ne dispensait que 120, 121 et 123, donc 124-127 pouvaient etre jetes et
// laisser une note soufflee valve ouverte.
static void cc_channel_mode_messages_are_never_rate_limited() {
  auditResetCfg();
  InstrumentManager* im = makeReadyInstrument();

  const byte silencing[] = {120, 123, 124, 125, 126, 127};
  for (byte cc : silencing) {
    __test_millis += 10;
    im->noteOn(60, 100);
    for (int i = 0; i < 3; i++) { __test_millis += 20; im->update(); }
    assert(im->getSequencer().getState() != STATE_IDLE);
    // Saturer largement la fenetre de debit AVANT d'envoyer le CC de securite.
    for (int i = 0; i < CC_RATE_LIMIT_PER_SECOND * 5; i++) im->handleControlChange(MIDI_CC_VOLUME, 100);
    im->handleControlChange(cc, 0);
    __test_millis += 20;
    im->update();
    assert(im->getSequencer().getState() == STATE_IDLE);
    assert(!im->getAirflowCtrl().isValveOpen());
  }

  // CC121 Reset All Controllers : applique meme fenetre saturee.
  __test_millis += CC_RATE_WINDOW_MS + 10;   // nouvelle fenetre de debit
  im->handleControlChange(MIDI_CC_VOLUME, 5);
  assert(im->getCCVolume() == 5);
  for (int i = 0; i < CC_RATE_LIMIT_PER_SECOND * 5; i++) im->handleControlChange(MIDI_CC_VOLUME, 100);
  im->handleControlChange(MIDI_CC_RESET_ALL_CONTROLLERS, 0);
  assert(im->getCCVolume() == cfg.ccVolumeDefault);

  // Un CC ordinaire, lui, EST bien limite : sans cela le test ci-dessus ne
  // prouverait rien (le limiteur pourrait simplement ne jamais agir).
  __test_millis += CC_RATE_WINDOW_MS + 10;
  im->handleControlChange(MIDI_CC_VOLUME, 1);
  assert(im->getCCVolume() == 1);
  for (int i = 0; i < CC_RATE_LIMIT_PER_SECOND * 3; i++) im->handleControlChange(MIDI_CC_VOLUME, 90);
  im->handleControlChange(MIDI_CC_VOLUME, 42);
  assert(im->getCCVolume() != 42);   // jete par le limiteur de debit

  delete im;
}

// Une demande de silence CC2 (valeur sous le seuil) est TOUJOURS appliquee, meme
// sous une rafale qui sature la fenetre de debit.
static void cc2_silence_request_is_never_dropped() {
  auditResetCfg();
  cfg.cc2Enabled = true;
  cfg.cc2SilenceThreshold = 10;
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  im->noteOn(60, 100);
  for (int i = 0; i < 3; i++) { __test_millis += 20; im->update(); }
  assert(im->getAirflowCtrl().isValveOpen());

  // Rafale de CC2 non silencieux : bien au-dela de la limite par seconde.
  for (int i = 0; i < CC2_RATE_LIMIT_PER_SECOND * 4; i++) im->handleControlChange(MIDI_CC_BREATH, 100);
  im->update();
  assert(im->getAirflowCtrl().isValveOpen());   // la note souffle toujours

  // Le controleur demande zero : la valeur DOIT passer immediatement, bien que la
  // fenetre de debit soit saturee. Le lissage CC2 moyenne les CC2_SMOOTHING_BUFFER_SIZE
  // dernieres valeurs, donc il faut ce nombre de zeros pour descendre sous le seuil :
  // si UN SEUL d'entre eux etait jete par le limiteur, la note continuerait a souffler.
  for (uint8_t i = 0; i < CC2_SMOOTHING_BUFFER_SIZE; i++) {
    im->handleControlChange(MIDI_CC_BREATH, 0);
    assert(im->getCCBreath() == 0);   // chaque zero est bien traite, jamais mis en attente
    im->update();
  }
  assert(!im->getAirflowCtrl().isValveOpen());  // le souffle est coupe
  delete im;
}

// Sous rafale, la DERNIERE valeur CC2 recue est conservee puis appliquee (aucune
// perte silencieuse) : c'est la coalescence.
static void cc2_last_value_is_coalesced_not_lost() {
  auditResetCfg();
  cfg.cc2Enabled = true;
  cfg.cc2SilenceThreshold = 10;
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  im->noteOn(60, 100);
  for (int i = 0; i < 3; i++) { __test_millis += 20; im->update(); }

  // Saturer la fenetre, puis envoyer une derniere valeur distinctive.
  for (int i = 0; i < CC2_RATE_LIMIT_PER_SECOND + 5; i++) im->handleControlChange(MIDI_CC_BREATH, 90);
  im->handleControlChange(MIDI_CC_BREATH, 77);
  assert(im->getCCBreath() != 77);   // mise en attente, pas encore appliquee

  // Fenetre suivante : la derniere valeur est appliquee sans nouvel envoi.
  __test_millis += CC_RATE_WINDOW_MS + 10;
  im->update();
  assert(im->getCCBreath() == 77);
  delete im;
}

// --- 8. Propriete des actionneurs pendant l'auto-calibration -----------------
static void no_actuator_command_during_calibration_except_owner() {
  auditResetCfg();
  __pwm_write_count = 0;
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  im->setActuatorSessionActive(true);
  assert(im->isActuatorSessionActive());

  int before = __pwm_write_count;
  // Toute commande externe (web, MIDI) postee pendant la session est appliquee
  // par update() mais refusee par les gardes de noteOn/CC ; les tests de servo
  // sont eux bloques en amont par WebConfigurator (voir
  // actuatorCommandBlockedDuringCalibration). Ici on verifie le coeur : MIDI et
  // CC n'atteignent pas les actionneurs.
  im->postCommand(ACMD_NOTE_ON, 60, 100);
  im->postCommand(ACMD_CONTROL_CHANGE, MIDI_CC_VOLUME, 10);
  for (int i = 0; i < 3; i++) { __test_millis += 20; im->update(); }
  assert(__pwm_write_count == before);
  assert(im->getSequencer().getState() == STATE_IDLE);

  // Le proprietaire (le calibrateur) pilote directement les controleurs : c'est
  // le seul chemin autorise pendant la session.
  im->getAirflowCtrl().testAirflowAngle(120);
  assert(__pwm_write_count > before);

  im->setActuatorSessionActive(false);
  delete im;
}

// --- 9. Sessions web ---------------------------------------------------------
static uint32_t g_rngState = 12345;
static uint32_t auditRng() {
  g_rngState = g_rngState * 1664525u + 1013904223u;
  return g_rngState;
}

static void web_auth_sessions() {
  WebAuth auth;
  auth.begin(auditRng, 1000);

  // Un jeton inconnu (ou vide) est refuse.
  assert(!auth.validate("", 0));
  assert(!auth.validate("deadbeef", 0));

  String t1 = auth.createSession(0);
  assert(t1.length() == WEB_AUTH_TOKEN_LEN);
  assert(auth.validate(t1, 0));
  assert(auth.activeSessions(0) == 1);

  // Deux sessions successives ne produisent pas le meme jeton.
  String t2 = auth.createSession(0);
  assert(t2 != t1);
  assert(auth.validate(t2, 0));

  // Expiration glissante : un acces avant l'echeance repousse celle-ci.
  assert(auth.validate(t1, 900));
  assert(auth.validate(t1, 1800));
  // Sans acces, la session expire.
  assert(!auth.validate(t2, 5000));

  // Revocation explicite et globale.
  auth.revoke(t1);
  assert(!auth.validate(t1, 1800));
  String t3 = auth.createSession(2000);
  assert(auth.validate(t3, 2000));
  auth.revokeAll();
  assert(!auth.validate(t3, 2000));
  assert(auth.activeSessions(2000) == 0);

  // Plus de sessions que de places : la plus proche de l'expiration est recyclee,
  // jamais une session encore fraiche.
  auth.begin(auditRng, 1000);
  String tokens[WEB_AUTH_MAX_SESSIONS + 1];
  for (int i = 0; i <= WEB_AUTH_MAX_SESSIONS; i++) tokens[i] = auth.createSession(i * 10);
  assert(auth.validate(tokens[WEB_AUTH_MAX_SESSIONS], 100));
  assert(!auth.validate(tokens[0], 100));   // la plus ancienne a ete recyclee
}

static void web_auth_secret_comparison() {
  assert(webAuthSecretEquals(String("hunter2"), String("hunter2")));
  assert(!webAuthSecretEquals(String("hunter2"), String("hunter3")));
  assert(!webAuthSecretEquals(String("hunter2"), String("hunter")));
  assert(!webAuthSecretEquals(String("hunter"), String("hunter2")));
  assert(webAuthSecretEquals(String(""), String("")));
}

// --- 10. SHA-256 autonome (derivation du mot de passe administrateur) --------
//
// L'implementation locale remplace mbedTLS, dont les noms de fonctions changent
// entre ESP-IDF 4.x et 5.x. Vecteurs de reference FIPS 180-4.
static String sha256Hex(const char* text) {
  uint8_t digest[SHA256_DIGEST_SIZE];
  Sha256::hash((const uint8_t*)text, strlen(text), digest);
  static const char* hexDigits = "0123456789abcdef";
  String out;
  for (uint8_t i = 0; i < SHA256_DIGEST_SIZE; i++) {
    out += hexDigits[(digest[i] >> 4) & 0x0F];
    out += hexDigits[digest[i] & 0x0F];
  }
  return out;
}

static void sha256_known_answers() {
  assert(sha256Hex("") ==
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(sha256Hex("abc") ==
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // Message plus long que plusieurs blocs (exerce le remplissage multi-bloc).
  String longText;
  for (int i = 0; i < 1000; i++) longText += 'a';
  uint8_t digest[SHA256_DIGEST_SIZE];
  Sha256::hash((const uint8_t*)longText.c_str(), longText.length(), digest);
  // SHA-256("a" x 1000)
  String out;
  static const char* hexDigits = "0123456789abcdef";
  for (uint8_t i = 0; i < SHA256_DIGEST_SIZE; i++) {
    out += hexDigits[(digest[i] >> 4) & 0x0F];
    out += hexDigits[digest[i] & 0x0F];
  }
  assert(out == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");

  // L'etat est reinitialise apres finish() : deux hachages successifs sur la
  // meme instance donnent le meme resultat qu'une instance neuve.
  Sha256 ctx;
  uint8_t d1[SHA256_DIGEST_SIZE], d2[SHA256_DIGEST_SIZE];
  ctx.update((const uint8_t*)"abc", 3);
  ctx.finish(d1);
  ctx.update((const uint8_t*)"abc", 3);
  ctx.finish(d2);
  assert(memcmp(d1, d2, SHA256_DIGEST_SIZE) == 0);
}


namespace {

/*******************************************************************************
 * INTEGRATION : InstrumentManager + session web + AutoCalibrator
 *
 * Les tests de calibration existants exercent AutoCalibrator ISOLEMENT, avec ses
 * propres controleurs. C'est exactement ce qui a laisse passer le defaut P0 : la
 * calibration n'etait pas cassee par le calibrateur, mais par l'INTERACTION
 * entre WebConfigurator (qui reclamait la session a chaque tour de boucle) et
 * InstrumentManager (dont la prise de session appelle _sequencer.stop(), donc
 * ferme la valve et ramene le servo de souffle au repos). Les tests ci-dessous
 * font tourner les trois ensemble, sur les MEMES controleurs, dans l'ordre reel
 * de loop() : serveur web d'abord, instrument ensuite.
 ******************************************************************************/

// Source audio simulee : pas d'I2S sur hote.
struct AuditAudio : public IAudioSource {
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

// Modele PHYSIQUE de la flute. Point essentiel : le son est derive de l'etat
// REEL des actionneurs (valve + angle du servo de souffle), jamais de l'intention
// du calibrateur. C'est ce qui rend ce test capable de voir le defaut : quand un
// tiers referme la valve ou ramene le servo au repos entre le positionnement et
// la mesure, le micro simule n'entend plus rien - exactement comme le vrai.
void auditFluteModel(AirflowController& air, int expectedMidi, int lo, int hi,
                     AuditAudio& f) {
  int pct = -1;
  if (air.isValveOpen()) {
    int range = (int)cfg.servoAirflowMax - (int)cfg.servoAirflowMin;
    if (range > 0) {
      pct = ((int)air.getAirflowAngle() - (int)cfg.servoAirflowMin) * 100 / range;
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }
  }
  if (pct >= lo && pct <= hi) {
    f.rms = 0.06f; f.valid = true; f.conf = 0.95f; f.midi = expectedMidi;
    f.hz = PitchMath::midiToHz(expectedMidi); f.cents = -4.0f; f.snd = true;
  } else if (pct > hi) {
    f.rms = 0.06f; f.valid = true; f.conf = 0.95f; f.midi = expectedMidi + 12;
    f.hz = PitchMath::midiToHz(expectedMidi + 12); f.cents = -4.0f; f.snd = true;
  } else {
    // Valve fermee ou souffle insuffisant : uniquement le bruit de fond.
    f.rms = 0.003f; f.valid = false; f.conf = 0.0f; f.midi = 0; f.hz = 0; f.cents = 0; f.snd = false;
  }
}

// Modele FIDELE du pilote de session de WebConfigurator : meme sequence d'appels
// que executeWebOp(WEBOP_AUTOCAL_START_*), WebConfigurator::update() et
// cancelActiveActuatorSession().
struct AuditWebSession {
  InstrumentManager& im;
  AutoCalibrator& cal;
  bool calCancelRequested = false;   // drapeau non perdable (cf. _calCancelRequested)
  bool sessionOwned = false;
  // Reproduit l'ancien WebConfigurator::update() : re-demande de la session a
  // CHAQUE tour de boucle tant que la calibration tourne.
  bool reclaimEveryLoop = false;
  // CONTROLE NEGATIF : effet de bord exact de l'ancienne prise de session non
  // idempotente (_sequencer.stop() a chaque re-demande). Sert uniquement a
  // prouver que les assertions ci-dessous detectent bien le defaut.
  bool legacyNonIdempotent = false;
  int cancelCount = 0;

  AuditWebSession(InstrumentManager& i, AutoCalibrator& c) : im(i), cal(c) {}

  void startCalibration(AutoCalMode mode) {
    im.setActuatorSessionActive(true);
    sessionOwned = true;
    cal.start(mode);
  }
  void requestCalibrationCancel() { calCancelRequested = true; }
  void cancelActiveActuatorSession() {
    if (cal.isRunning() || cal.isComplete() || cal.isRangeFinderComplete()) cal.stop();
    im.setActuatorSessionActive(false);
    sessionOwned = false;
    cancelCount++;
  }
  void update() {
    if (calCancelRequested) {
      calCancelRequested = false;
      cancelActiveActuatorSession();
    }
    if (cal.isRunning()) {
      if (reclaimEveryLoop) im.setActuatorSessionActive(true);
      if (legacyNonIdempotent) im.getSequencer().stop();
      cal.update();
    } else if (sessionOwned && (cal.isComplete() || cal.isRangeFinderComplete())) {
      cancelActiveActuatorSession();
    }
  }
};

// Observations recoltees pendant un cycle complet.
struct AuditCalRun {
  bool resultValid = false;
  uint8_t airMin = 0, airMax = 0, airNominal = 0;
  uint8_t failureReason = 0;
  bool sawValveOpen = false;
  int loops = 0;
  bool sessionReleased = false;
};

// Execute une calibration de souffle complete a travers la vraie boucle
// principale (web puis instrument), avec le vrai InstrumentManager, ses propres
// controleurs, et la vraie CalibrationAirSupply branchee sur eux.
AuditCalRun runCalibrationThroughLoop(bool reclaimEveryLoop, bool legacyNonIdempotent) {
  auditResetCfg();
  cfg.numNotes = 1;                       // un seul cycle : rapide et suffisant
  cfg.notes[0].midiNote = 60;
  cfg.airMode = AIR_MODE_SOLENOID_SERVO;  // source d'air passive : toujours prete

  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  AuditAudio audio;
  // Exactement le cablage de production (WebConfigurator::begin) : le calibrateur
  // recoit les controleurs REELS de l'instrument et sa vraie passerelle d'air.
  AutoCalibrator cal(im->getFingerCtrl(), im->getAirflowCtrl(), audio,
                     im->getCalibrationAirSupply());
  AuditWebSession web(*im, cal);
  web.reclaimEveryLoop = reclaimEveryLoop;
  web.legacyNonIdempotent = legacyNonIdempotent;

  web.startCalibration(ACAL_MODE_AIRFLOW);
  assert(im->isActuatorSessionActive());

  AuditCalRun out;
  while ((cal.isRunning() || web.sessionOwned) && out.loops < 200000) {
    auditFluteModel(im->getAirflowCtrl(), 60, 20, 60, audio);
    audio.seq++;
    audio.ts = __test_millis;
    __test_millis += 25;
    web.update();                       // wireless->update() : WebConfigurator
    im->update();                       // instrument->update()
    if (im->getAirflowCtrl().isValveOpen()) out.sawValveOpen = true;
    out.loops++;
  }

  AutoCalNoteResult r = cal.getResult(0);
  out.resultValid = r.valid;
  out.airMin = r.airMin;
  out.airMax = r.airMax;
  out.airNominal = r.airNominal;
  out.failureReason = r.failureReason;
  out.sessionReleased = !im->isActuatorSessionActive();
  delete im;
  return out;
}

// P0 : la prise de session repetee ne doit plus casser la calibration.
void calibration_session_survives_repeated_ownership() {
  // 1. Prise de session UNE fois (comportement corrige du WebConfigurator).
  AuditCalRun once = runCalibrationThroughLoop(/*reclaimEveryLoop=*/false,
                                               /*legacyNonIdempotent=*/false);
  assert(once.sawValveOpen);
  assert(once.resultValid);
  // Le modele fait sonner la note entre 20 % et 60 % de souffle. Le calibrateur
  // doit retrouver cette bande a un pas de balayage pres (la conversion
  // pourcentage -> angle est quantifiee : 40 degres pour 100 %).
  assert(once.airMin <= 25 && once.airMax >= 55);
  assert(once.airMin < once.airNominal && once.airNominal < once.airMax);
  // La session est rendue a la fin : la gestion d'energie peut reprendre.
  assert(once.sessionReleased);

  // 2. Meme cycle, mais la session est re-demandee a CHAQUE tour de boucle.
  // Comme setActuatorSessionActive() est desormais idempotent, le resultat doit
  // etre RIGOUREUSEMENT identique.
  AuditCalRun repeated = runCalibrationThroughLoop(/*reclaimEveryLoop=*/true,
                                                   /*legacyNonIdempotent=*/false);
  assert(repeated.resultValid);
  assert(repeated.airMin == once.airMin);
  assert(repeated.airMax == once.airMax);
  assert(repeated.airNominal == once.airNominal);
  assert(repeated.sessionReleased);

  // 3. CONTROLE NEGATIF. On rejoue l'effet de bord exact de l'ancienne prise de
  // session non idempotente : _sequencer.stop() a chaque tour, donc valve
  // refermee et servo de souffle ramene au repos entre le positionnement et la
  // mesure. La calibration DOIT alors echouer - c'est la preuve que les
  // assertions ci-dessus ne sont pas vides, et c'est exactement ce que voyait
  // l'utilisateur : "aucun son detecte".
  AuditCalRun broken = runCalibrationThroughLoop(/*reclaimEveryLoop=*/true,
                                                 /*legacyNonIdempotent=*/true);
  assert(!broken.resultValid);
  assert(broken.failureReason == ACAL_FAIL_NO_SOUND ||
         broken.failureReason == ACAL_FAIL_NOTE_TIMEOUT ||
         broken.failureReason == ACAL_FAIL_GLOBAL_TIMEOUT);
}

// P0 (assertion directe) : pendant une session d'actionneurs, ni la valve ni
// l'angle de souffle poses par le proprietaire ne doivent bouger, quel que soit
// le nombre de fois ou la session est re-demandee.
void actuator_session_take_is_idempotent() {
  auditResetCfg();
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  im->setActuatorSessionActive(true);
  // Ce que fait le calibrateur a l'entree d'une position : ouvrir le chemin
  // d'air une fois, poser l'angle une fois. Il ne les reapplique PAS pendant
  // ST_SETTLE / ST_COLLECT.
  im->getAirflowCtrl().testSolenoid(true);
  im->getAirflowCtrl().testAirflowAngle(120);
  assert(im->getAirflowCtrl().isValveOpen());
  assert(im->getAirflowCtrl().getAirflowAngle() == 120);

  for (int i = 0; i < 50; i++) {
    __test_millis += 20;
    im->setActuatorSessionActive(true);   // l'appel repetitif de l'ancien update()
    im->update();
  }
  assert(im->isActuatorSessionActive());
  assert(im->getAirflowCtrl().isValveOpen());
  assert(im->getAirflowCtrl().getAirflowAngle() == 120);

  // La liberation, elle, n'est pas idempotente au sens "sans effet" : elle rend
  // la main a la gestion d'energie. Une seconde liberation ne doit rien casser.
  im->setActuatorSessionActive(false);
  im->setActuatorSessionActive(false);
  assert(!im->isActuatorSessionActive());
  delete im;
}

// Finding 2 : pendant la session, la source d'air appartient au calibrateur.
// updateAirSourceFromSequencer() ne doit plus ecraser sa demande.
void calibration_owns_the_air_source() {
  auditResetCfg();
  cfg.airMode = AIR_MODE_PUMP_VALVE;
  cfg.pumpDirectIdlePercent = 10;
  cfg.pumpDirectMaxPercent = 90;
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  ICalibrationAirSupply& supply = im->getCalibrationAirSupply();

  // Point ESSENTIEL : une note est EN COURS quand la calibration demarre. C'est
  // le cas reel (la calibration se lance depuis l'interface pendant que le
  // lecteur MIDI joue ; WEBOP_AUTOCAL_START_* met d'ailleurs le lecteur en
  // pause). Sans note prealable, le sequenceur est deja a STATE_IDLE et il n'y a
  // aucune transition a mal interpreter : le defaut ne se reproduit pas.
  im->noteOn(60, 100);
  for (int i = 0; i < 6; i++) { __test_millis += 20; im->update(); }
  assert(im->getSequencer().getState() == STATE_PLAYING);
  assert(im->getPressureCtrl().getTargetPercent() > cfg.pumpDirectIdlePercent);

  // Prise de session : elle arrete le sequenceur (retour force a STATE_IDLE).
  im->setActuatorSessionActive(true);
  supply.prepare();                      // demande representative du mode
  supply.setDemandPercent(100);
  uint8_t demanded = im->getPressureCtrl().getTargetPercent();
  assert(demanded == cfg.pumpDirectMaxPercent);

  for (int i = 0; i < 40; i++) { __test_millis += 20; im->update(); }
  // Sans le garde ET le realignement, le retour force a STATE_IDLE etait lu au
  // tour suivant comme une fin de note et remettait la pompe au ralenti sous les
  // pieds du calibrateur.
  assert(im->getPressureCtrl().getTargetPercent() == demanded);

  // Session rendue : le sequenceur reprend la main et redescend au ralenti.
  im->setActuatorSessionActive(false);
  im->noteOn(60, 100);
  for (int i = 0; i < 5; i++) { __test_millis += 20; im->update(); }
  im->noteOff(60);
  for (int i = 0; i < 20; i++) { __test_millis += 30; im->update(); }
  assert(im->getSequencer().getState() == STATE_IDLE);
  assert(im->getPressureCtrl().getTargetPercent() == cfg.pumpDirectIdlePercent);
  delete im;
}

// Finding 3 : l'annulation de calibration ne doit jamais etre perdue.
void calibration_cancel_is_never_lost() {
  auditResetCfg();
  cfg.numNotes = 1;
  cfg.notes[0].midiNote = 60;
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  AuditAudio audio;
  AutoCalibrator cal(im->getFingerCtrl(), im->getAirflowCtrl(), audio,
                     im->getCalibrationAirSupply());
  AuditWebSession web(*im, cal);

  web.startCalibration(ACAL_MODE_AIRFLOW);
  for (int i = 0; i < 40; i++) {
    auditFluteModel(im->getAirflowCtrl(), 60, 20, 60, audio);
    audio.seq++; audio.ts = __test_millis; __test_millis += 25;
    web.update(); im->update();
  }
  assert(cal.isRunning());

  // Le drapeau est pose depuis la tache AsyncTCP (panic / deconnexion du
  // proprietaire / stop). Contrairement a un postWebOp(), il ne peut pas etre
  // refuse : il est consomme au tour suivant, quoi qu'il arrive.
  web.requestCalibrationCancel();
  web.update();
  assert(!cal.isRunning());
  assert(!im->isActuatorSessionActive());
  assert(web.cancelCount == 1);

  // Une annulation posee alors que rien ne tourne ne doit rien casser.
  web.requestCalibrationCancel();
  web.update();
  assert(!im->isActuatorSessionActive());
  delete im;
}

// Finding 4 : un NOTE OFF ne doit jamais etre perdu, meme file pleine.
void note_off_is_never_dropped_on_full_command_queue() {
  auditResetCfg();
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  im->noteOn(60, 100);
  for (int i = 0; i < 3; i++) { __test_millis += 20; im->update(); }
  assert(im->getAirflowCtrl().isValveOpen());

  // Saturer l'anneau de commandes : tous les push suivants echouent.
  int accepted = 0;
  for (int i = 0; i < COMMAND_QUEUE_SIZE * 4; i++) {
    if (im->postCommand(ACMD_TEST_FINGER, 0, 0, 90)) accepted++;
  }
  assert(accepted == COMMAND_QUEUE_SIZE);   // l'anneau est plein, le reste est jete

  // Le relachement passe malgre tout : il n'emprunte pas l'anneau.
  assert(im->postCommand(ACMD_NOTE_OFF, 60));
  assert(im->commandQueue().hasPendingNoteOff());

  for (int i = 0; i < 20; i++) { __test_millis += 30; im->update(); }
  assert(!im->commandQueue().hasPendingNoteOff());
  assert(!im->getAirflowCtrl().isValveOpen());
  assert(im->getSequencer().getState() == STATE_IDLE);
  delete im;
}

// Finding 5 : un silence demande par CC2 doit faire retomber la source d'air,
// et la remontee du CC2 doit la restaurer - sans aucune transition d'etat du
// sequenceur, puisque la note reste TENUE.
void cc2_silence_drops_air_source_and_restores() {
  auditResetCfg();
  cfg.airMode = AIR_MODE_PUMP_VALVE;
  cfg.cc2Enabled = true;
  cfg.cc2SilenceThreshold = 10;
  cfg.pumpDirectIdlePercent = 10;
  cfg.pumpDirectMaxPercent = 90;
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  im->noteOn(60, 100);
  for (int i = 0; i < 6; i++) { __test_millis += 20; im->update(); }
  assert(im->getSequencer().getState() == STATE_PLAYING);
  assert(im->getAirflowCtrl().isNoteSounding());
  uint8_t playing = im->getPressureCtrl().getTargetPercent();
  assert(playing > cfg.pumpDirectIdlePercent);

  // Souffle a zero : le lissage CC2 moyenne les dernieres valeurs, il faut donc
  // remplir la fenetre pour passer sous le seuil de silence.
  for (uint8_t i = 0; i < CC2_SMOOTHING_BUFFER_SIZE; i++) {
    im->handleControlChange(MIDI_CC_BREATH, 0);
    __test_millis += 20;
    im->update();
  }
  assert(!im->getAirflowCtrl().isNoteSounding());
  assert(!im->getAirflowCtrl().isValveOpen());
  // La note est toujours TENUE : aucune transition du sequenceur n'a eu lieu.
  assert(im->getSequencer().getState() == STATE_PLAYING);
  // La pompe doit malgre tout etre redescendue au ralenti : elle poussait
  // jusqu'ici a pleine demande contre une valve fermee.
  assert(im->getPressureCtrl().getTargetPercent() == cfg.pumpDirectIdlePercent);

  // Remontee du souffle : la demande est restauree.
  for (uint8_t i = 0; i < CC2_SMOOTHING_BUFFER_SIZE; i++) {
    im->handleControlChange(MIDI_CC_BREATH, 127);
    __test_millis += 20;
    im->update();
  }
  assert(im->getAirflowCtrl().isNoteSounding());
  assert(im->getPressureCtrl().getTargetPercent() == playing);
  delete im;
}

// Finding 11 : Reset All Controllers doit aussi remettre l'etat runtime
// d'expression (lissage CC2, timeout CC2, mode d'attaque CC73).
void reset_all_controllers_clears_expression_runtime_state() {
  auditResetCfg();
  cfg.cc2Enabled = true;
  cfg.airAttackMode = 0;
  cfg.airAttackOffset = 20;
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  // CC73 change le mode d'attaque runtime, CC2 remplit le tampon de lissage.
  im->handleControlChange(MIDI_CC_ATTACK_TIME, 120);
  assert(im->getAirflowCtrl().runtimeAttackMode() != cfg.airAttackMode);
  for (uint8_t i = 0; i < CC2_SMOOTHING_BUFFER_SIZE; i++) {
    im->handleControlChange(MIDI_CC_BREATH, 5);
    __test_millis += 20;
    im->update();
  }
  assert(im->getCCBreath() == 5);

  im->handleControlChange(MIDI_CC_RESET_ALL_CONTROLLERS, 0);
  __test_millis += 20;
  im->update();

  assert(im->getAirflowCtrl().runtimeAttackMode() == cfg.airAttackMode);
  assert(im->getAirflowCtrl().runtimeAttackOffset() == cfg.airAttackOffset);
  assert(im->getCCBreath() == cfg.ccBreathDefault);
  // Le tampon de lissage est reparti du defaut : une note jouee juste apres
  // n'herite plus du souffle quasi nul precedent.
  im->noteOn(60, 100);
  for (int i = 0; i < 6; i++) { __test_millis += 20; im->update(); }
  assert(im->getAirflowCtrl().isNoteSounding());
  delete im;
}

// Finding 10 : le vibrato REEL doit osciller symetriquement autour de l'angle de
// base. Le test de formule plus bas documente le biais ; celui-ci verifie le
// chemin de production (AirflowController::update()).
void vibrato_oscillation_is_symmetric_around_base() {
  auditResetCfg();
  cfg.cc2Enabled = false;                 // pas de souffle CC2 dans ce test
  cfg.vibratoFrequencyHz = 6.0f;
  cfg.vibratoMaxAmplitudeDeg = 8.0f;
  // Plage large et nominal au centre : l'oscillation ne doit jamais etre ecretee
  // par _currentMinAngle / _currentMaxAngle, sinon la symetrie testee serait
  // celle du limiteur et non celle de l'arrondi.
  cfg.servoAirflowOff = 10;
  cfg.servoAirflowMin = 40;
  cfg.servoAirflowMax = 140;
  for (int i = 0; i < 3; i++) {
    cfg.notes[i].airflowMinPercent = 0;
    cfg.notes[i].airflowMaxPercent = 100;
    cfg.notes[i].airflowNominalPercent = 50;
  }
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  // CC1 = 127 : modulation au maximum, donc vibrato actif a pleine amplitude.
  im->handleControlChange(MIDI_CC_MODULATION, MIDI_CC_MAX);
  im->noteOn(60, 100);
  for (int i = 0; i < 6; i++) { __test_millis += 20; im->update(); }
  assert(im->getAirflowCtrl().isNoteSounding());

  const int base = (int)im->getAirflowCtrl().getBaseAirflowAngle();
  int lowest = base, highest = base;
  // Plusieurs periodes completes (6 Hz => 167 ms), pas de 1 ms pour visiter
  // finement les deux alternances.
  for (int i = 0; i < 1200; i++) {
    __test_millis += 1;
    im->update();
    int a = (int)im->getAirflowCtrl().getAirflowAngle();
    if (a < lowest) lowest = a;
    if (a > highest) highest = a;
  }
  const int below = base - lowest;
  const int above = highest - base;
  assert(below > 0 && above > 0);          // le vibrato oscille bien des deux cotes
  assert(below == above);                  // et symetriquement
  // Amplitude conforme a la configuration (8 degres a CC1 = 127).
  assert(above == (int)cfg.vibratoMaxAmplitudeDeg);
  delete im;
}

// Finding 10 : l'arrondi du vibrato doit etre symetrique.
void vibrato_rounding_is_symmetric() {
  // (int16_t)(x + 0.5) tronque vers zero : -0.6 donnait 0 et +0.6 donnait 1,
  // soit un demi-degre de biais vers le haut sur toute l'alternance negative.
  auto biased = [](float x) { return (int16_t)(x + 0.5f); };
  auto fixed  = [](float x) { return (int16_t)lroundf(x); };
  assert(biased(-0.6f) == 0 && fixed(-0.6f) == -1);
  assert(biased(-1.6f) == -1 && fixed(-1.6f) == -2);
  assert(fixed(0.6f) == 1);
  // Symetrie exacte sur toute la plage utile d'amplitude.
  for (int i = 1; i <= 200; i++) {
    float v = i * 0.05f;
    assert(fixed(v) == -fixed(-v));
  }
}

}  // namespace

void audit_run_all_tests() {
  sha256_known_answers();
  eventqueue_atomic_pop_is_indivisible();
  eventqueue_note_on_lead_is_applied_atomically();
  eventqueue_lead_does_not_underflow();
  eventqueue_millis_rollover();
  eventqueue_clear_invalidates_burst();
  eventqueue_forced_note_off_on_full_queue();
  eventqueue_producer_interleaved_with_consumer();
  panic_during_active_note_releases_everything();
  sequencer_no_event_executed_after_clear();
  commandqueue_panic_never_dropped_and_cancels_pending();
  hardware_not_ready_refuses_every_actuator_command();
  hardware_not_ready_still_allows_safe_commands();
  hardware_not_ready_panic_is_consumed();
  hardware_ready_applies_actuator_command();
  vibrato_lut_is_signed();
  vibrato_fast_sin_quadrants_and_guards();
  config_float_validation();
  config_string_and_timing_validation();
  config_commit_activates_only_after_save();
  config_commit_rejects_invalid_candidate_without_touching_active();
  config_commit_rolls_back_on_storage_failure();
  config_commit_keeps_active_config_when_restart_required();
  config_commit_no_intermediate_state_visible();
  cc_channel_mode_messages_are_never_rate_limited();
  cc2_silence_request_is_never_dropped();
  cc2_last_value_is_coalesced_not_lost();
  no_actuator_command_during_calibration_except_owner();
  web_auth_sessions();
  web_auth_secret_comparison();
  actuator_session_take_is_idempotent();
  calibration_session_survives_repeated_ownership();
  calibration_owns_the_air_source();
  calibration_cancel_is_never_lost();
  note_off_is_never_dropped_on_full_command_queue();
  cc2_silence_drops_air_source_and_restores();
  reset_all_controllers_clears_expression_runtime_state();
  vibrato_oscillation_is_symmetric_around_base();
  vibrato_rounding_is_symmetric();
}
