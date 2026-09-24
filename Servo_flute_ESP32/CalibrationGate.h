/***********************************************************************************************
 * CalibrationGate - a-t-on le droit de DEMARRER une auto-calibration ?
 *
 * POURQUOI UN MODULE SEPARE
 * -------------------------
 * La decision vivait en ligne dans `WebConfigurator::executeWebOp()`, un fichier
 * qu'aucun build hote ne compile (ESPAsyncWebServer, ArduinoJson, LittleFS).
 * Elle n'etait donc verifiable que par relecture. Ici elle est PURE - trois
 * booleens en entree, un verdict en sortie - donc executable sur hote, et
 * `WebConfigurator` n'en est plus que l'adaptateur.
 *
 * LE DEFAUT QU'ELLE FERME
 * -----------------------
 * Une session de test manuel (`_testActive`) et une auto-calibration pouvaient
 * coexister. `cancelActiveActuatorSession()`, appelee au demarrage de la
 * calibration, arrete le calibrateur et relache la session d'actionneurs mais
 * ne touche PAS `_testActive` / `_testStartTime`. Consequence, sur un banc
 * reel :
 *
 *   t0      l'operateur bouge un curseur -> session de test manuel ouverte
 *   t0+dt   il lance l'auto-calibration -> elle demarre et prend les actionneurs
 *   t0+30s  update() voit testSessionExpired() -> endTestSession(true)
 *           -> requestPanic() EN PLEINE MESURE
 *
 * La calibration est alors coupee au milieu (le panic fait bouger
 * `panicCount()`, ce qui declenche `requestCalibrationCancel()` au tour
 * suivant), et l'operateur ne recoit qu'un `test_expired` sans rapport apparent.
 * Les actionneurs finissent en securite - ce n'est pas un defaut de surete -
 * mais une campagne de mesure est perdue sans explication lisible.
 *
 * LE CHOIX : REFUSER, PAS VOLER
 * -----------------------------
 * Effacer `_testActive` au demarrage de la calibration reviendrait a desarmer
 * le filet de TEST_SESSION_MAX_MS sans mise en securite - exactement le defaut
 * corrige a la passe precedente sur `endTestSession()`. La calibration est donc
 * REFUSEE tant qu'une session manuelle est ouverte ; l'operateur l'arrete, ou
 * laisse son plafond la fermer.
 *
 * Note sur la session EXPIREE MAIS PAS ENCORE MOISSONNEE : `serviceWsOps()`
 * s'execute AVANT le controle de `testSessionExpired()` dans `update()`. Une
 * session dont le plafond vient d'echoir est donc encore vue active ici, et la
 * calibration est refusee. C'est le comportement voulu : c'est la moisson qui
 * remet le materiel au repos, et elle n'a pas encore eu lieu. Le tour suivant,
 * la session est fermee et la meme demande passe.
 ***********************************************************************************************/
#ifndef CALIBRATION_GATE_H
#define CALIBRATION_GATE_H

#include <stdint.h>

enum CalStartVerdict : uint8_t {
  CALSTART_OK = 0,
  CALSTART_NO_CALIBRATOR,       // pas de calibrateur instancie (build sans micro)
  CALSTART_NO_MICROPHONE,       // micro absent ou non detecte
  CALSTART_CALIBRATION_BUSY,    // une calibration tourne deja
  CALSTART_MANUAL_TEST_ACTIVE,  // une session de test manuel possede les actionneurs
};

struct CalStartInputs {
  bool calibratorPresent;
  bool micDetected;
  bool calibrationRunning;
  bool manualTestActive;
};

// Verdict d'une demande de demarrage. L'ORDRE des refus est stable et teste :
// les trois premiers sont ceux qui existaient deja, dans leur ordre d'origine,
// de sorte qu'aucune reponse ne change quand aucune session manuelle n'est
// ouverte. `manual_test_active` s'ajoute en dernier - il n'y a pas d'enjeu de
// surete dans cet ordre (un refus est un refus), seulement de non-regression.
CalStartVerdict calibrationStartVerdict(const CalStartInputs& in);

// Code d'erreur a renvoyer au client. Chacun DOIT avoir un libelle dans
// acalErrText() de web_content.h, sans quoi l'interface afficherait le code brut.
const char* calibrationStartErrorCode(CalStartVerdict v);

#endif
