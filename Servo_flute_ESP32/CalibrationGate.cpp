#include "CalibrationGate.h"

CalStartVerdict calibrationStartVerdict(const CalStartInputs& in) {
  if (!in.calibratorPresent) return CALSTART_NO_CALIBRATOR;
  if (!in.micDetected)       return CALSTART_NO_MICROPHONE;
  if (in.calibrationRunning) return CALSTART_CALIBRATION_BUSY;
  // Une session de test manuel possede deja les actionneurs ET porte son propre
  // filet de securite temporel. Demarrer par-dessus laisserait ce filet se
  // declencher au milieu de la mesure et paniquer l'instrument.
  if (in.manualTestActive)   return CALSTART_MANUAL_TEST_ACTIVE;
  return CALSTART_OK;
}

const char* calibrationStartErrorCode(CalStartVerdict v) {
  switch (v) {
    case CALSTART_NO_CALIBRATOR:      return "no_microphone";
    case CALSTART_NO_MICROPHONE:      return "no_microphone";
    case CALSTART_CALIBRATION_BUSY:   return "calibration_busy";
    case CALSTART_MANUAL_TEST_ACTIVE: return "manual_test_active";
    case CALSTART_OK:                 return "";
  }
  return "";
}
