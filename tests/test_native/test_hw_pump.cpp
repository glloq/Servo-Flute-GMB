// Securite materielle de la pompe : ce fichier verrouille le defaut le plus
// destructeur de l'audit - celui qui n'a besoin de personne pour casser du
// materiel, juste d'une mise sous tension.
//
// Le defaut, tel qu'il a ete reproduit en executant le code de production :
//   PressureController::begin() declarait le capteur present sans RIEN sonder
//   (`pinMode(...); _sensorDetected = true;`), et les branches Hall et fin de
//   course rendaient la main AVANT les gardes de securite, qui n'existaient que
//   sur le chemin ToF. Un fil de capteur coupe se lisait donc "reservoir vide"
//   et le PID poussait la pompe a 255 indefiniment : sans note, sans client web,
//   depuis le seul demarrage. Une pompe qui tourne contre une valve fermee monte
//   en pression.
//
// Chaque test ci-dessous doit ECHOUER si on retire la correction correspondante
// (preuve par mutation faite dans le rapport). Les tests qui decrivent un COUT
// (un remplissage lent legitime qui ne doit pas etre interrompu, le mode direct
// volontairement non couvert) sont aussi importants que les autres : ce sont eux
// qui empecheront qu'on "simplifie" une securite dans six mois.
#include <cassert>
#include <cstdio>
#include <map>
#include "Arduino.h"
#include "Wire.h"
#include "ConfigStorage.h"
#include "PressureController.h"

extern std::map<uint8_t, int> __analog_writes, __digital_writes, __analog_reads, __digital_reads;

namespace {

const uint8_t kPumpPin = 25;
const uint8_t kHallPin = 36;
const uint8_t kEndstopPin = 27;

// Configuration minimale de mode reservoir (mode 5) avec capteur Hall.
void hwResetCfg() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.airMode = AIR_MODE_PUMP_RESERVOIR;
  cfg.numPumps = 1;
  cfg.pumpPins[0] = kPumpPin;
  cfg.pumpMinPwm[0] = 80;
  cfg.pumpMaxPwm[0] = 200;
  cfg.motorType = MOTOR_TYPE_PWM;
  cfg.pumpCascadeThreshold = 0;
  cfg.pumpStaggerMs = 0;
  cfg.bangbangHysteresis = 5;
  cfg.sensorType = SENSOR_TYPE_HALL_KY024;
  cfg.hallPin = kHallPin;
  cfg.hallThresholdLow = 1000;
  cfg.hallThresholdHigh = 2000;
  cfg.endstopPin = kEndstopPin;
  cfg.endstopActiveHigh = true;
  cfg.endstopPumpOn = false;
  cfg.sensorMinMm = 30;
  cfg.sensorTargetMm = 60;
  cfg.sensorMaxMm = 120;
  cfg.pidKp = 30;   // 3.0
  cfg.pidKi = 5;    // 0.5
  cfg.reservoirTargetPercent = 100;
  __analog_writes.clear();
  __digital_writes.clear();
  __analog_reads.clear();
  __digital_reads.clear();
  Wire.clear();
  // Jamais l'epoque : 0 n'est pas un horodatage, et un code qui compare
  // `millis() - t` a un delai doit etre teste ailleurs qu'a l'origine.
  __test_millis = 100000;
}

// Valeur ADC brute correspondant a un remplissage donne, dans la bande declaree.
uint16_t hallRawForFill(int fillPercent) {
  return (uint16_t)(cfg.hallThresholdLow +
                    (cfg.hallThresholdHigh - cfg.hallThresholdLow) * fillPercent / 100);
}

// Fait passer le temps en appelant update() a la cadence d'une vraie boucle.
// Retourne le PWM physique maximum observe pendant la periode.
int runFor(PressureController& pc, unsigned long durationMs, unsigned long stepMs = 20) {
  int maxPwm = 0;
  for (unsigned long t = 0; t < durationMs; t += stepMs) {
    __test_millis += stepMs;
    pc.update();
    if (__analog_writes[kPumpPin] > maxPwm) maxPwm = __analog_writes[kPumpPin];
  }
  return maxPwm;
}

// Avance jusqu'a ce que la pompe soit coupee, et rend le delai ecoule (ou
// `limitMs` si elle ne l'a jamais ete).
unsigned long runUntilPumpStops(PressureController& pc, unsigned long limitMs,
                                unsigned long stepMs = 20) {
  for (unsigned long t = 0; t < limitMs; t += stepMs) {
    __test_millis += stepMs;
    pc.update();
    if (__analog_writes[kPumpPin] == 0) return t + stepMs;
  }
  return limitMs;
}

// Un VL53L0X credible pour le stub I2C : Model ID correct, poignee de main SPAD,
// mesure declaree prete, statut de plage 11 (valide) et distance 60 mm.
void fakeTofReady(uint16_t distanceMm) {
  Wire.setPresent(0x29, true);
  Wire.setReg8(0x29, 0xC0, 0xEE);
  Wire.setReg8(0x29, 0x83, 0x01);
  Wire.setReg8(0x29, 0x13, 0x07);
  Wire.setReg8(0x29, 0x14, (uint8_t)(11 << 3));
  Wire.setReg8(0x29, 0x1E, (uint8_t)(distanceMm >> 8));
  Wire.setReg8(0x29, 0x1F, (uint8_t)(distanceMm & 0xFF));
}

// ---------------------------------------------------------------------------
// 1. LE SCENARIO DU RAPPORT D'AUDIT, execute tel quel.
// airMode=5, capteur Hall, autostart, fil coupe (ADC ~0). Avant correction :
// PWM pompe = 255 apres 60 s, et encore 255 apres UNE HEURE. Apres correction :
// jamais rien, parce qu'une lecture collee au rail n'est pas une mesure.
// ---------------------------------------------------------------------------
void hw_pump_hall_cut_wire_never_runs_the_pump() {
  hwResetCfg();
  __analog_reads[kHallPin] = 0;            // fil du capteur coupe
  PressureController pc;
  pc.begin();
  pc.setTargetPercent(cfg.reservoirTargetPercent);   // autostart reservoir

  assert(runFor(pc, 60000) == 0);          // 60 s : la pompe n'a jamais demarre
  assert(runFor(pc, 3600000) == 0);        // 1 h  : toujours rien
  assert(pc.getPumpPwm() == 0);
  assert(!pc.isPumpRunning());
  // La mesure n'existe pas : elle doit se declarer perimee, pas valoir 0 %.
  assert(pc.isMeasurementStale());
  assert(!pc.isMeasurementValid());
  // Et le capteur ne doit pas se dire "detecte" : il n'est pas identifiable.
  assert(pc.sensorPresence() == SENSOR_PRESENCE_PRESUMED);
  assert(std::string(pc.sensorStateName()) == "presumed_no_reading");

  // Meme chose cote rail haut (court-circuit a l'alimentation).
  hwResetCfg();
  __analog_reads[kHallPin] = 4095;
  PressureController pc2;
  pc2.begin();
  pc2.setTargetPercent(100);
  assert(runFor(pc2, 600000) == 0);
  assert(pc2.isMeasurementStale());
}

// ---------------------------------------------------------------------------
// 2. Une bande de seuils degeneree n'est pas une echelle de mesure : tout se
// lirait "0 %", donc "vide", donc pompe au maximum pour toujours.
// ---------------------------------------------------------------------------
void hw_pump_degenerate_hall_band_is_not_a_measurement() {
  hwResetCfg();
  cfg.hallThresholdHigh = cfg.hallThresholdLow;   // bande nulle
  __analog_reads[kHallPin] = 1500;                // lecture pourtant "propre"
  PressureController pc;
  pc.begin();
  pc.setTargetPercent(100);
  assert(runFor(pc, 300000) == 0);
  assert(pc.isMeasurementStale());
}

// ---------------------------------------------------------------------------
// 3. Un capteur Hall sain regule, puis meurt en cours de route. La pompe doit
// s'arreter environ RESERVOIR_STALE_MS apres la derniere mesure acceptee - pas
// continuer sur la derniere valeur connue.
// ---------------------------------------------------------------------------
void hw_pump_hall_sensor_lost_mid_run_stops_the_pump() {
  hwResetCfg();
  __analog_reads[kHallPin] = hallRawForFill(50);
  PressureController pc;
  pc.begin();
  pc.setTargetPercent(100);
  assert(runFor(pc, 2000) > 0);              // capteur sain : la pompe regule
  assert(!pc.isMeasurementStale());
  assert(pc.isMeasurementValid());

  __analog_reads[kHallPin] = 0;              // le fil casse ici
  unsigned long elapsed = runUntilPumpStops(pc, 10000);
  // Borne basse : on ne coupe pas au premier echec de lecture (la boucle
  // principale peut rater quelques cycles). Borne haute : on coupe bien dans le
  // delai annonce, et pas "un jour".
  assert(elapsed >= RESERVOIR_STALE_MS - PRESSURE_READ_INTERVAL_MS - 40);
  assert(elapsed <= RESERVOIR_STALE_MS + PRESSURE_READ_INTERVAL_MS + 40);
  assert(pc.getPumpPwm() == 0);
  assert(pc.isMeasurementStale());
  assert(runFor(pc, 600000) == 0);           // et ca ne repart pas tout seul
}

// ---------------------------------------------------------------------------
// 4. La panne qui ne se voit pas : une lecture PLAUSIBLE mais figee (fil coupe
// sur une entree qui flotte a mi-echelle, aimant tombe, fuite, valve fermee).
// La mesure est fraiche et dans la bande : aucune garde de plausibilite ne peut
// la refuser. Seule une duree maximale de marche protege le materiel.
// ---------------------------------------------------------------------------
void hw_pump_frozen_plausible_reading_trips_the_run_time_limit() {
  hwResetCfg();
  __analog_reads[kHallPin] = hallRawForFill(50);   // fige a 50 %, pour toujours
  PressureController pc;
  pc.begin();
  pc.setTargetPercent(100);

  // Avant l'echeance la pompe tourne : la limite n'est pas un bridage deguise.
  assert(runFor(pc, PUMP_MAX_RUN_MS - 2000) > 0);
  assert(!pc.isPumpRunawayLatched());
  assert(pc.isPumpRunning());

  unsigned long elapsed = runUntilPumpStops(pc, 10000);
  assert(elapsed <= 2000 + PRESSURE_PID_INTERVAL_MS + 40);   // coupee a l'echeance
  assert(pc.isPumpRunawayLatched());
  assert(std::string(pc.sensorStateName()) == "no_effect");

  // Et le verrou TIENT : sans preuve que le capteur vit, une heure de plus ne
  // redonne pas la main a la pompe (sinon la limite ne serait qu'un rapport
  // cyclique, et la pression continuerait de monter).
  assert(runFor(pc, 3600000) == 0);
  assert(pc.isPumpRunawayLatched());
}

// ---------------------------------------------------------------------------
// 5. Rearmement : la mesure qui BOUGE pendant que la pompe est arretee prouve
// que le capteur vit encore. C'est la seule chose qui rearme automatiquement.
// ---------------------------------------------------------------------------
void hw_pump_runaway_latch_clears_only_on_proof_of_life() {
  hwResetCfg();
  __analog_reads[kHallPin] = hallRawForFill(50);
  PressureController pc;
  pc.begin();
  pc.setTargetPercent(100);
  runFor(pc, PUMP_MAX_RUN_MS + 2000);
  assert(pc.isPumpRunawayLatched());

  // Le reservoir se vide en jouant : la mesure descend. Le capteur vit.
  __analog_reads[kHallPin] = hallRawForFill(20);
  assert(runFor(pc, 2000) > 0);              // la pompe peut repartir
  assert(!pc.isPumpRunawayLatched());

  // Rearmement explicite (a cabler sur l'UI) : meme effet, sans attendre.
  hwResetCfg();
  __analog_reads[kHallPin] = hallRawForFill(50);
  PressureController pc2;
  pc2.begin();
  pc2.setTargetPercent(100);
  runFor(pc2, PUMP_MAX_RUN_MS + 2000);
  assert(pc2.isPumpRunawayLatched());
  pc2.clearPumpRunawayFault();
  assert(!pc2.isPumpRunawayLatched());
  assert(runFor(pc2, 2000) > 0);
}

// ---------------------------------------------------------------------------
// 6. CE QUE LA CORRECTION COUTE, verrouille dans les deux sens : un remplissage
// legitime mais LENT ne doit jamais etre interrompu. Ici la mesure progresse de
// 6 % toutes les 30 s (8 minutes pour un remplissage complet, bien plus que
// PUMP_MAX_RUN_MS) : la fenetre se rearme sur la progression, pas sur l'horloge.
// ---------------------------------------------------------------------------
void hw_pump_slow_but_real_fill_is_never_interrupted() {
  hwResetCfg();
  cfg.reservoirTargetPercent = 80;
  PressureController pc;
  __analog_reads[kHallPin] = hallRawForFill(0);
  pc.begin();
  pc.setTargetPercent(80);

  int fill = 0;
  for (unsigned long t = 0; t < 600000UL; t += 20) {
    __test_millis += 20;
    if (t % 30000 == 0 && fill < 100) {         // +6 % toutes les 30 s
      fill += 6;
      if (fill > 100) fill = 100;
      __analog_reads[kHallPin] = hallRawForFill(fill);
    }
    pc.update();
    // Aucune coupure d'emballement pendant tout le remplissage.
    assert(!pc.isPumpRunawayLatched());
  }
}

// ---------------------------------------------------------------------------
// 7. Fin de course : la securite ne doit dependre d'AUCUN drapeau de polarite.
// Pour chacune des quatre combinaisons (actif HIGH/LOW x pompe ON sur actif ou
// sur inactif), on cable la ligne figee dans le sens "pomper" - c'est la panne
// qui, avant correction, laissait la pompe a 255 en permanence.
// ---------------------------------------------------------------------------
void hw_pump_endstop_stuck_line_stops_in_every_polarity() {
  for (int activeHigh = 0; activeHigh <= 1; activeHigh++) {
    for (int pumpOnActive = 0; pumpOnActive <= 1; pumpOnActive++) {
      hwResetCfg();
      cfg.sensorType = SENSOR_TYPE_ENDSTOP_MECH;
      cfg.endstopActiveHigh = (activeHigh != 0);
      cfg.endstopPumpOn = (pumpOnActive != 0);
      // Niveau qui signifie "il faut pomper" dans cette combinaison.
      bool wantActive = (pumpOnActive != 0);
      int level = (wantActive == (activeHigh != 0)) ? HIGH : LOW;
      __digital_reads[kEndstopPin] = level;

      PressureController pc;
      pc.begin();
      pc.setTargetPercent(100);
      assert(runFor(pc, 2000) > 0);             // la pompe demarre : cas nominal
      assert(!pc.isMeasurementStale());         // une entree logique reste fraiche

      unsigned long elapsed = runUntilPumpStops(pc, PUMP_MAX_RUN_MS + 10000);
      assert(elapsed <= PUMP_MAX_RUN_MS + 2000 + PRESSURE_PID_INTERVAL_MS);
      assert(pc.isPumpRunawayLatched());
      assert(runFor(pc, 600000) == 0);          // et ca reste coupe
    }
  }
}

// ---------------------------------------------------------------------------
// 8. Le rappel interne d'une entree fin de course suit la polarite DECLAREE et
// produit le niveau INACTIF : un contact sec n'impose qu'un seul des deux
// niveaux. L'ancien INPUT_PULLUP inconditionnel contredisait le defaut
// DEFAULT_ENDSTOP_ACTIVE_HIGH = true (lecture "actif" permanente).
// Ce n'est PAS une securite - voir le test 7 pour la securite.
// ---------------------------------------------------------------------------
void hw_pump_endstop_pull_matches_declared_inactive_level() {
  assert(PressureController::endstopPinModeFor(true) == INPUT_PULLDOWN);
  assert(PressureController::endstopPinModeFor(false) == INPUT_PULLUP);
  assert(INPUT_PULLDOWN != INPUT_PULLUP);
}

// ---------------------------------------------------------------------------
// 9. La peremption vaut pour TOUTES les familles de capteur. Avant correction
// isMeasurementStale() commencait par `if (!usesTofSensor()) return false;` :
// elle desactivait donc la garde pour deux familles sur trois.
// ---------------------------------------------------------------------------
void hw_pump_staleness_applies_to_every_sensor_family() {
  const uint8_t families[] = {SENSOR_TYPE_HALL_KY024, SENSOR_TYPE_ENDSTOP_MECH,
                              SENSOR_TYPE_ENDSTOP_OPT, SENSOR_TYPE_TOF_VL53L0X};
  for (uint8_t family : families) {
    hwResetCfg();
    cfg.sensorType = family;
    __analog_reads[kHallPin] = 0;              // rien de lisible cote Hall
    PressureController pc;
    pc.begin();
    // Rien n'a jamais ete mesure : l'epoque n'est pas une mesure. Vrai pour les
    // quatre familles, alors que la garde ne repondait "perimee" que pour le ToF.
    assert(pc.isMeasurementStale());
    pc.setTargetPercent(100);
    // Les familles qu'une panne peut priver de mesure (ADC colle au rail, ToF
    // absent) gardent la pompe a l'arret. Une entree logique, elle, rend
    // toujours un niveau : rien ne peut la rendre perimee tant que la boucle
    // tourne, et c'est le chien de garde de marche qui la couvre (test 7).
    if (family != SENSOR_TYPE_ENDSTOP_MECH && family != SENSOR_TYPE_ENDSTOP_OPT) {
      assert(runFor(pc, 5000) == 0);
      assert(pc.isMeasurementStale());
    }
  }

  // Et une mesure acceptee la rend fraiche, pour les familles non-ToF aussi.
  hwResetCfg();
  __analog_reads[kHallPin] = hallRawForFill(40);
  PressureController pcHall;
  pcHall.begin();
  assert(!pcHall.isMeasurementStale());
  assert(pcHall.getFillPercent() == 40);

  hwResetCfg();
  cfg.sensorType = SENSOR_TYPE_ENDSTOP_OPT;
  __digital_reads[kEndstopPin] = LOW;
  PressureController pcEnd;
  pcEnd.begin();
  assert(pcEnd.isMeasurementStale());          // pas encore lue
  __test_millis += 20;
  pcEnd.update();
  assert(!pcEnd.isMeasurementStale());         // lue a l'instant
}

// ---------------------------------------------------------------------------
// 9 bis. Au DEMARRAGE, l'epoque n'est pas une mesure. millis() vaut quelques
// centaines de ms quand begin() s'execute : `millis() - _lastValidReadTime` est
// alors inferieur au delai de peremption et la mesure jamais faite passait pour
// "recente". La pompe pouvait donc reguler sur un 0 % invente pendant tout le
// premier delai suivant la mise sous tension - y compris sur le chemin ToF.
// Ce test part d'un temps de boot realiste, pas d'une horloge deja avancee.
// ---------------------------------------------------------------------------
void hw_pump_nothing_runs_before_the_first_measurement() {
  hwResetCfg();
  __test_millis = 250;                         // on sort du boot
  __analog_reads[kHallPin] = 0;                // capteur Hall muet
  PressureController hall;
  hall.begin();
  hall.setTargetPercent(100);
  assert(hall.isMeasurementStale());
  assert(runFor(hall, 5000, 10) == 0);

  // Meme chose sur le chemin ToF : capteur reellement initialise, mais aucune
  // mesure ne se termine jamais.
  hwResetCfg();
  __test_millis = 250;
  cfg.sensorType = SENSOR_TYPE_TOF_VL53L0X;
  fakeTofReady(60);
  PressureController tof;
  assert(tof.begin());
  Wire.setReg8(0x29, 0x13, 0x00);              // plus jamais "mesure prete"
  tof.setTargetPercent(100);
  assert(tof.isMeasurementStale());
  assert(runFor(tof, 5000, 10) == 0);
}

// ---------------------------------------------------------------------------
// 10. Le chemin ToF reste le modele : il continue de reguler normalement apres
// la reorganisation (acquisition -> gardes communes -> controle), et il est
// desormais couvert par la MEME limite de duree que les autres familles.
// ---------------------------------------------------------------------------
void hw_pump_tof_path_still_regulates_and_shares_the_limit() {
  hwResetCfg();
  cfg.sensorType = SENSOR_TYPE_TOF_VL53L0X;
  fakeTofReady(60);                       // 60 mm sur une echelle 30-120 mm
  PressureController pc;
  assert(pc.begin());
  assert(pc.sensorPresence() == SENSOR_PRESENCE_DETECTED);   // identifie, lui
  assert(std::string(pc.sensorStateName()) == "ready");
  pc.setTargetPercent(100);
  assert(runFor(pc, 2000) > 0);           // regulation nominale
  assert(pc.isMeasurementValid());
  assert(!pc.isMeasurementStale());
  assert(pc.getDistanceMm() == 60);

  // Distance figee = la pompe ne remplit rien, quelle que soit la qualite du
  // capteur. La limite de duree s'applique aussi au ToF.
  runFor(pc, PUMP_MAX_RUN_MS + 2000);
  assert(pc.isPumpRunawayLatched());
  assert(pc.getPumpPwm() == 0);
}

// ---------------------------------------------------------------------------
// 11. Un capteur "presume" ne doit jamais se declarer "detecte". Seul le ToF
// s'identifie (Model ID) ; le Hall et les fins de course sont surveilles, pas
// detectes, et l'etat expose doit le dire.
// ---------------------------------------------------------------------------
void hw_pump_presumed_is_never_reported_as_detected() {
  hwResetCfg();
  __analog_reads[kHallPin] = hallRawForFill(50);
  PressureController hall;
  hall.begin();
  assert(hall.sensorPresence() == SENSOR_PRESENCE_PRESUMED);
  assert(std::string(hall.sensorStateName()) == "presumed");

  hwResetCfg();
  cfg.sensorType = SENSOR_TYPE_ENDSTOP_MECH;
  PressureController endstop;
  endstop.begin();
  assert(endstop.sensorPresence() == SENSOR_PRESENCE_PRESUMED);

  hwResetCfg();
  cfg.sensorType = SENSOR_TYPE_TOF_VL6180X;     // rien sur le bus
  PressureController absent;
  assert(!absent.begin());
  assert(absent.sensorPresence() == SENSOR_PRESENCE_ABSENT);
  assert(std::string(absent.sensorStateName()) == "absent");
}

// ---------------------------------------------------------------------------
// 12. CE QUI N'EST VOLONTAIREMENT PAS COUVERT : le mode pompe directe (4). Il
// n'a aucun capteur, donc aucune mesure a mettre en doute ; la consigne EST la
// demande de l'operateur. Couper une note tenue serait une panne musicale sans
// contrepartie de securite. Ce test existe pour que ce choix soit explicite.
// ---------------------------------------------------------------------------
void hw_pump_direct_mode_is_deliberately_not_time_limited() {
  hwResetCfg();
  cfg.airMode = AIR_MODE_PUMP_VALVE;
  PressureController pc;
  pc.begin();
  pc.setTargetPercent(100);
  assert(runFor(pc, PUMP_MAX_RUN_MS * 2) > 0);
  assert(pc.isPumpRunning());
  assert(!pc.isPumpRunawayLatched());
}

// ---------------------------------------------------------------------------
// 13. Un test manuel mono-pompe est borne dans le temps : il est lance par une
// commande web et arrete par une autre. Onglet ferme, WiFi coupe, operateur
// parti : plus personne n'envoie l'arret.
// ---------------------------------------------------------------------------
void hw_pump_manual_single_pump_test_expires() {
  hwResetCfg();
  cfg.airMode = AIR_MODE_PUMP_VALVE;
  PressureController pc;
  pc.begin();
  pc.testSinglePump(0, 50);
  assert(runFor(pc, PUMP_TEST_MAX_MS - 2000) > 0);   // le test tourne
  assert(pc.isPumpRunning());
  assert(runUntilPumpStops(pc, 10000) <= 2000 + 40);
  assert(pc.getPumpPwm() == 0);
  assert(runFor(pc, 600000) == 0);                   // et il ne repart pas
}

}  // namespace

void hw_pump_run_all_tests() {
  hw_pump_hall_cut_wire_never_runs_the_pump();
  hw_pump_degenerate_hall_band_is_not_a_measurement();
  hw_pump_hall_sensor_lost_mid_run_stops_the_pump();
  hw_pump_frozen_plausible_reading_trips_the_run_time_limit();
  hw_pump_runaway_latch_clears_only_on_proof_of_life();
  hw_pump_slow_but_real_fill_is_never_interrupted();
  hw_pump_endstop_stuck_line_stops_in_every_polarity();
  hw_pump_endstop_pull_matches_declared_inactive_level();
  hw_pump_staleness_applies_to_every_sensor_family();
  hw_pump_nothing_runs_before_the_first_measurement();
  hw_pump_tof_path_still_regulates_and_shares_the_limit();
  hw_pump_presumed_is_never_reported_as_detected();
  hw_pump_direct_mode_is_deliberately_not_time_limited();
  hw_pump_manual_single_pump_test_expires();
}

#ifdef STANDALONE_TEST_MAIN
int main() { hw_pump_run_all_tests(); printf("hw pump tests passed\n"); return 0; }
#endif
