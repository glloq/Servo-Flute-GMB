// Finalisation - LOT A : les ordres d'ARRET ne doivent pas etre perdables, et
// l'ordre d'emission des notes doit etre respecte.
//
// CE QUE CES TESTS PROUVENT, ET CE QU'ILS NE PROUVENT PAS
// -------------------------------------------------------
// Les commandes arrivent de taches FreeRTOS distinctes de loop() (AsyncTCP pour
// HTTP/WebSocket, hote NimBLE, MIDI). Un test sur HOTE est MONO-TACHE et
// `portENTER_CRITICAL` y est un no-op : rien ici ne demontre l'absence de
// course. Ce qui est verrouille est le CONTRAT que le code de production doit
// offrir :
//
//   - un ordre qui RETIRE de l'energie (arret pompe, arret ventilateur, arret
//     d'une pompe, pump_enable=false, fermeture de la valve) n'emprunte pas
//     l'anneau : il ne peut donc pas etre refuse quand celui-ci est plein, et
//     `postCommand()` rend TOUJOURS true ;
//   - il est applique a la passe COURANTE, jamais derriere la borne de travail
//     par passe (INSTRUMENT_MAX_COMMANDS_PER_UPDATE), exactement comme le panic ;
//   - N demandes identiques se coalescent en UNE application, et JAMAIS en zero ;
//   - une commande d'ALIMENTATION deja en file ne peut pas realimenter
//     l'actionneur APRES l'arret : l'arret purge de l'anneau ce qui le
//     realimenterait, comme le panic vide l'anneau en entier ;
//   - un Note Off suit l'ordre FIFO comme tout le monde, et ne bascule sur le
//     bitmap non perdable que lorsque l'anneau REFUSE - le repli n'est pas une
//     perte et ne se compte pas comme telle ;
//   - une transition de configuration qui change la TOPOLOGIE materielle
//     (broche de fin de course, son niveau actif, broche du capteur Hall)
//     impose un redemarrage.
//
// L'etat verifie est celui de l'ACTIONNEUR (PWM reellement ecrit sur la broche,
// etat du controleur), pas seulement la valeur rendue par postCommand().
//
// Niveau de validation atteint : EXECUTE SUR HOTE. Rien n'a tourne sur ESP32.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include "Arduino.h"
#include "Wire.h"
#include "ConfigStorage.h"
#include "CommandQueue.h"
#include "InstrumentManager.h"

extern std::map<uint8_t, int> __analog_writes, __digital_writes;

namespace {

const uint8_t kFinPumpPin = 25;
const uint8_t kFinSolenoidPin = 13;
const uint8_t kFinFanPin = 26;

// Reglages communs aux deux configurations d'essai. Temps courts pour que les
// passes d'update() restent lisibles.
void finCommonCfg() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.numFingers = 1;
  cfg.fingers[0].pcaChannel = 0;
  cfg.fingers[0].closedAngle = 90;
  cfg.fingers[0].direction = 1;
  cfg.airflowPcaChannel = 10;
  cfg.solenoidPin = kFinSolenoidPin;
  cfg.solenoidActivationTimeMs = 50;
  cfg.solenoidPwmActivation = 255;
  cfg.solenoidPwmHolding = 128;
  cfg.servoToSolenoidDelayMs = 10;
  cfg.minNoteDurationMs = 0;
  cfg.minNoteIntervalForValveCloseMs = 0;
  cfg.timeUnpower = 5000;          // pas de coupure servo pendant les essais
  cfg.servoAirflowOff = 20;
  cfg.servoAirflowMin = 60;
  cfg.servoAirflowMax = 100;
  cfg.servoAngleOff = 90;
  cfg.servoAngleMin = 45;
  cfg.servoAngleMax = 135;
  cfg.ccVolumeDefault = 100;
  cfg.ccExpressionDefault = 127;
  cfg.ccBreathDefault = 127;
  cfg.ccBrightnessDefault = 64;
  cfg.airVelocityResponse = 100;
  cfg.cc2Enabled = true;
  cfg.cc2SilenceThreshold = 10;
  cfg.cc2ResponseCurve = 1.4f;
  cfg.cc2TimeoutMs = 0;
  cfg.vibratoFrequencyHz = 6.0f;
  cfg.vibratoMaxAmplitudeDeg = 8.0f;
  strcpy(cfg.embouchure, "bec");
  cfg.numNotes = 3;
  cfg.notes[0].midiNote = 60;
  cfg.notes[1].midiNote = 61;
  cfg.notes[2].midiNote = 62;
  for (int i = 0; i < 3; i++) {
    cfg.notes[i].airflowMinPercent = 0;
    cfg.notes[i].airflowMaxPercent = 100;
    cfg.notes[i].airflowNominalPercent = 40;
  }
  __analog_writes.clear();
  __digital_writes.clear();
}

// Pompe DIRECTE + valve a solenoide : le PWM de la pompe et celui de la bobine
// sont tous deux observables sur une broche.
void finResetCfg() {
  finCommonCfg();
  cfg.airMode = AIR_MODE_PUMP_VALVE;
  cfg.valveType = 0;                      // solenoide GPIO
  cfg.numPumps = 1;
  cfg.pumpPins[0] = kFinPumpPin;
  cfg.pumpMinPwm[0] = 80;
  cfg.pumpMaxPwm[0] = 200;
  cfg.motorType = MOTOR_TYPE_PWM;
  cfg.pumpFollowAirflow = true;
  cfg.pumpDirectMaxPercent = 100;
  cfg.pumpDirectIdlePercent = 0;
}

// Ventilateur : seul ce mode fait tourner FanController::update(), donc seul lui
// rend le PWM du ventilateur observable sur la broche.
void finResetCfgFan() {
  finCommonCfg();
  cfg.airMode = AIR_MODE_FAN_SERVO;
  cfg.fanPin = kFinFanPin;
  cfg.fanMinPwm = 60;
  cfg.fanMaxPwm = 240;
  cfg.fanIdlePercent = 0;
  cfg.fanIdleTimeoutMs = 0;
  cfg.fanDefaultPercent = 0;
  cfg.fanMaxNotePercent = 100;
  cfg.fanFollowAirflow = true;
}

InstrumentManager* finMakeReadyInstrument() {
  Wire.clear();
  Wire.setPresent(PCA_ADDR_BOARD0, true);
  InstrumentManager* im = new InstrumentManager();
  assert(im->beginSafe());
  assert(im->isHardwareReady());
  return im;
}

// Sature l'anneau avec des commandes NEUTRES pour la source d'air (un angle de
// doigt) : l'anneau est plein sans qu'aucune de ces commandes ne puisse
// realimenter la pompe, le ventilateur ou la valve.
// Exactement COMMAND_QUEUE_SIZE depots, pas un de plus : un push REFUSE
// incremente le compteur de commandes perdues, et plusieurs tests ci-dessous
// veulent pouvoir affirmer que ce compteur vaut toujours zero.
uint8_t finSaturateRing(InstrumentManager* im) {
  uint8_t accepted = 0;
  for (uint8_t i = 0; i < COMMAND_QUEUE_SIZE; i++) {
    if (!im->postCommand(ACMD_TEST_FINGER, 0, 0, 90)) break;
    accepted++;
  }
  return accepted;
}

// Sature l'anneau avec des consignes de pompe CROISSANTES : chacune realimente
// la pompe, donc le dernier `getTargetPercent()` dit exactement laquelle a ete
// appliquee en dernier.
uint8_t finFillRingWithPumpTargets(InstrumentManager* im) {
  uint8_t accepted = 0;
  for (uint8_t i = 0; i < COMMAND_QUEUE_SIZE; i++) {
    if (!im->postCommand(ACMD_PUMP_TARGET, 0, (uint8_t)(i + 1))) break;
    accepted++;
  }
  return accepted;
}

void finRunPasses(InstrumentManager* im, int passes, unsigned long stepMs) {
  for (int i = 0; i < passes; i++) {
    __test_millis += stepMs;
    im->update();
  }
}

/*=============================================================================
 * A-1 - un ordre d'ARRET ne doit jamais etre perdu par saturation de l'anneau
 *
 * LE DEFAUT, tel qu'il existe dans le depot :
 *   InstrumentManager::postCommand() route hors de l'anneau le panic, les Note
 *   Off et le CC121 ; TOUT LE RESTE finit sur `return _commands.push(cmd);`,
 *   qui rend false quand l'anneau est plein. WebConfigurator ignore cette
 *   valeur de retour et, pour "pump_stop" comme pour "fan_stop", appelle
 *   ensuite endTestSession(false), qui ANNULE le timeout de securite de la
 *   session d'essai. Anneau plein = l'ordre d'arret est jete ET le garde-fou
 *   temporel est retire : la pompe reste alimentee a sa consigne, sans limite
 *   de duree.
 *
 * Ces tests verifient l'etat de l'ACTIONNEUR (PWM reellement ecrit), pas
 * seulement la valeur rendue par postCommand().
 *===========================================================================*/

void pump_stop_reaches_the_pump_even_when_the_ring_is_full() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  // La pompe tourne REELLEMENT.
  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 80));
  finRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kFinPumpPin] > 0);

  // L'anneau est plein : toute commande ordinaire est desormais refusee.
  assert(finSaturateRing(im) == COMMAND_QUEUE_SIZE);
  assert(!im->postCommand(ACMD_TEST_FINGER, 0, 0, 90));

  // L'ordre d'arret, lui, DOIT passer.
  assert(im->postCommand(ACMD_PUMP_STOP));

  finRunPasses(im, 1, 10);
  assert(!im->getPressureCtrl().isPumpRunning());
  assert(im->getPressureCtrl().getTargetPercent() == 0);
  assert(__analog_writes[kFinPumpPin] == 0);

  delete im;
}

void fan_stop_reaches_the_fan_even_when_the_ring_is_full() {
  finResetCfgFan();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  // Le ventilateur tourne REELLEMENT (la rampe dure 300 ms).
  assert(im->postCommand(ACMD_FAN_TARGET, 0, 80));
  finRunPasses(im, 6, 100);
  assert(im->getFanCtrl().isRunning());
  assert(__analog_writes[kFinFanPin] > 0);

  assert(finSaturateRing(im) == COMMAND_QUEUE_SIZE);
  assert(!im->postCommand(ACMD_FAN_TARGET, 0, 80));

  assert(im->postCommand(ACMD_FAN_STOP));

  finRunPasses(im, 1, 10);
  assert(!im->getFanCtrl().isRunning());
  assert(im->getFanCtrl().getSpeed() == 0);
  assert(__analog_writes[kFinFanPin] == 0);

  delete im;
}

void single_pump_stop_reaches_the_pump_even_when_the_ring_is_full() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  // Test mono-pompe en cours : la pompe 0 est alimentee.
  assert(im->postCommand(ACMD_PUMP_SINGLE_TEST, 0, 60));
  finRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kFinPumpPin] > 0);

  assert(finSaturateRing(im) == COMMAND_QUEUE_SIZE);

  assert(im->postCommand(ACMD_PUMP_STOP_SINGLE, 0));

  finRunPasses(im, 1, 10);
  assert(!im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kFinPumpPin] == 0);

  delete im;
}

void pump_disable_reaches_the_pump_even_when_the_ring_is_full() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 80));
  finRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().isPumpRunning());

  assert(finSaturateRing(im) == COMMAND_QUEUE_SIZE);

  // pump_enable = false RETIRE de l'energie : non perdable.
  assert(im->postCommand(ACMD_PUMP_ENABLE, 0));

  finRunPasses(im, 1, 10);
  assert(!im->getPressureCtrl().isEnabled());
  assert(!im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kFinPumpPin] == 0);

  // Et la coupure TIENT : les passes suivantes ne la defont pas, alors meme que
  // la consigne de 80 % est toujours memorisee.
  finRunPasses(im, 4, 10);
  assert(!im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kFinPumpPin] == 0);

  delete im;
}

void solenoid_close_reaches_the_valve_even_when_the_ring_is_full() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  // La bobine est alimentee.
  assert(im->postCommand(ACMD_TEST_SOLENOID, 1));
  finRunPasses(im, 1, 10);
  assert(im->getAirflowCtrl().isSolenoidOpen());
  assert(__analog_writes[kFinSolenoidPin] > 0);

  assert(finSaturateRing(im) == COMMAND_QUEUE_SIZE);

  // La FERMETURE retire de l'energie : non perdable.
  assert(im->postCommand(ACMD_TEST_SOLENOID, 0));

  finRunPasses(im, 1, 10);
  assert(!im->getAirflowCtrl().isSolenoidOpen());
  assert(__analog_writes[kFinSolenoidPin] == 0);

  delete im;
}

/*=============================================================================
 * A-1 (suite) - un arret n'attend pas derriere la borne de travail par passe
 *
 * processCommands() n'applique qu'INSTRUMENT_MAX_COMMANDS_PER_UPDATE commandes
 * par passe. Un ordre d'arret place dans l'anneau attendrait donc son tour
 * derriere jusqu'a COMMAND_QUEUE_SIZE - 1 autres commandes. Comme le panic, il
 * doit etre consomme EN TETE de passe.
 *===========================================================================*/

void a_stop_order_does_not_wait_behind_the_per_pass_bound() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 80));
  finRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().isPumpRunning());

  assert(finSaturateRing(im) == COMMAND_QUEUE_SIZE);
  assert(im->postCommand(ACMD_PUMP_STOP));

  // UNE seule passe.
  finRunPasses(im, 1, 10);

  // L'arret a ete applique MAINTENANT...
  assert(!im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kFinPumpPin] == 0);
  // ...alors que la borne a bel et bien joue : l'anneau n'est pas vide, donc
  // l'arret n'a pas attendu que le travail differe s'ecoule.
  assert(im->commandQueue().count() ==
         (uint8_t)(COMMAND_QUEUE_SIZE - INSTRUMENT_MAX_COMMANDS_PER_UPDATE));
  assert(im->commandQueue().count() > 0);

  delete im;
}

// L'arret est consomme EN TETE de passe, avant le drainage de l'anneau : une
// commande d'alimentation emise APRES lui doit donc gagner, meme appliquee dans
// la MEME passe - elle est plus recente, c'est la derniere intention de
// l'operateur. Consommer les arrets a la fin de la passe inverserait cet ordre
// et ferait gagner un arret sur une demande posterieure.
void a_command_posted_after_the_stop_wins_in_the_same_pass() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  // Trois commandes neutres en file : elles tiennent sous la borne, donc toute
  // la file s'ecoule dans la passe qui suit.
  for (int i = 0; i < 3; i++) assert(im->postCommand(ACMD_TEST_FINGER, 0, 0, 90));

  assert(im->postCommand(ACMD_PUMP_STOP));
  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 70));   // POSTERIEUR a l'arret

  finRunPasses(im, 1, 10);
  assert(im->commandQueue().count() == 0);            // tout a bien ete applique
  assert(im->getPressureCtrl().getTargetPercent() == 70);
  assert(im->getPressureCtrl().isPumpRunning());

  delete im;
}

/*=============================================================================
 * A-1 (suite) - dix ordres d'arret = UN arret, jamais zero
 *===========================================================================*/

void repeated_stop_orders_coalesce_into_one() {
  // Niveau file : la coalescence elle-meme, sans le reste de l'instrument.
  CommandQueue q(4);
  for (int i = 0; i < 10; i++) q.requestStop(CommandQueue::STOPREQ_FAN);
  assert(q.stopRequestsPending());
  assert(q.count() == 0);             // aucun emplacement d'anneau consomme
  assert(q.droppedCount() == 0);      // et rien n'est compte comme perdu
  // Une SEULE prise ramasse les dix demandes, et une seule fois.
  assert(q.takeStopRequests() == CommandQueue::STOPREQ_FAN);
  assert(!q.stopRequestsPending());
  assert(q.takeStopRequests() == 0);

  // Niveau instrument : dix ACMD_FAN_STOP d'affilee arretent le ventilateur,
  // sans consommer d'emplacement ni perdre d'ordre.
  finResetCfgFan();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;
  assert(im->postCommand(ACMD_FAN_TARGET, 0, 80));
  finRunPasses(im, 6, 100);
  assert(im->getFanCtrl().isRunning());

  for (int i = 0; i < 10; i++) assert(im->postCommand(ACMD_FAN_STOP));
  assert(im->commandQueue().count() == 0);
  assert(im->commandQueue().droppedCount() == 0);
  assert(im->commandQueue().stopRequestsPending());

  finRunPasses(im, 1, 10);
  assert(!im->commandQueue().stopRequestsPending());   // tout consomme en UNE passe
  assert(!im->getFanCtrl().isRunning());
  assert(__analog_writes[kFinFanPin] == 0);

  delete im;
}

/*=============================================================================
 * A-1 (suite) - une commande d'ALIMENTATION anterieure ne doit pas defaire
 * l'arret
 *
 * REGLE DE SURETE : on ne doit jamais finir alimente parce qu'une commande
 * d'alimentation ANTERIEURE a l'arret a ete appliquee APRES lui. Le panic
 * resout deja ce probleme en VIDANT l'anneau ; un arret cible en retire ce qui
 * realimenterait l'actionneur vise, et rien d'autre.
 *===========================================================================*/

void a_stop_cancels_the_energizing_commands_already_queued() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 80));
  finRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().isPumpRunning());

  // L'anneau est rempli de consignes d'ALIMENTATION (1 % ... 24 %), puis
  // l'arret est demande APRES elles.
  assert(finFillRingWithPumpTargets(im) == COMMAND_QUEUE_SIZE);
  assert(im->postCommand(ACMD_PUMP_STOP));

  // Aucune de ces consignes anterieures ne doit realimenter la pompe.
  finRunPasses(im, 8, 10);
  assert(im->getPressureCtrl().getTargetPercent() == 0);
  assert(!im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kFinPumpPin] == 0);
  assert(im->commandQueue().count() == 0);

  // En revanche une consigne POSTERIEURE a l'arret reste legitime : l'arret ne
  // doit pas devenir un verrou permanent.
  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 70));
  finRunPasses(im, 2, 10);
  assert(im->getPressureCtrl().getTargetPercent() == 70);
  assert(im->getPressureCtrl().isPumpRunning());

  delete im;
}

// Meme regle pour l'arret d'UNE pompe. PressureController::stopSinglePumpTest()
// n'a pas d'index : il arrete LE test en cours, quel qu'il soit. Un test
// mono-pompe deja en file le redemarrerait donc aussitot apres l'arret.
void a_single_pump_stop_cancels_the_pump_test_already_queued() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  assert(im->postCommand(ACMD_PUMP_SINGLE_TEST, 0, 60));
  finRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().isPumpRunning());

  // Une relance est deja en file, puis l'arret est demande APRES elle.
  assert(im->postCommand(ACMD_PUMP_SINGLE_TEST, 0, 60));
  assert(im->commandQueue().count() == 1);
  assert(im->postCommand(ACMD_PUMP_STOP_SINGLE, 0));

  finRunPasses(im, 2, 10);
  assert(!im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kFinPumpPin] == 0);

  delete im;
}

// Et pour les deux autres actionneurs vises par le canal d'arret : ce qui les
// realimente doit partir de l'anneau, rien d'autre.
void a_fan_stop_and_a_valve_close_cancel_what_would_undo_them() {
  // --- ventilateur ---
  finResetCfgFan();
  InstrumentManager* fanIm = finMakeReadyInstrument();
  __test_millis = 1000;
  assert(fanIm->postCommand(ACMD_FAN_TARGET, 0, 80));
  finRunPasses(fanIm, 6, 100);
  assert(fanIm->getFanCtrl().isRunning());

  assert(fanIm->postCommand(ACMD_FAN_TARGET, 0, 90));   // realimente
  assert(fanIm->postCommand(ACMD_TEST_FINGER, 0, 0, 90));  // neutre : doit RESTER
  assert(fanIm->commandQueue().count() == 2);
  assert(fanIm->postCommand(ACMD_FAN_STOP));
  assert(fanIm->commandQueue().count() == 1);           // seule la neutre survit

  finRunPasses(fanIm, 6, 100);
  assert(!fanIm->getFanCtrl().isRunning());
  assert(fanIm->getFanCtrl().getSpeed() == 0);
  assert(__analog_writes[kFinFanPin] == 0);
  delete fanIm;

  // --- valve ---
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;
  assert(im->postCommand(ACMD_TEST_SOLENOID, 1));
  finRunPasses(im, 1, 10);
  assert(im->getAirflowCtrl().isSolenoidOpen());

  assert(im->postCommand(ACMD_TEST_SOLENOID, 1));       // rouvrirait
  assert(im->commandQueue().count() == 1);
  assert(im->postCommand(ACMD_TEST_SOLENOID, 0));
  assert(im->commandQueue().count() == 0);

  finRunPasses(im, 2, 10);
  assert(!im->getAirflowCtrl().isSolenoidOpen());
  assert(__analog_writes[kFinSolenoidPin] == 0);

  delete im;
}

/*=============================================================================
 * A-1 (suite) - l'asymetrie du contrat : seule la variante qui RETIRE de
 * l'energie quitte l'anneau
 *===========================================================================*/

void only_the_de_energizing_variants_leave_the_ring() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  // pump_enable = true et test_sol = 1 AJOUTENT de l'energie : anneau ordinaire.
  assert(im->commandQueue().count() == 0);
  assert(im->postCommand(ACMD_PUMP_ENABLE, 1));
  assert(im->commandQueue().count() == 1);
  assert(im->postCommand(ACMD_TEST_SOLENOID, 1));
  assert(im->commandQueue().count() == 2);
  assert(!im->commandQueue().stopRequestsPending());

  // Sous saturation, ces variantes sont refusees comme n'importe quelle
  // commande ordinaire - perdre une ouverture est SUR.
  while (im->postCommand(ACMD_TEST_FINGER, 0, 0, 90)) { }
  assert(!im->postCommand(ACMD_PUMP_ENABLE, 1));
  assert(!im->postCommand(ACMD_TEST_SOLENOID, 1));

  // Leurs variantes d'arret, elles, passent toujours.
  assert(im->postCommand(ACMD_PUMP_ENABLE, 0));
  assert(im->postCommand(ACMD_TEST_SOLENOID, 0));
  assert(im->commandQueue().stopRequestsPending());

  delete im;
}

/*=============================================================================
 * A-2 - l'ordre Note On / Note Off doit etre respecte
 *
 * LE DEFAUT : un Note On passe par l'anneau FIFO, un Note Off par un bitmap
 * separe applique APRES l'anneau. Un utilisateur qui relache puis re-appuie
 * entre deux tours de loop() voyait donc l'ordre s'inverser - Note On puis Note
 * Off - et la note finissait MUETTE alors qu'il l'avait demandee sonnante.
 *===========================================================================*/

void a_note_off_followed_by_a_note_on_leaves_the_note_sounding() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;
  assert(im->getSequencer().getState() == STATE_IDLE);

  // Les DEUX ordres sont emis avant le prochain tour de loop().
  assert(im->postCommand(ACMD_NOTE_OFF, 60));
  assert(im->postCommand(ACMD_NOTE_ON, 60, 100));

  finRunPasses(im, 10, 20);

  // L'ordre d'EMISSION doit etre respecte : la note SONNE.
  assert(im->getSequencer().getState() == STATE_PLAYING);
  assert(im->getAirflowCtrl().isValveOpen());

  delete im;
}

void a_note_on_followed_by_a_note_off_releases_the_note() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  // La propriete SYMETRIQUE, celle qui ne doit pas regresser : dans cet
  // ordre-la, la note doit bien finir relachee.
  assert(im->postCommand(ACMD_NOTE_ON, 60, 100));
  assert(im->postCommand(ACMD_NOTE_OFF, 60));

  finRunPasses(im, 12, 20);

  assert(!im->getAirflowCtrl().isValveOpen());
  assert(im->getSequencer().getState() == STATE_IDLE);

  delete im;
}

void a_note_off_still_falls_back_to_the_bitmap_under_saturation() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  // Une note joue reellement.
  im->noteOn(60, 100);
  finRunPasses(im, 4, 20);
  assert(im->getSequencer().getState() == STATE_PLAYING);
  assert(im->getAirflowCtrl().isValveOpen());

  // Anneau plein : le Note Off ne peut PLUS y entrer.
  assert(finSaturateRing(im) == COMMAND_QUEUE_SIZE);
  assert(im->postCommand(ACMD_NOTE_OFF, 60));

  // Il bascule sur le canal non perdable, et ce repli n'est pas une PERTE :
  // le compteur de commandes perdues ne doit pas bouger.
  assert(im->commandQueue().hasPendingNoteOff());
  assert(im->commandQueue().droppedCount() == 0);

  // Et il eteint reellement la note.
  finRunPasses(im, 24, 20);
  assert(!im->commandQueue().hasPendingNoteOff());
  assert(!im->getAirflowCtrl().isValveOpen());
  assert(im->getSequencer().getState() == STATE_IDLE);
  assert(im->commandQueue().droppedCount() == 0);

  delete im;
}

/*=============================================================================
 * A-3 - configChangeRequiresRestart() doit couvrir TOUTE la topologie
 *
 * LE DEFAUT : la liste des champs qui imposent un redemarrage etait ecrite a la
 * main et il en manquait. Les pinMode() du firmware utilisent aussi
 * cfg.endstopPin, cfg.endstopActiveHigh (PressureController.cpp:73) et
 * cfg.hallPin (PressureController.cpp:95) : aucun des trois n'etait teste. Une
 * broche de fin de course changee a chaud laissait l'ancienne configuree en
 * entree et la nouvelle jamais initialisee.
 *
 * La correction est un simple RELAIS vers configTopologyRequiresRestart()
 * (LOT B), qui porte la table exhaustive. Ce test verrouille le contrat rendu
 * par le relais, pas son implementation.
 *===========================================================================*/

void hardware_topology_changes_require_a_restart() {
  RuntimeConfig* base = new RuntimeConfig();
  RuntimeConfig* changed = new RuntimeConfig();
  memset(base, 0, sizeof(RuntimeConfig));
  base->numFingers = 1;
  base->numPumps = 1;
  base->airMode = AIR_MODE_PUMP_RESERVOIR;
  base->sensorType = SENSOR_TYPE_HALL_KY024;
  base->solenoidPin = 13;
  base->fanPin = 26;
  base->pumpPins[0] = 25;
  base->endstopPin = 27;
  base->endstopActiveHigh = true;
  base->hallPin = 36;

  // Reference : une configuration identique n'impose rien.
  *changed = *base;
  assert(!InstrumentManager::configChangeRequiresRestart(*base, *changed));

  // LES TROIS MANQUANTS.
  *changed = *base; changed->endstopPin = 33;
  assert(InstrumentManager::configChangeRequiresRestart(*base, *changed));

  *changed = *base; changed->endstopActiveHigh = !base->endstopActiveHigh;
  assert(InstrumentManager::configChangeRequiresRestart(*base, *changed));

  *changed = *base; changed->hallPin = 39;
  assert(InstrumentManager::configChangeRequiresRestart(*base, *changed));

  // Ce qui etait deja couvert doit le rester : le relais ne doit rien perdre.
  *changed = *base; changed->solenoidPin = 14;
  assert(InstrumentManager::configChangeRequiresRestart(*base, *changed));

  *changed = *base; changed->fanPin = 27;
  assert(InstrumentManager::configChangeRequiresRestart(*base, *changed));

  *changed = *base; changed->pumpPins[0] = 32;
  assert(InstrumentManager::configChangeRequiresRestart(*base, *changed));

  *changed = *base; changed->numFingers = 3;
  assert(InstrumentManager::configChangeRequiresRestart(*base, *changed));

  *changed = *base; changed->airMode = AIR_MODE_FAN_SERVO;
  assert(InstrumentManager::configChangeRequiresRestart(*base, *changed));

  delete base;
  delete changed;
}

/*=============================================================================
 * A-6 - une consigne a ZERO retire de l'energie, donc elle ne doit pas non plus
 *       etre perdable
 *
 * LE DEFAUT, tel qu'il existe apres le LOT A :
 *   `postCommand()` sort de l'anneau les ordres d'ARRET (pump_stop, fan_stop,
 *   pump_enable=false, test_sol=0, pump_stop_single). Mais l'interface web ne
 *   passe pas par eux pour amener un actionneur a zero : les curseurs envoient
 *   "pump_target" / "fan_target" avec v = 0, qui finissent sur le
 *   `return _commands.push(cmd)` ordinaire. Anneau plein = la consigne a zero
 *   est JETEE, et l'actionneur reste a sa consigne precedente.
 *
 *   La borne INSTRUMENT_MAX_COMMANDS_PER_UPDATE introduite par cette meme passe
 *   rend la saturation PLUS atteignable qu'avant : 24 emplacements s'ecoulent
 *   desormais par tranches de 6. Une rafale de notes BLE et un curseur ramene a
 *   zero suffisent.
 *
 * POURQUOI PAS SIMPLEMENT LES ROUTER VERS STOPREQ_PUMPS / STOPREQ_FAN :
 *   parce que `stop()` n'est PAS `setTargetPercent(0)`.
 *   PressureController::stop() annule en plus le test mono-pompe en cours et
 *   ecrase le PWM immediatement ; FanController::stop() saute la rampe de
 *   descente. Router la consigne a zero vers l'arret dur changerait le
 *   comportement observable de l'interface. Le canal ajoute applique donc
 *   exactement la meme commande qu'avant - ACMD_PUMP_TARGET / ACMD_FAN_TARGET
 *   avec b = 0 - par le meme applyCommand(), en la rendant seulement
 *   imperdable. Le test A-6d verrouille cette distinction.
 *===========================================================================*/

void a_zero_pump_target_is_not_lost_when_the_ring_is_full() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  // La pompe tourne REELLEMENT.
  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 80));
  finRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().getTargetPercent() == 80);
  assert(im->getPressureCtrl().isPumpRunning());

  // Anneau sature de commandes NEUTRES pour la source d'air.
  assert(finSaturateRing(im) == COMMAND_QUEUE_SIZE);
  assert(!im->postCommand(ACMD_TEST_FINGER, 0, 0, 90));   // il est bien plein

  // L'operateur ramene le curseur a zero.
  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 0));

  // Applique des la passe COURANTE, sans attendre que les 24 commandes neutres
  // se soient ecoulees par tranches de six.
  finRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().getTargetPercent() == 0);
  assert(!im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kFinPumpPin] == 0);

  // UNE SEULE perte comptee : la sonde ci-dessus, qui sert justement a prouver
  // que l'anneau etait plein. La consigne a zero, elle, n'y figure pas - c'est
  // exactement ce que ce test verrouille.
  assert(im->droppedCommandCount() == 1);

  delete im;
}

void a_zero_fan_target_is_not_lost_when_the_ring_is_full() {
  finResetCfgFan();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  assert(im->postCommand(ACMD_FAN_TARGET, 0, 80));
  finRunPasses(im, 4, 200);          // laisser la rampe monter
  assert(im->getFanCtrl().getSpeed() == 80);
  assert(im->getFanCtrl().isRunning());

  assert(finSaturateRing(im) == COMMAND_QUEUE_SIZE);
  assert(!im->postCommand(ACMD_TEST_FINGER, 0, 0, 90));

  assert(im->postCommand(ACMD_FAN_TARGET, 0, 0));

  finRunPasses(im, 4, 200);          // laisser la rampe redescendre
  assert(im->getFanCtrl().getSpeed() == 0);
  assert(!im->getFanCtrl().isRunning());
  assert(__analog_writes[kFinFanPin] == 0);
  assert(im->droppedCommandCount() == 1);   // la sonde seule, comme ci-dessus

  delete im;
}

// Meme raisonnement que pour un arret : une consigne NON NULLE deja en file ne
// doit pas realimenter l'actionneur APRES la consigne a zero.
void a_zero_target_cancels_the_higher_targets_already_queued() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  // Anneau rempli de consignes croissantes : sans purge, la derniere appliquee
  // serait COMMAND_QUEUE_SIZE %.
  assert(finFillRingWithPumpTargets(im) == COMMAND_QUEUE_SIZE);
  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 0));

  finRunPasses(im, 8, 10);           // largement de quoi vider l'anneau
  assert(im->commandQueue().count() == 0);
  assert(im->getPressureCtrl().getTargetPercent() == 0);
  assert(!im->getPressureCtrl().isPumpRunning());

  delete im;
}

// CONTRE-EPREUVE DE COMPORTEMENT : une consigne a zero n'est pas un arret dur.
// `stop()` annule le test mono-pompe ; `setTargetPercent(0)` ne le fait pas, et
// ne doit pas commencer a le faire parce que la commande a change de canal.
void a_zero_pump_target_does_not_end_a_single_pump_test() {
  finResetCfg();
  InstrumentManager* im = finMakeReadyInstrument();
  __test_millis = 1000;

  // PREMIER VOLET - le test mono-pompe est encore DANS L'ANNEAU quand la
  // consigne a zero arrive. La purge ne doit pas l'emporter : elle ne retire
  // que ce qui realimenterait la CIBLE GLOBALE.
  assert(im->postCommand(ACMD_PUMP_SINGLE_TEST, 0, 60));
  assert(im->commandQueue().count() == 1);
  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 0));
  assert(im->commandQueue().count() == 1);   // toujours la, non purge
  finRunPasses(im, 1, 10);
  const int duringTest = __analog_writes[kFinPumpPin];
  assert(duringTest > 0);                    // le test demande a bien demarre

  // SECOND VOLET - consigne globale a zero pendant que le test mono-pompe est
  // DEJA EN COURS.
  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 0));
  finRunPasses(im, 1, 10);

  // La cible globale est bien a zero...
  assert(im->getPressureCtrl().getTargetPercent() == 0);
  // ...mais le test mono-pompe possede toujours la pompe : update() sort par sa
  // branche prioritaire et continue d'ecrire le PWM du test.
  assert(__analog_writes[kFinPumpPin] == duringTest);

  // L'arret DUR, lui, y met fin - c'est la difference que ce test protege.
  assert(im->postCommand(ACMD_PUMP_STOP_SINGLE, 0));
  finRunPasses(im, 1, 10);
  assert(__analog_writes[kFinPumpPin] == 0);

  delete im;
}

}  // namespace

void fin_actuators_run_all_tests() {
  pump_stop_reaches_the_pump_even_when_the_ring_is_full();
  fan_stop_reaches_the_fan_even_when_the_ring_is_full();
  single_pump_stop_reaches_the_pump_even_when_the_ring_is_full();
  pump_disable_reaches_the_pump_even_when_the_ring_is_full();
  solenoid_close_reaches_the_valve_even_when_the_ring_is_full();
  a_stop_order_does_not_wait_behind_the_per_pass_bound();
  a_command_posted_after_the_stop_wins_in_the_same_pass();
  repeated_stop_orders_coalesce_into_one();
  a_stop_cancels_the_energizing_commands_already_queued();
  a_single_pump_stop_cancels_the_pump_test_already_queued();
  a_fan_stop_and_a_valve_close_cancel_what_would_undo_them();
  only_the_de_energizing_variants_leave_the_ring();
  a_note_off_followed_by_a_note_on_leaves_the_note_sounding();
  a_note_on_followed_by_a_note_off_releases_the_note();
  a_note_off_still_falls_back_to_the_bitmap_under_saturation();
  hardware_topology_changes_require_a_restart();
  a_zero_pump_target_is_not_lost_when_the_ring_is_full();
  a_zero_fan_target_is_not_lost_when_the_ring_is_full();
  a_zero_target_cancels_the_higher_targets_already_queued();
  a_zero_pump_target_does_not_end_a_single_pump_test();
}
