#include "FingerController.h"
#include "ConfigStorage.h"
#include "ServoMath.h"

FingerController::FingerController(PwmWriteFn writePwm)
  : _writePwm(writePwm) {
}

void FingerController::begin() {
  if (DEBUG) {
    Serial.println("DEBUG: FingerController - Initialisation");
    Serial.print("DEBUG:   - Nombre de doigts: ");
    Serial.println(cfg.numFingers);
    Serial.print("DEBUG:   - Nombre de notes: ");
    Serial.println(cfg.numNotes);
  }
  closeAllFingers();
}

void FingerController::setFingerPattern(const uint8_t pattern[MAX_FINGER_SERVOS]) {
  for (int i = 0; i < cfg.numFingers; i++) {
    uint16_t angle = calculateServoAngle(i, pattern[i]);
    setServoAngle(i, angle);
  }

  if (DEBUG) {
    Serial.print("DEBUG: FingerController - Pattern applique: ");
    for (int i = 0; i < cfg.numFingers; i++) {
      Serial.print(pattern[i]);
      Serial.print(" ");
    }
    Serial.println();
  }
}

void FingerController::setFingerPatternForNote(byte midiNote) {
  const NoteConfig* note = getNoteByMidi(midiNote);

  if (note == nullptr) {
    if (DEBUG) {
      Serial.print("DEBUG: FingerController - Note non trouvee: ");
      Serial.println(midiNote);
    }
    return;
  }

  setFingerPattern(note->fingerPattern);

  if (DEBUG) {
    Serial.print("DEBUG: FingerController - Note MIDI: ");
    Serial.println(midiNote);
  }
}

void FingerController::closeAllFingers() {
  for (int i = 0; i < cfg.numFingers; i++) {
    setServoAngle(i, cfg.fingers[i].closedAngle);
  }

  if (DEBUG) {
    Serial.println("DEBUG: FingerController - Tous les doigts fermes");
  }
}

void FingerController::openAllFingers() {
  for (int i = 0; i < cfg.numFingers; i++) {
    uint16_t openAngle = calculateServoAngle(i, 1);
    setServoAngle(i, openAngle);
  }

  if (DEBUG) {
    Serial.println("DEBUG: FingerController - Tous les doigts ouverts");
  }
}

uint16_t FingerController::calculateServoAngle(int fingerIndex, uint8_t openState) {
  uint16_t baseAngle = cfg.fingers[fingerIndex].closedAngle;

  if (openState == 0) {
    return baseAngle;
  } else {
    // 1=full open, 2=half open (configurable % of full angle)
    int16_t travel = cfg.fingerAngleOpen * cfg.fingers[fingerIndex].direction;
    if (openState == 2) {
      uint8_t hp = cfg.fingers[fingerIndex].halfPercent > 0
                   ? cfg.fingers[fingerIndex].halfPercent
                   : cfg.halfHolePercent;
      travel = travel * hp / 100;
    }
    int16_t angle = baseAngle + travel;

    if (angle < SERVO_MIN_ANGLE) angle = SERVO_MIN_ANGLE;
    if (angle > SERVO_MAX_ANGLE) angle = SERVO_MAX_ANGLE;

    return (uint16_t)angle;
  }
}

void FingerController::setServoAngle(int fingerIndex, uint16_t angle) {
  int pcaChannel = cfg.fingers[fingerIndex].pcaChannel;
  uint16_t pwmValue = servoAngleToPWM(angle);
  _writePwm(pcaChannel, 0, pwmValue);
}

/*----------------------------------------------------------------------------
 * Borne de la commande de reglage manuel d'un doigt
 *
 * Ce qu'elle empeche : {"t":"test_finger","i":0,"a":180} ne passait que par
 * SERVO_MAX_ANGLE, c'est-a-dire par la course ELECTRIQUE du servo. Sur la
 * configuration expediee (closedAngle 90, direction -1, fingerAngleOpen 30),
 * la course mecanique du doigt va de 60 a 90 deg : le curseur "Closed angle"
 * de l'interface, qui envoie sa valeur a chaque mouvement, commandait jusqu'a
 * 90 deg au-dela de la butee. Le servo y restait, a son courant de calage.
 *
 * La marge d'exploration existe parce que regler closedAngle sert justement a
 * TROUVER la bonne position, donc a sortir de celle qui est enregistree ; son
 * dimensionnement est argumente sur SERVO_TEST_MARGIN_DEG dans settings.h.
 *
 * Le firmware borne EN PREMIER : le WebSocket est ouvert a n'importe quel
 * client, pas seulement a la page servie par la carte.
 *--------------------------------------------------------------------------*/
uint16_t FingerController::clampFingerTestAngle(int fingerIndex, uint16_t angle) const {
  const FingerConfig& f = cfg.fingers[fingerIndex];
  // La course declaree : du doigt ferme au doigt pleinement ouvert. `direction`
  // vaut +-1 (le validateur le garantit), donc l'ouverture peut etre au-dessus
  // comme au-dessous de la position fermee.
  int32_t closed = (int32_t)f.closedAngle;
  int32_t opened = closed + (int32_t)cfg.fingerAngleOpen * (int32_t)f.direction;
  int32_t lo = closed < opened ? closed : opened;
  int32_t hi = closed < opened ? opened : closed;

  lo -= SERVO_TEST_MARGIN_DEG;
  hi += SERVO_TEST_MARGIN_DEG;
  if (lo < SERVO_MIN_ANGLE) lo = SERVO_MIN_ANGLE;
  if (hi > SERVO_MAX_ANGLE) hi = SERVO_MAX_ANGLE;

  if ((int32_t)angle < lo) return (uint16_t)lo;
  if ((int32_t)angle > hi) return (uint16_t)hi;
  return angle;
}

void FingerController::testFingerAngle(int fingerIndex, uint16_t angle) {
  if (fingerIndex < 0 || fingerIndex >= cfg.numFingers) return;
  angle = clampFingerTestAngle(fingerIndex, angle);
  setServoAngle(fingerIndex, angle);

  if (DEBUG) {
    Serial.print("DEBUG: FingerController - Test doigt ");
    Serial.print(fingerIndex);
    Serial.print(" angle: ");
    Serial.println(angle);
  }
}

