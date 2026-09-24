// Finalisation 2 - LOT 2 : une auto-calibration ne doit jamais demarrer
// par-dessus une session de test manuel encore ouverte.
//
// LE DEFAUT, tel qu'il existe dans le depot
// -----------------------------------------
// `cancelActiveActuatorSession()`, appelee au demarrage d'une calibration,
// arrete le calibrateur et relache la session d'actionneurs - mais ne touche
// PAS `_testActive` / `_testStartTime`. Une session de test manuel ouverte juste
// avant survit donc au demarrage de la calibration, et son plafond
// TEST_SESSION_MAX_MS finit par echoir EN PLEINE MESURE :
// `endTestSession(true)` demande alors un panic, qui coupe la calibration.
//
// CE QUI EST TESTE ICI, ET CE QUI NE PEUT PAS L'ETRE
// --------------------------------------------------
// La DECISION est extraite dans `CalibrationGate`, pur : elle s'execute donc
// reellement dans ces tests. Le CABLAGE (quel booleen WebConfigurator passe a
// quel champ) vit dans un fichier qu'aucun build hote ne compile ; il est
// verrouille par une garde de source dans tests/test_static_audit.py, dont le
// role est precisement de completer ces tests-ci et non de les remplacer.
//
// Niveau de validation atteint : EXECUTE SUR HOTE. Rien n'a tourne sur ESP32.
#include <cassert>
#include <cstring>
#include "CalibrationGate.h"

namespace {

CalStartInputs calAllGood() {
  CalStartInputs in;
  in.calibratorPresent = true;
  in.micDetected = true;
  in.calibrationRunning = false;
  in.manualTestActive = false;
  return in;
}

void a_manual_test_session_refuses_the_calibration() {
  CalStartInputs in = calAllGood();
  in.manualTestActive = true;
  assert(calibrationStartVerdict(in) == CALSTART_MANUAL_TEST_ACTIVE);
  assert(strcmp(calibrationStartErrorCode(calibrationStartVerdict(in)),
                "manual_test_active") == 0);
}

void a_closed_manual_test_lets_the_calibration_start() {
  CalStartInputs in = calAllGood();
  assert(calibrationStartVerdict(in) == CALSTART_OK);
  // Et le code d'erreur d'un verdict OK est vide : rien a afficher.
  assert(calibrationStartErrorCode(CALSTART_OK)[0] == '\0');
}

// NON-REGRESSION : tant qu'aucune session manuelle n'est ouverte, les reponses
// sont exactement celles d'avant, dans le meme ordre. Un refus ajoute ne doit
// pas deplacer les refus existants - c'est ce qui rend ce changement sur pour
// l'interface deja ecrite.
void the_existing_refusals_are_unchanged_and_keep_their_order() {
  CalStartInputs in = calAllGood();
  in.calibratorPresent = false;
  assert(strcmp(calibrationStartErrorCode(calibrationStartVerdict(in)), "no_microphone") == 0);

  in = calAllGood();
  in.micDetected = false;
  assert(calibrationStartVerdict(in) == CALSTART_NO_MICROPHONE);

  in = calAllGood();
  in.calibrationRunning = true;
  assert(calibrationStartVerdict(in) == CALSTART_CALIBRATION_BUSY);

  // Micro absent ET calibration en cours : c'est le micro qui parle, comme
  // avant. Micro absent ET test manuel : le micro aussi.
  in = calAllGood();
  in.micDetected = false; in.calibrationRunning = true;
  assert(calibrationStartVerdict(in) == CALSTART_NO_MICROPHONE);

  in = calAllGood();
  in.micDetected = false; in.manualTestActive = true;
  assert(calibrationStartVerdict(in) == CALSTART_NO_MICROPHONE);

  // Calibration en cours ET test manuel : "busy" d'abord, l'ordre d'origine.
  in = calAllGood();
  in.calibrationRunning = true; in.manualTestActive = true;
  assert(calibrationStartVerdict(in) == CALSTART_CALIBRATION_BUSY);
}

// DEUX CLIENTS. Le verdict ne prend AUCUN identifiant de client, et c'est
// deliberé : la question n'est pas "qui demande" mais "les actionneurs
// sont-ils deja pris". Cabler ce champ sur `isTestOwner(clientId)` au lieu de
// `testSessionActive()` laisserait un SECOND navigateur demarrer une
// calibration pendant le test manuel du premier - le cas a deux clients. La
// structure d'entree rend ce cablage impossible a exprimer ici ; la garde de
// source verifie qu'il ne l'est pas non plus dans l'adaptateur.
void the_verdict_ignores_who_is_asking() {
  CalStartInputs owner = calAllGood();
  owner.manualTestActive = true;
  CalStartInputs other = owner;   // meme etat materiel, autre demandeur
  assert(calibrationStartVerdict(owner) == calibrationStartVerdict(other));
  assert(calibrationStartVerdict(other) == CALSTART_MANUAL_TEST_ACTIVE);
}

// Tout verdict de refus doit porter un code NON VIDE : un code vide ferait
// emettre `{"t":"acal_error","msg":""}`, que l'interface afficherait comme une
// erreur sans cause.
void every_refusal_has_a_non_empty_code() {
  const CalStartVerdict all[] = {
    CALSTART_NO_CALIBRATOR, CALSTART_NO_MICROPHONE,
    CALSTART_CALIBRATION_BUSY, CALSTART_MANUAL_TEST_ACTIVE,
  };
  for (CalStartVerdict v : all) {
    assert(v != CALSTART_OK);
    assert(calibrationStartErrorCode(v)[0] != '\0');
  }
}

}  // namespace

void fin2_calgate_run_all_tests() {
  a_manual_test_session_refuses_the_calibration();
  a_closed_manual_test_lets_the_calibration_start();
  the_existing_refusals_are_unchanged_and_keep_their_order();
  the_verdict_ignores_who_is_asking();
  every_refusal_has_a_non_empty_code();
}
