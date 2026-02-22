#include "FingerController.h"
#include "ConfigStorage.h"

FingerController::FingerController(Adafruit_PWMServoDriver& pwm)
  : _pwm(pwm) {
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
    if (openState == 2) travel = travel * cfg.halfHolePercent / 100;
    int16_t angle = baseAngle + travel;

    if (angle < SERVO_MIN_ANGLE) angle = SERVO_MIN_ANGLE;
    if (angle > SERVO_MAX_ANGLE) angle = SERVO_MAX_ANGLE;

    return (uint16_t)angle;
  }
}

void FingerController::setServoAngle(int fingerIndex, uint16_t angle) {
  int pcaChannel = cfg.fingers[fingerIndex].pcaChannel;
  uint16_t pwmValue = angleToPWM(angle);
  _pwm.setPWM(pcaChannel, 0, pwmValue);
}

void FingerController::testFingerAngle(int fingerIndex, uint16_t angle) {
  if (fingerIndex < 0 || fingerIndex >= cfg.numFingers) return;
  if (angle > SERVO_MAX_ANGLE) angle = SERVO_MAX_ANGLE;
  setServoAngle(fingerIndex, angle);

  if (DEBUG) {
    Serial.print("DEBUG: FingerController - Test doigt ");
    Serial.print(fingerIndex);
    Serial.print(" angle: ");
    Serial.println(angle);
  }
}

uint16_t FingerController::angleToPWM(uint16_t angle) {
  if (angle < SERVO_MIN_ANGLE) angle = SERVO_MIN_ANGLE;
  if (angle > SERVO_MAX_ANGLE) angle = SERVO_MAX_ANGLE;

  uint16_t pulse = map(angle, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE,
                       SERVO_PULSE_MIN, SERVO_PULSE_MAX);

  float pulseDuration = (float)pulse / 1000000.0;
  float pwmValue = pulseDuration * SERVO_FREQUENCY * 4096.0;

  return (uint16_t)(pwmValue + 0.5f);
}
