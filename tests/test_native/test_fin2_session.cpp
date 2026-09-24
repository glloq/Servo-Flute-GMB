// Finalisation 2 - LOT 1 : pendant une session d'auto-calibration, aucune
// commande exterieure ne doit pouvoir toucher un actionneur - et toute commande
// de SECURITE doit continuer de passer.
//
// CE QUE CES TESTS PROUVENT
// -------------------------
// La prise de possession (`setActuatorSessionActive(true)`) arretait le
// sequenceur et vidait la file d'EVENEMENTS MIDI, mais laissait intacte la file
// de COMMANDES d'actionneurs. Une commande web deposee juste avant le depart de
// la calibration - un angle de doigt, une consigne de pompe, l'ouverture de la
// valve - etait donc appliquee PENDANT la mesure, sur un instrument que le
// calibrateur croit posseder seul. Sur un vrai banc, cela fausse la mesure sans
// que rien ne le signale.
//
// Deux barrieres, volontairement redondantes :
//   1. la PURGE a la transition : ce qui attendait avant la prise de possession
//      n'est jamais applique ;
//   2. la GARDE CENTRALE dans applyCommand(), qui vit sur la tache proprietaire
//      des actionneurs : meme une commande deposee APRES le depart ne bouge
//      rien. C'est elle la barriere physique finale ; le filtrage de
//      WebConfigurator (`calibration_active`) n'est qu'un confort d'interface
//      execute sur la tache AsyncTCP.
//
// CE QUI NE DOIT SURTOUT PAS ETRE BLOQUE : les ordres qui RETIRENT de
// l'energie. Une garde naive du type `_actuatorSessionActive &&
// commandDrivesActuators(type)` bloquerait `ACMD_PUMP_TARGET` avec b == 0, qui
// est precisement le canal imperdable par lequel l'interface coupe une pompe.
// D'ou un predicat distinct et teste : commandMayEnergizeActuator().
//
// L'etat verifie est celui de l'ACTIONNEUR (PWM reellement ecrit sur la broche),
// pas la valeur rendue par postCommand().
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

const uint8_t kSesPumpPin = 25;
const uint8_t kSesSolenoidPin = 13;
const uint8_t kSesFanPin = 26;

void sesCommonCfg() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.numFingers = 1;
  cfg.fingers[0].pcaChannel = 0;
  cfg.fingers[0].closedAngle = 90;
  cfg.fingers[0].direction = 1;
  cfg.airflowPcaChannel = 10;
  cfg.solenoidPin = kSesSolenoidPin;
  cfg.solenoidActivationTimeMs = 50;
  cfg.solenoidPwmActivation = 255;
  cfg.solenoidPwmHolding = 128;
  cfg.servoToSolenoidDelayMs = 10;
  cfg.minNoteDurationMs = 0;
  cfg.minNoteIntervalForValveCloseMs = 0;
  cfg.timeUnpower = 5000;
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

// Pompe directe + valve a solenoide : les deux sont observables sur une broche.
void sesResetCfg() {
  sesCommonCfg();
  cfg.airMode = AIR_MODE_PUMP_VALVE;
  cfg.valveType = 0;
  cfg.numPumps = 1;
  cfg.pumpPins[0] = kSesPumpPin;
  cfg.pumpMinPwm[0] = 80;
  cfg.pumpMaxPwm[0] = 200;
  cfg.motorType = MOTOR_TYPE_PWM;
  cfg.pumpFollowAirflow = true;
  cfg.pumpDirectMaxPercent = 100;
  cfg.pumpDirectIdlePercent = 0;
}

void sesResetCfgFan() {
  sesCommonCfg();
  cfg.airMode = AIR_MODE_FAN_SERVO;
  cfg.fanPin = kSesFanPin;
  cfg.fanMinPwm = 60;
  cfg.fanMaxPwm = 240;
  cfg.fanIdlePercent = 0;
  cfg.fanIdleTimeoutMs = 0;
  cfg.fanDefaultPercent = 0;
  cfg.fanMaxNotePercent = 100;
  cfg.fanFollowAirflow = true;
}

InstrumentManager* sesMakeReadyInstrument() {
  Wire.clear();
  Wire.setPresent(PCA_ADDR_BOARD0, true);
  InstrumentManager* im = new InstrumentManager();
  assert(im->beginSafe());
  assert(im->isHardwareReady());
  return im;
}

void sesRunPasses(InstrumentManager* im, int passes, unsigned long stepMs) {
  for (int i = 0; i < passes; i++) {
    __test_millis += stepMs;
    im->update();
  }
}

bool sesSolenoidEnergised() {
  // Valve GPIO : active HIGH ou LOW selon SOLENOID_ACTIVE_HIGH. On observe la
  // presence d'une ecriture PWM non nulle, qui est ce que testSolenoid(true)
  // produit (activation puis maintien).
  return __analog_writes[kSesSolenoidPin] > 0;
}

/*=============================================================================
 * 1-A - la purge a la transition
 *
 * LE DEFAUT : setActuatorSessionActive(true) appelait _eventQueue.clear() mais
 * jamais _commands.clear(). Une commande deposee par la tache AsyncTCP et pas
 * encore consommee par update() survivait donc a la prise de possession.
 *===========================================================================*/

void a_command_queued_before_the_session_never_reaches_the_actuators() {
  sesResetCfg();
  InstrumentManager* im = sesMakeReadyInstrument();
  __test_millis = 1000;

  // Des commandes d'actionneurs attendent dans la file, PAS encore appliquees.
  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 80));
  assert(im->postCommand(ACMD_TEST_SOLENOID, 1));
  assert(im->postCommand(ACMD_TEST_FINGER, 0, 0, 40));
  assert(im->commandQueue().count() == 3);

  // La calibration prend possession AVANT le prochain update().
  im->setActuatorSessionActive(true);

  // EFFET PROPRE A LA PURGE : l'anneau est vide DES la transition, sans
  // attendre qu'update() le draine par tranches de
  // INSTRUMENT_MAX_COMMANDS_PER_UPDATE. La garde centrale, elle, ne ferait que
  // refuser ces commandes une a une au fil des passes - meme resultat sur
  // l'actionneur, mais l'anneau resterait plein en attendant.
  assert(im->commandQueue().count() == 0);

  // Rien de tout cela ne doit jamais s'appliquer.
  sesRunPasses(im, 4, 10);
  assert(im->getPressureCtrl().getTargetPercent() == 0);
  assert(!im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kSesPumpPin] == 0);
  assert(!sesSolenoidEnergised());

  delete im;
}

// Meme scenario cote ventilateur : seul AIR_MODE_FAN_SERVO fait tourner
// FanController::update(), donc seul lui rend le PWM observable.
void a_fan_command_queued_before_the_session_never_reaches_the_fan() {
  sesResetCfgFan();
  InstrumentManager* im = sesMakeReadyInstrument();
  __test_millis = 1000;

  assert(im->postCommand(ACMD_FAN_TARGET, 0, 80));
  assert(im->commandQueue().count() == 1);

  im->setActuatorSessionActive(true);

  sesRunPasses(im, 6, 200);   // large : la rampe aurait eu le temps de monter
  assert(im->getFanCtrl().getSpeed() == 0);
  assert(!im->getFanCtrl().isRunning());
  assert(__analog_writes[kSesFanPin] == 0);

  delete im;
}

/*=============================================================================
 * 1-B - la garde centrale
 *
 * La purge ne couvre que ce qui etait DEJA en file. Une commande deposee par
 * AsyncTCP pendant la mesure doit elle aussi rester sans effet, et cette
 * decision-la doit vivre sur la tache proprietaire des actionneurs.
 *===========================================================================*/

void a_command_posted_during_the_session_moves_nothing() {
  sesResetCfg();
  InstrumentManager* im = sesMakeReadyInstrument();
  __test_millis = 1000;

  im->setActuatorSessionActive(true);
  sesRunPasses(im, 1, 10);

  // Tout ce qu'un client web peut demander pendant la mesure.
  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 90));
  assert(im->postCommand(ACMD_PUMP_SINGLE_TEST, 0, 90));
  assert(im->postCommand(ACMD_TEST_SOLENOID, 1));
  assert(im->postCommand(ACMD_TEST_FINGER, 0, 0, 40));
  assert(im->postCommand(ACMD_OPEN_ALL_FINGERS));
  assert(im->postCommand(ACMD_AIR_LIVE_PERCENT, 0, 100));
  sesRunPasses(im, 6, 10);

  assert(im->getPressureCtrl().getTargetPercent() == 0);
  assert(!im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kSesPumpPin] == 0);
  assert(!sesSolenoidEnergised());
  // La file a bien ete DRAINEE (les commandes sont refusees a l'application,
  // pas laissees a s'accumuler jusqu'a saturation).
  assert(im->commandQueue().count() == 0);

  delete im;
}

// Le MIDI etait deja ignore pendant une session (noteOn/noteOff/handleControlChange
// sortent tot). Ce test le verrouille depuis la FILE, qui est le chemin qu'emprunte
// reellement un Note On venu de BLE ou de rtpMIDI.
void midi_posted_during_the_session_moves_nothing() {
  sesResetCfg();
  InstrumentManager* im = sesMakeReadyInstrument();
  __test_millis = 1000;

  im->setActuatorSessionActive(true);
  sesRunPasses(im, 1, 10);

  assert(im->postCommand(ACMD_NOTE_ON, 60, 100));
  assert(im->postCommand(ACMD_CONTROL_CHANGE, 2, 127));
  sesRunPasses(im, 4, 10);

  assert(!sesSolenoidEnergised());
  assert(!im->getPressureCtrl().isPumpRunning());

  delete im;
}

/*=============================================================================
 * 1-C - ce que la garde ne doit JAMAIS bloquer
 *
 * C'est le piege de cette correction : la garde doit distinguer "ajoute de
 * l'energie" de "en retire". Une consigne a zero, un arret, un panic sont des
 * ordres de SECURITE et doivent traverser la session.
 *===========================================================================*/

void safety_orders_still_reach_the_actuators_during_the_session() {
  sesResetCfg();
  InstrumentManager* im = sesMakeReadyInstrument();
  __test_millis = 1000;

  im->setActuatorSessionActive(true);
  sesRunPasses(im, 1, 10);

  // Le CALIBRATEUR alimente la pompe par le chemin DIRECT (il possede des
  // references sur les controleurs et n'emprunte jamais la file).
  im->getPressureCtrl().setTargetPercent(80);
  sesRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kSesPumpPin] > 0);

  // UN ARRET VENU DE L'EXTERIEUR DOIT PASSER.
  assert(im->postCommand(ACMD_PUMP_STOP));
  sesRunPasses(im, 1, 10);
  assert(!im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kSesPumpPin] == 0);

  delete im;
}

// Le cas exact contre lequel une garde naive echouerait : `pump_target v=0`
// est ce que les CURSEURS de l'interface envoient pour couper une pompe, et il
// emprunte le canal imperdable. Le bloquer parce qu'il "drives actuators"
// rendrait la coupure impossible pendant une calibration.
void a_zero_setpoint_is_a_safety_order_and_is_never_blocked() {
  sesResetCfg();
  InstrumentManager* im = sesMakeReadyInstrument();
  __test_millis = 1000;

  im->setActuatorSessionActive(true);
  sesRunPasses(im, 1, 10);

  im->getPressureCtrl().setTargetPercent(80);
  sesRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().isPumpRunning());

  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 0));
  sesRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().getTargetPercent() == 0);
  assert(!im->getPressureCtrl().isPumpRunning());

  delete im;
}

void a_zero_fan_setpoint_is_never_blocked_either() {
  sesResetCfgFan();
  InstrumentManager* im = sesMakeReadyInstrument();
  __test_millis = 1000;

  im->setActuatorSessionActive(true);
  im->getFanCtrl().setSpeed(80);
  sesRunPasses(im, 4, 200);
  assert(im->getFanCtrl().isRunning());

  assert(im->postCommand(ACMD_FAN_TARGET, 0, 0));
  sesRunPasses(im, 4, 200);
  assert(im->getFanCtrl().getSpeed() == 0);
  assert(!im->getFanCtrl().isRunning());

  delete im;
}

void the_valve_can_still_be_closed_during_the_session() {
  sesResetCfg();
  InstrumentManager* im = sesMakeReadyInstrument();
  __test_millis = 1000;

  im->setActuatorSessionActive(true);
  // Le calibrateur ouvre la valve par le chemin direct.
  im->getAirflowCtrl().testSolenoid(true);
  sesRunPasses(im, 1, 10);
  assert(sesSolenoidEnergised());

  // La fermeture venue de l'exterieur doit passer.
  assert(im->postCommand(ACMD_TEST_SOLENOID, 0));
  sesRunPasses(im, 1, 10);
  assert(!sesSolenoidEnergised());

  delete im;
}

void a_panic_still_works_during_the_session() {
  sesResetCfg();
  InstrumentManager* im = sesMakeReadyInstrument();
  __test_millis = 1000;

  im->setActuatorSessionActive(true);
  im->getPressureCtrl().setTargetPercent(80);
  im->getAirflowCtrl().testSolenoid(true);
  sesRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().isPumpRunning());
  assert(sesSolenoidEnergised());

  const uint32_t before = im->panicCount();
  im->requestPanic();
  sesRunPasses(im, 1, 10);
  assert(im->panicCount() == before + 1);
  assert(!im->getPressureCtrl().isPumpRunning());
  assert(!sesSolenoidEnergised());
  assert(__analog_writes[kSesPumpPin] == 0);

  delete im;
}

/*=============================================================================
 * 1-D - le predicat lui-meme, classe par classe
 *
 * Les 21 valeurs de ActuatorCommandType sont passees en revue. Ce n'est pas une
 * redite des tests ci-dessus : ceux-la prouvent l'EFFET sur quelques commandes,
 * celui-ci verrouille la CLASSIFICATION de toutes - y compris celles dont
 * l'effet n'est pas observable sur une broche.
 *===========================================================================*/

void every_command_type_is_classified() {
  // Ajoutent de l'energie ou deplacent un actionneur.
  const ActuatorCommand energising[] = {
    ActuatorCommand(ACMD_NOTE_ON, 60, 100),
    ActuatorCommand(ACMD_CONTROL_CHANGE, 2, 127),
    ActuatorCommand(ACMD_RESET_CONTROLLERS),
    ActuatorCommand(ACMD_TEST_FINGER, 0, 0, 40),
    ActuatorCommand(ACMD_TEST_AIRFLOW_ANGLE, 0, 0, 80),
    ActuatorCommand(ACMD_TEST_ANGLE_SERVO, 0, 0, 80),
    ActuatorCommand(ACMD_AIR_LIVE_PERCENT, 0, 100),
    ActuatorCommand(ACMD_ANGLE_LIVE_PERCENT, 0, 100),
    ActuatorCommand(ACMD_TEST_SOLENOID, 1),
    ActuatorCommand(ACMD_PUMP_TARGET, 0, 1),
    ActuatorCommand(ACMD_PUMP_ENABLE, 1),
    ActuatorCommand(ACMD_FAN_TARGET, 0, 1),
    ActuatorCommand(ACMD_OPEN_ALL_FINGERS),
    // Un test mono-pompe SAISIT le controleur de pression (il court-circuite la
    // regulation dans update()), meme a 0 % : il prend une possession que la
    // session refuse, d'ou sa classification cote energie quelle que soit b.
    ActuatorCommand(ACMD_PUMP_SINGLE_TEST, 0, 60),
    ActuatorCommand(ACMD_PUMP_SINGLE_TEST, 0, 0),
  };
  for (const ActuatorCommand& c : energising) {
    assert(InstrumentManager::commandMayEnergizeActuator(c));
  }

  // Ne peuvent que RETIRER de l'energie, ou ne touchent aucun actionneur.
  const ActuatorCommand safe[] = {
    ActuatorCommand(ACMD_NONE),
    ActuatorCommand(ACMD_NOTE_OFF, 60),
    ActuatorCommand(ACMD_ALL_SOUND_OFF),
    ActuatorCommand(ACMD_PUMP_STOP),
    ActuatorCommand(ACMD_PUMP_STOP_SINGLE, 0),
    ActuatorCommand(ACMD_FAN_STOP),
    ActuatorCommand(ACMD_SET_ACTUATOR_SESSION, 0),
    ActuatorCommand(ACMD_TEST_SOLENOID, 0),
    ActuatorCommand(ACMD_PUMP_ENABLE, 0),
    ActuatorCommand(ACMD_PUMP_TARGET, 0, 0),
    ActuatorCommand(ACMD_FAN_TARGET, 0, 0),
  };
  for (const ActuatorCommand& c : safe) {
    assert(!InstrumentManager::commandMayEnergizeActuator(c));
  }

  // LE PREDICAT N'EST PAS commandDrivesActuators(). La confusion des deux est
  // exactement le defaut contre lequel ce test existe : une consigne a zero
  // "drives actuators" (elle ecrit un PWM) tout en ne pouvant qu'en RETIRER.
  assert(InstrumentManager::commandDrivesActuators(ACMD_PUMP_TARGET));
  assert(!InstrumentManager::commandMayEnergizeActuator(ActuatorCommand(ACMD_PUMP_TARGET, 0, 0)));
}

// Hors session, la garde ne doit rien changer : une commande energisante
// s'applique normalement. Sans cette contre-epreuve, une garde qui bloquerait
// TOUT passerait les tests ci-dessus.
void outside_a_session_nothing_is_blocked() {
  sesResetCfg();
  InstrumentManager* im = sesMakeReadyInstrument();
  __test_millis = 1000;

  assert(!im->isActuatorSessionActive());
  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 80));
  sesRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().getTargetPercent() == 80);
  assert(im->getPressureCtrl().isPumpRunning());

  delete im;
}

// La session relachee, l'instrument redevient pilotable immediatement.
void releasing_the_session_restores_normal_commands() {
  sesResetCfg();
  InstrumentManager* im = sesMakeReadyInstrument();
  __test_millis = 1000;

  im->setActuatorSessionActive(true);
  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 80));
  sesRunPasses(im, 2, 10);
  assert(!im->getPressureCtrl().isPumpRunning());

  im->setActuatorSessionActive(false);
  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 80));
  sesRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().getTargetPercent() == 80);
  assert(im->getPressureCtrl().isPumpRunning());

  delete im;
}

// CE QUE LA GARDE CENTRALE NE COUVRE PAS, et qui justifie la purge a elle
// seule : une session prise puis RELACHEE sans qu'update() se soit intercale.
// L'anneau n'a alors jamais ete draine pendant la session, donc la garde n'a
// jamais eu l'occasion de refuser quoi que ce soit - et les commandes d'AVANT
// la calibration s'appliqueraient APRES elle.
//
// Ce n'est pas un montage de laboratoire : serviceWsOps() execute plusieurs
// operations web dans la MEME passe de loop(). Un "auto_cal start" suivi d'un
// "auto_cal stop" dans la meme salve produit exactement cette sequence.
void commands_queued_before_the_session_are_gone_even_if_it_is_released_at_once() {
  sesResetCfg();
  InstrumentManager* im = sesMakeReadyInstrument();
  __test_millis = 1000;

  assert(im->postCommand(ACMD_PUMP_TARGET, 0, 80));
  assert(im->postCommand(ACMD_TEST_SOLENOID, 1));
  assert(im->commandQueue().count() == 2);

  // Prise PUIS relache, sans un seul update() entre les deux.
  im->setActuatorSessionActive(true);
  im->setActuatorSessionActive(false);
  assert(!im->isActuatorSessionActive());

  sesRunPasses(im, 4, 10);
  assert(im->getPressureCtrl().getTargetPercent() == 0);
  assert(!im->getPressureCtrl().isPumpRunning());
  assert(__analog_writes[kSesPumpPin] == 0);
  assert(!sesSolenoidEnergised());

  delete im;
}

// La purge ne doit pas emporter un ordre d'ARRET depose avant la prise de
// possession : il vit hors de l'anneau (drapeau dedie) et doit survivre.
void the_purge_keeps_a_stop_order_posted_before_the_session() {
  sesResetCfg();
  InstrumentManager* im = sesMakeReadyInstrument();
  __test_millis = 1000;

  // Pompe alimentee par le chemin direct, comme le ferait le calibrateur.
  im->getPressureCtrl().setTargetPercent(80);
  sesRunPasses(im, 1, 10);
  assert(im->getPressureCtrl().isPumpRunning());

  // Arret depose AVANT la prise de possession, pas encore consomme.
  assert(im->postCommand(ACMD_PUMP_STOP));
  assert(im->commandQueue().stopRequestsPending());

  im->setActuatorSessionActive(true);
  // La purge a vide l'anneau ordinaire mais PAS le canal d'arret.
  assert(im->commandQueue().stopRequestsPending());

  sesRunPasses(im, 1, 10);
  assert(!im->getPressureCtrl().isPumpRunning());

  delete im;
}

}  // namespace

void fin2_session_run_all_tests() {
  a_command_queued_before_the_session_never_reaches_the_actuators();
  a_fan_command_queued_before_the_session_never_reaches_the_fan();
  a_command_posted_during_the_session_moves_nothing();
  midi_posted_during_the_session_moves_nothing();
  safety_orders_still_reach_the_actuators_during_the_session();
  a_zero_setpoint_is_a_safety_order_and_is_never_blocked();
  a_zero_fan_setpoint_is_never_blocked_either();
  the_valve_can_still_be_closed_during_the_session();
  a_panic_still_works_during_the_session();
  every_command_type_is_classified();
  outside_a_session_nothing_is_blocked();
  releasing_the_session_restores_normal_commands();
  commands_queued_before_the_session_are_gone_even_if_it_is_released_at_once();
  the_purge_keeps_a_stop_order_posted_before_the_session();
}
