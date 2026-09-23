/***********************************************************************************************
 * test_hw_servo.cpp - Tests natifs des bornages de SURETE MATERIELLE
 *
 * Ce que ce fichier verrouille, et pourquoi c'est ici et pas ailleurs :
 *
 *   1. LES COMMANDES DE REGLAGE NE SORTENT PLUS DE LA COURSE MECANIQUE.
 *      testAirflowAngle(), testAngleServoAngle() et testFingerAngle() ne
 *      bornaient qu'a SERVO_MAX_ANGLE, c'est-a-dire a la course ELECTRIQUE du
 *      servo. Un curseur glisse jusqu'au bout commandait 180 deg a un doigt
 *      dont la course fait 30 deg : le servo poussait contre sa butee et y
 *      restait, a son courant de calage. Les tests mesurent maintenant l'angle
 *      REELLEMENT ecrit (via le PWM sorti, converti par servoAngleToPWM), pas
 *      l'intention.
 *
 *   2. LA RETOMBEE THERMIQUE DE LA BOBINE RESTE UNE RETOMBEE. sol_hold = 255
 *      etait accepte par le validateur : le mecanisme anti-chauffe s'executait
 *      et ecrivait 255. Et testSolenoid(true) rearmait le chronometre de
 *      retombee a chaque appel, donc un flot de commandes gardait la bobine a
 *      pleine tension indefiniment.
 *
 *   3. UNE VALEUR QUI DESARME UNE PROTECTION NE PASSE PLUS EN SILENCE.
 *      timeUnpower = 0 reste accepte - il y a des montages qui en ont besoin -
 *      mais il produit desormais un avertissement que TOUT client recoit.
 *
 * CE QUI N'EST PAS ICI : les corrections de WebConfigurator (plafond absolu de
 * session de test, limiteur de debit WebSocket, annulation de calibration sur
 * panique). WebConfigurator.cpp n'est pas dans la compilation native - il exige
 * ESPAsyncWebServer - et le mettre dans ce harnais demanderait de simuler une
 * pile TCP entiere. Cette partie est verifiee par compilation contre les vraies
 * entetes, pas par execution ; le rapport le dit explicitement.
 ***********************************************************************************************/
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "Arduino.h"
#include "ConfigStorage.h"
#include "FingerController.h"
#include "AirflowController.h"
#include "ServoMath.h"

extern std::map<uint8_t, int> __analog_writes;

namespace {

// Configuration de reference. Volontairement locale a ce fichier : les valeurs
// des plages sont ce que les tests raisonnent, elles doivent etre lisibles ici
// et ne pas bouger parce qu'un autre fichier de test a change son decor.
//
// Elle reprend les valeurs EXPEDIEES par settings.h (SERVO_AIRFLOW_OFF 20 /
// MIN 60 / MAX 100, SERVO_ANGLE_OFF 90 / MIN 60 / MAX 120, ANGLE_OPEN 30,
// SOLENOID_PWM_ACTIVATION 255 / HOLDING 128 / ACTIVATION_TIME_MS 50), pour que
// les enveloppes calculees soient celles d'une machine reelle.
void hwResetCfg() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.numFingers = 3;
  for (uint8_t i = 0; i < 3; i++) {
    cfg.fingers[i].pcaChannel = i;
    cfg.fingers[i].closedAngle = 90;
    cfg.fingers[i].direction = -1;
    cfg.fingers[i].halfPercent = 0;
  }
  cfg.fingerAngleOpen = 30;
  cfg.halfHolePercent = 50;

  cfg.numNotes = 1;
  cfg.notes[0].midiNote = 60;
  cfg.notes[0].airflowMinPercent = 20;
  cfg.notes[0].airflowNominalPercent = 50;
  cfg.notes[0].airflowMaxPercent = 80;
  cfg.notes[0].anglePercent = 50;

  cfg.airMode = AIR_MODE_SOLENOID_SERVO;
  cfg.valveType = 0;
  cfg.airflowPcaChannel = 10;
  cfg.angleServoPcaChannel = 12;
  cfg.valveServoPcaChannel = 11;
  cfg.solenoidPin = 13;
  cfg.solenoidPwmActivation = 255;
  cfg.solenoidPwmHolding = 128;
  cfg.solenoidActivationTimeMs = 50;

  cfg.servoAirflowOff = 20;
  cfg.servoAirflowMin = 60;
  cfg.servoAirflowMax = 100;
  cfg.servoAngleOff = 90;
  cfg.servoAngleMin = 60;
  cfg.servoAngleMax = 120;
  cfg.angleServoEnabled = true;

  cfg.numPumps = 1;
  cfg.pumpPins[0] = 25;
  cfg.pumpMinPwm[0] = 80;
  cfg.pumpMaxPwm[0] = 200;
  cfg.motorType = MOTOR_TYPE_PWM;

  cfg.ccVolumeDefault = 127;
  cfg.ccExpressionDefault = 127;
  cfg.ccBreathDefault = 127;
  cfg.ccBrightnessDefault = 64;
  cfg.airVelocityResponse = 100;
  cfg.cc2Enabled = false;
  cfg.cc2ResponseCurve = 1.4f;
  cfg.vibratoFrequencyHz = 6.0f;
  cfg.vibratoMaxAmplitudeDeg = 8.0f;
  cfg.airAttackMs = 150;
  cfg.minNoteDurationMs = 0;
  cfg.midiStorageLimitKb = 500;
  cfg.timeUnpower = 200;
  strcpy(cfg.embouchure, "trav");
  strcpy(cfg.resFormat, "balloon");
  strcpy(cfg.instrumentColor, "#D4B044");

  __test_millis = 0;
  __analog_writes.clear();
}

// Recherche d'un motif dans la liste d'avertissements du validateur. strstr()
// plutot que String::indexOf(), qui ne prend qu'un caractere dans les stubs
// natifs et une chaine sur ESP32 : c_str() existe des deux cotes.
bool warns(const String& warnings, const char* needle) {
  return strstr(warnings.c_str(), needle) != nullptr;
}

// Enregistre le DERNIER angle ecrit sur chaque canal PCA. On repasse par
// servoAngleToPWM parce que c'est la seule chose que le materiel voit : un test
// qui lirait un accesseur interne ne dirait pas ou le servo va reellement.
struct PwmSpy {
  std::map<uint8_t, uint16_t> lastPwm;
  std::map<uint8_t, int> writes;
  void operator()(uint8_t ch, uint16_t, uint16_t off) {
    lastPwm[ch] = off;
    writes[ch]++;
  }
  bool wrote(uint8_t ch) const { return writes.count(ch) && writes.at(ch) > 0; }
  // Angle correspondant au dernier PWM ecrit sur ce canal.
  int angleOn(uint8_t ch) const {
    auto it = lastPwm.find(ch);
    if (it == lastPwm.end()) return -1;
    for (int a = SERVO_MIN_ANGLE; a <= SERVO_MAX_ANGLE; a++) {
      if (servoAngleToPWM((uint16_t)a) == it->second) return a;
    }
    return -2;   // PWM hors de toute la course : impossible, mais explicite
  }
};

/*=============================================================================
 * 1. Servo de souffle : la commande de reglage reste dans la course declaree
 *===========================================================================*/
void airflow_test_angle_stays_inside_declared_travel() {
  hwResetCfg();
  PwmSpy spy;
  AirflowController ac(std::ref(spy));

  // Enveloppe declaree = {off 20, min 60, max 100} -> [20, 100].
  // Elargie de SERVO_TEST_MARGIN_DEG -> [0, 120].
  const int lo = 20 - SERVO_TEST_MARGIN_DEG;    // 0
  const int hi = 100 + SERVO_TEST_MARGIN_DEG;   // 120

  // C'est la mesure exacte du rapport d'audit : a=180 arrivait a 180 deg sur
  // une plage configuree 60..100.
  ac.testAirflowAngle(180);
  assert(spy.angleOn(cfg.airflowPcaChannel) == hi);
  assert(ac.getAirflowAngle() == (uint16_t)hi);

  ac.testAirflowAngle(0);
  assert(spy.angleOn(cfg.airflowPcaChannel) == lo);

  // Une valeur A L'INTERIEUR n'est jamais deplacee : le bornage ne doit pas
  // rendre le reglage imprecis.
  ac.testAirflowAngle(83);
  assert(spy.angleOn(cfg.airflowPcaChannel) == 83);
  ac.testAirflowAngle((uint16_t)lo);
  assert(spy.angleOn(cfg.airflowPcaChannel) == lo);
  ac.testAirflowAngle((uint16_t)hi);
  assert(spy.angleOn(cfg.airflowPcaChannel) == hi);

  // Une plage haute et etroite : l'enveloppe suit la CONFIGURATION, elle n'est
  // pas un intervalle fixe deguise.
  hwResetCfg();
  cfg.servoAirflowOff = 150;
  cfg.servoAirflowMin = 155;
  cfg.servoAirflowMax = 170;
  PwmSpy spy2;
  AirflowController ac2(std::ref(spy2));
  ac2.testAirflowAngle(0);
  assert(spy2.angleOn(cfg.airflowPcaChannel) == 150 - SERVO_TEST_MARGIN_DEG);   // 130
  ac2.testAirflowAngle(180);
  // 170 + 20 = 190 depasserait la course electrique : ramene au rail.
  assert(spy2.angleOn(cfg.airflowPcaChannel) == SERVO_MAX_ANGLE);

  // Enveloppe collee au rail haut : le bornage ne peut jamais rendre un angle
  // hors de la course electrique.
  hwResetCfg();
  cfg.servoAirflowOff = 175;
  cfg.servoAirflowMin = 176;
  cfg.servoAirflowMax = 179;
  PwmSpy spy3;
  AirflowController ac3(std::ref(spy3));
  ac3.testAirflowAngle(65535);
  assert(spy3.angleOn(cfg.airflowPcaChannel) == SERVO_MAX_ANGLE);
}

/*=============================================================================
 * 2. Servo d'angle (traversiere) : meme regle, sur sa propre enveloppe
 *===========================================================================*/
void angle_servo_test_angle_stays_inside_declared_travel() {
  hwResetCfg();
  PwmSpy spy;
  AirflowController ac(std::ref(spy));

  // {off 90, min 60, max 120} -> [60, 120] -> elargi [40, 140].
  ac.testAngleServoAngle(180);
  assert(spy.angleOn(cfg.angleServoPcaChannel) == 120 + SERVO_TEST_MARGIN_DEG);
  assert(ac.getAngleServoAngle() == (uint16_t)(120 + SERVO_TEST_MARGIN_DEG));

  ac.testAngleServoAngle(0);
  assert(spy.angleOn(cfg.angleServoPcaChannel) == 60 - SERVO_TEST_MARGIN_DEG);

  ac.testAngleServoAngle(95);
  assert(spy.angleOn(cfg.angleServoPcaChannel) == 95);

  // Embouchure non traversiere : le servo d'angle n'existe pas, aucune ecriture.
  // (Garde preexistante - on verifie que le bornage ne l'a pas contournee.)
  hwResetCfg();
  strcpy(cfg.embouchure, "bec");
  PwmSpy spy2;
  AirflowController ac2(std::ref(spy2));
  ac2.testAngleServoAngle(180);
  assert(!spy2.wrote(cfg.angleServoPcaChannel));
}

/*=============================================================================
 * 3. Doigts : l'enveloppe est celle de CE doigt, dans SON sens
 *===========================================================================*/
void finger_test_angle_stays_inside_declared_travel() {
  hwResetCfg();
  PwmSpy spy;
  FingerController fc(std::ref(spy));

  // Doigt 0 : ferme 90, sens -1, ouverture 30 -> course [60, 90] -> [40, 110].
  fc.testFingerAngle(0, 180);
  assert(spy.angleOn(0) == 110);
  fc.testFingerAngle(0, 0);
  assert(spy.angleOn(0) == 40);
  fc.testFingerAngle(0, 72);
  assert(spy.angleOn(0) == 72);

  // Doigt 1 dans l'AUTRE sens, ferme ailleurs : course [100, 130] -> [80, 150].
  cfg.fingers[1].closedAngle = 100;
  cfg.fingers[1].direction = 1;
  fc.testFingerAngle(1, 180);
  assert(spy.angleOn(1) == 150);
  fc.testFingerAngle(1, 0);
  assert(spy.angleOn(1) == 80);

  // Les deux doigts n'ont donc PAS la meme borne : l'enveloppe est bien par
  // doigt, elle n'est pas un intervalle global.
  assert(spy.angleOn(0) != spy.angleOn(1));

  // Course collee au rail : la marge ne fait jamais sortir de 0..180.
  cfg.fingers[2].closedAngle = 175;
  cfg.fingers[2].direction = 1;
  fc.testFingerAngle(2, 65535);
  assert(spy.angleOn(2) == SERVO_MAX_ANGLE);

  // Indice hors plage : toujours refuse, aucune ecriture (garde preexistante).
  PwmSpy spy2;
  FingerController fc2(std::ref(spy2));
  fc2.testFingerAngle(7, 90);
  assert(spy2.writes.empty());
  fc2.testFingerAngle(-1, 90);
  assert(spy2.writes.empty());
}

/*=============================================================================
 * 4. Le chemin de JEU n'est pas retreci par le bornage
 *
 * C'est la contrepartie indispensable des trois tests precedents : un bornage
 * qui protegerait le materiel en empechant l'instrument de fermer ses trous
 * serait une regression, pas une correction.
 *===========================================================================*/
void play_path_is_untouched_by_the_test_bounds() {
  hwResetCfg();
  PwmSpy spy;
  FingerController fc(std::ref(spy));

  fc.closeAllFingers();
  for (uint8_t i = 0; i < cfg.numFingers; i++) {
    assert(spy.angleOn(cfg.fingers[i].pcaChannel) == (int)cfg.fingers[i].closedAngle);
  }

  fc.openAllFingers();
  for (uint8_t i = 0; i < cfg.numFingers; i++) {
    const int expected = (int)cfg.fingers[i].closedAngle +
                         (int)cfg.fingerAngleOpen * (int)cfg.fingers[i].direction;
    assert(spy.angleOn(cfg.fingers[i].pcaChannel) == expected);
  }

  // Demi-trou : entre les deux, donc a l'interieur par construction.
  uint8_t half[MAX_FINGER_SERVOS] = {2, 2, 2};
  fc.setFingerPattern(half);
  for (uint8_t i = 0; i < cfg.numFingers; i++) {
    const int closed = (int)cfg.fingers[i].closedAngle;
    const int opened = closed + (int)cfg.fingerAngleOpen * (int)cfg.fingers[i].direction;
    const int a = spy.angleOn(cfg.fingers[i].pcaChannel);
    assert(a >= (closed < opened ? closed : opened));
    assert(a <= (closed < opened ? opened : closed));
  }

  // Souffle : repos et pleine puissance restent atteignables exactement.
  PwmSpy aspy;
  AirflowController ac(std::ref(aspy));
  ac.setAirflowToRest();
  assert(aspy.angleOn(cfg.airflowPcaChannel) == (int)cfg.servoAirflowOff);
  ac.setAirflowVelocity(127);
  assert(aspy.angleOn(cfg.airflowPcaChannel) == (int)cfg.servoAirflowMax);
  ac.setAirflowVelocity(1);
  assert(aspy.angleOn(cfg.airflowPcaChannel) == (int)cfg.servoAirflowMin);
}

/*=============================================================================
 * 5. test_sol : une valve deja ouverte ne se re-impulse pas
 *
 * Scenario reproduit : un client envoie {"t":"test_sol","o":1} toutes les
 * 40 ms. Chaque appel reecrivait solenoidPwmActivation et rearmait le
 * chronometre, donc la bascule vers solenoidPwmHolding n'arrivait jamais et la
 * bobine restait a pleine tension aussi longtemps que le flot durait.
 *===========================================================================*/
void test_solenoid_does_not_repulse_an_open_valve() {
  hwResetCfg();
  PwmSpy spy;
  AirflowController ac(std::ref(spy));
  ac.begin();

  const uint8_t pin = cfg.solenoidPin;

  __test_millis = 1000;
  ac.testSolenoid(true);
  assert(ac.isValveOpen());
  assert(__analog_writes[pin] == cfg.solenoidPwmActivation);

  // La retombee arrive a l'heure quand personne ne la derange.
  __test_millis = 1000 + cfg.solenoidActivationTimeMs;
  ac.update();
  assert(__analog_writes[pin] == cfg.solenoidPwmHolding);

  // LE CAS DU DEFAUT : une nouvelle commande d'ouverture sur une valve deja
  // ouverte. Le sentinel -1 prouve l'absence d'ecriture : si openValve()
  // repassait, il ecrirait solenoidPwmActivation.
  __analog_writes[pin] = -1;
  ac.testSolenoid(true);
  assert(__analog_writes[pin] == -1);
  assert(ac.isValveOpen());

  // Et le flot complet : 25 commandes a 40 ms d'intervalle, soit une seconde.
  // Sans la garde, la bobine serait restee a 255 toute cette seconde.
  __analog_writes[pin] = cfg.solenoidPwmHolding;
  for (int i = 0; i < 25; i++) {
    __test_millis += 40;
    ac.testSolenoid(true);
    ac.update();
    assert(__analog_writes[pin] == cfg.solenoidPwmHolding);
  }

  // La FERMETURE, elle, doit toujours aboutir - y compris repetee, y compris
  // sur une valve deja fermee. La symetriser avec la garde transformerait un
  // desaccord d'etat en valve bloquee ouverte.
  ac.testSolenoid(false);
  assert(!ac.isValveOpen());
  assert(__analog_writes[pin] == 0);
  __analog_writes[pin] = -1;
  ac.testSolenoid(false);
  assert(__analog_writes[pin] == 0);
  assert(!ac.isValveOpen());

  // Apres une vraie fermeture, une ouverture repart bien a pleine tension :
  // la garde n'a pas casse l'appel de l'electrovanne.
  __test_millis += 100;
  ac.testSolenoid(true);
  assert(__analog_writes[pin] == cfg.solenoidPwmActivation);
}

/*=============================================================================
 * 6. Validateur : le maintien de bobine doit rester une REDUCTION
 *===========================================================================*/
void validator_caps_solenoid_hold_against_activation() {
  // Le cas exact du rapport : sol_hold = 255 etait accepte tel quel.
  hwResetCfg();
  cfg.solenoidPwmActivation = 255;
  cfg.solenoidPwmHolding = 255;
  ConfigValidationResult r = validateAndNormalizeConfig(cfg, nullptr);
  assert(r.valid);
  assert(r.corrected);
  assert(cfg.solenoidPwmHolding == 128);          // 50 % de 255, arrondi
  assert(cfg.solenoidPwmHolding < cfg.solenoidPwmActivation);
  assert(warns(r.warnings, "solenoidPwmHolding"));

  // LA VALEUR PAR DEFAUT EXPEDIEE N'EST PAS TOUCHEE. C'est la contrainte qui a
  // fixe le plafond a 50 % arrondi au superieur : une configuration qui marche
  // ne doit pas etre "corrigee" par une regle de surete.
  hwResetCfg();
  cfg.solenoidPwmActivation = 255;
  cfg.solenoidPwmHolding = 128;
  ConfigValidationResult r2 = validateAndNormalizeConfig(cfg, nullptr);
  assert(r2.valid);
  assert(cfg.solenoidPwmHolding == 128);
  assert(!warns(r2.warnings, "solenoidPwmHolding"));

  // Le plafond suit l'activation, il n'est pas une constante deguisee.
  hwResetCfg();
  cfg.solenoidPwmActivation = 100;
  cfg.solenoidPwmHolding = 90;
  validateAndNormalizeConfig(cfg, nullptr);
  assert(cfg.solenoidPwmHolding == 50);

  // Egalite : c'est le cas ou la retombee est un no-op sans etre "superieure".
  hwResetCfg();
  cfg.solenoidPwmActivation = 200;
  cfg.solenoidPwmHolding = 200;
  validateAndNormalizeConfig(cfg, nullptr);
  assert(cfg.solenoidPwmHolding == 100);

  // Activation nulle : le maintien ne peut valoir que 0. Pas de division, pas
  // de debordement, pas de valeur inventee.
  hwResetCfg();
  cfg.solenoidPwmActivation = 0;
  cfg.solenoidPwmHolding = 255;
  validateAndNormalizeConfig(cfg, nullptr);
  assert(cfg.solenoidPwmHolding == 0);

  // Un maintien deja sous le plafond passe intact.
  hwResetCfg();
  cfg.solenoidPwmActivation = 255;
  cfg.solenoidPwmHolding = 60;
  validateAndNormalizeConfig(cfg, nullptr);
  assert(cfg.solenoidPwmHolding == 60);
}

/*=============================================================================
 * 7. Validateur : la phase de pleine puissance est bornee
 *===========================================================================*/
void validator_caps_solenoid_full_power_window() {
  hwResetCfg();
  cfg.solenoidActivationTimeMs = 5000;   // accepte auparavant (CONFIG_MAX_...)
  ConfigValidationResult r = validateAndNormalizeConfig(cfg, nullptr);
  assert(r.valid && r.corrected);
  assert(cfg.solenoidActivationTimeMs == SOLENOID_PULSE_MAX_MS);
  assert(cfg.solenoidActivationTimeMs < CONFIG_MAX_SOLENOID_PULSE_MS);

  // 65535 ms : le pire cas que le type puisse porter.
  hwResetCfg();
  cfg.solenoidActivationTimeMs = 65535;
  validateAndNormalizeConfig(cfg, nullptr);
  assert(cfg.solenoidActivationTimeMs == SOLENOID_PULSE_MAX_MS);

  // La valeur par defaut expediee (50 ms) n'est pas touchee.
  hwResetCfg();
  cfg.solenoidActivationTimeMs = 50;
  ConfigValidationResult r3 = validateAndNormalizeConfig(cfg, nullptr);
  assert(r3.valid);
  assert(cfg.solenoidActivationTimeMs == 50);
}

/*=============================================================================
 * 8. Validateur : timeUnpower = 0 est accepte, mais plus en silence
 *
 * Le choix est argumente dans ConfigValidator.cpp : l'interdire ne supprimerait
 * pas le danger, seulement son nom (qui veut tenir une position mettrait
 * 60000). Ce qui est verrouille ici est qu'il soit DIT, a tout client.
 *===========================================================================*/
void validator_warns_but_accepts_permanent_servo_power() {
  hwResetCfg();
  cfg.timeUnpower = 0;
  ConfigValidationResult r = validateAndNormalizeConfig(cfg, nullptr);
  assert(r.valid);                       // accepte : ce n'est pas une erreur
  assert(cfg.timeUnpower == 0);          // et pas silencieusement remplace
  assert(warns(r.warnings, "timeUnpower=0"));

  // Une valeur normale ne declenche pas l'avertissement : un avertissement
  // permanent ne serait plus un avertissement.
  hwResetCfg();
  cfg.timeUnpower = 200;
  ConfigValidationResult r2 = validateAndNormalizeConfig(cfg, nullptr);
  assert(r2.valid);
  assert(cfg.timeUnpower == 200);
  assert(!warns(r2.warnings, "timeUnpower"));

  // Et le plafond haut reste celui d'avant : cette correction n'a rien retire.
  hwResetCfg();
  cfg.timeUnpower = 65535;
  validateAndNormalizeConfig(cfg, nullptr);
  assert(cfg.timeUnpower == CONFIG_MAX_UNPOWER_MS);
}

/*=============================================================================
 * 9. Une configuration expediee traverse le validateur sans etre corrigee
 *
 * Garde-fou contre la tentation de resserrer une borne "pour etre sur" : si
 * l'un de mes plafonds mordait sur les valeurs par defaut du projet, ce test
 * le dirait immediatement.
 *===========================================================================*/
void shipped_defaults_survive_validation_untouched() {
  hwResetCfg();
  cfg.solenoidPwmActivation = SOLENOID_PWM_ACTIVATION;
  cfg.solenoidPwmHolding = SOLENOID_PWM_HOLDING;
  cfg.solenoidActivationTimeMs = SOLENOID_ACTIVATION_TIME_MS;
  cfg.timeUnpower = TIMEUNPOWER;
  cfg.servoAirflowOff = SERVO_AIRFLOW_OFF;
  cfg.servoAirflowMin = SERVO_AIRFLOW_MIN;
  cfg.servoAirflowMax = SERVO_AIRFLOW_MAX;
  cfg.servoAngleOff = SERVO_ANGLE_OFF;
  cfg.servoAngleMin = SERVO_ANGLE_MIN;
  cfg.servoAngleMax = SERVO_ANGLE_MAX;
  cfg.fingerAngleOpen = ANGLE_OPEN;

  ConfigValidationResult r = validateAndNormalizeConfig(cfg, nullptr);
  assert(r.valid);
  assert(cfg.solenoidPwmActivation == SOLENOID_PWM_ACTIVATION);
  assert(cfg.solenoidPwmHolding == SOLENOID_PWM_HOLDING);
  assert(cfg.solenoidActivationTimeMs == SOLENOID_ACTIVATION_TIME_MS);
  assert(cfg.timeUnpower == TIMEUNPOWER);
  assert(!warns(r.warnings, "solenoidPwmHolding"));
  assert(!warns(r.warnings, "timeUnpower"));

  // Et sur ces valeurs-la, les positions de JEU restent toutes commandables par
  // le reglage manuel : l'ecran de configuration reste utilisable.
  PwmSpy spy;
  AirflowController ac(std::ref(spy));
  ac.testAirflowAngle(cfg.servoAirflowOff);
  assert(spy.angleOn(cfg.airflowPcaChannel) == (int)cfg.servoAirflowOff);
  ac.testAirflowAngle(cfg.servoAirflowMin);
  assert(spy.angleOn(cfg.airflowPcaChannel) == (int)cfg.servoAirflowMin);
  ac.testAirflowAngle(cfg.servoAirflowMax);
  assert(spy.angleOn(cfg.airflowPcaChannel) == (int)cfg.servoAirflowMax);

  PwmSpy fspy;
  FingerController fc(std::ref(fspy));
  for (uint8_t i = 0; i < cfg.numFingers; i++) {
    const int closed = (int)cfg.fingers[i].closedAngle;
    const int opened = closed + (int)cfg.fingerAngleOpen * (int)cfg.fingers[i].direction;
    fc.testFingerAngle(i, (uint16_t)closed);
    assert(fspy.angleOn(cfg.fingers[i].pcaChannel) == closed);
    fc.testFingerAngle(i, (uint16_t)opened);
    assert(fspy.angleOn(cfg.fingers[i].pcaChannel) == opened);
  }
}

}  // namespace

void hw_servo_run_all_tests() {
  airflow_test_angle_stays_inside_declared_travel();
  angle_servo_test_angle_stays_inside_declared_travel();
  finger_test_angle_stays_inside_declared_travel();
  play_path_is_untouched_by_the_test_bounds();
  test_solenoid_does_not_repulse_an_open_valve();
  validator_caps_solenoid_hold_against_activation();
  validator_caps_solenoid_full_power_window();
  validator_warns_but_accepts_permanent_servo_power();
  shipped_defaults_survive_validation_untouched();
}

#ifdef STANDALONE_TEST_MAIN
int main() { hw_servo_run_all_tests(); printf("hw servo tests passed\n"); return 0; }
#endif
