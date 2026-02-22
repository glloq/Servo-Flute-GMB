#include "AirflowController.h"
#include "ConfigStorage.h"

// Lookup table pour sin() - 256 entrees pour une periode complete [0, 2pi]
// Valeurs: -127 a +127 (represente -1.0 a +1.0)
// Note ESP32 : PROGMEM est ignore (flash mappe en memoire) mais compile sans erreur
const int8_t SIN_LUT[256] PROGMEM = {
  0, 3, 6, 9, 12, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46,
  49, 51, 54, 57, 60, 63, 65, 68, 71, 73, 76, 78, 81, 83, 85, 88,
  90, 92, 94, 96, 98, 100, 102, 104, 106, 107, 109, 111, 112, 113, 115, 116,
  117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127,
  127, 127, 127, 127, 126, 126, 126, 125, 125, 124, 123, 122, 122, 121, 120, 118,
  117, 116, 115, 113, 112, 111, 109, 107, 106, 104, 102, 100, 98, 96, 94, 92,
  90, 88, 85, 83, 81, 78, 76, 73, 71, 68, 65, 63, 60, 57, 54, 51,
  49, 46, 43, 40, 37, 34, 31, 28, 25, 22, 19, 16, 12, 9, 6, 3,
  0, -3, -6, -9, -12, -16, -19, -22, -25, -28, -31, -34, -37, -40, -43, -46,
  -49, -51, -54, -57, -60, -63, -65, -68, -71, -73, -76, -78, -81, -83, -85, -88,
  -90, -92, -94, -96, -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
  -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
  -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
  -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100, -98, -96, -94, -92,
  -90, -88, -85, -83, -81, -78, -76, -73, -71, -68, -65, -63, -60, -57, -54, -51,
  -49, -46, -43, -40, -37, -34, -31, -28, -25, -22, -19, -16, -12, -9, -6, -3
};

// Fonction helper pour lookup rapide sin()
// Sur ESP32, pgm_read_byte() est un simple acces memoire (pas de flash separation)
inline float fastSin(unsigned long timeMs, float frequency) {
  unsigned long period = (unsigned long)(1000.0 / frequency);
  unsigned long phase = timeMs % period;
  uint8_t index = (uint8_t)((phase * 256UL) / period);

  return pgm_read_byte(&SIN_LUT[index]) / SIN_LUT_SCALE;
}

AirflowController::AirflowController(Adafruit_PWMServoDriver& pwm)
  : _pwm(pwm), _solenoidOpen(false), _solenoidOpenTime(0),
    _ccVolume(cfg.ccVolumeDefault), _ccExpression(cfg.ccExpressionDefault),
    _ccModulation(cfg.ccModulationDefault),
    _ccBreath(cfg.ccBreathDefault),
    _cc2BufferIndex(0), _cc2BufferCount(0), _lastCC2Time(0), _lastVelocity(64),
    _baseAngleWithoutVibrato(cfg.servoAirflowOff), _vibratoActive(false),
    _currentMinAngle(cfg.servoAirflowMin), _currentMaxAngle(cfg.servoAirflowMax),
    _attackActive(false), _attackStartTime(0), _attackStartAngle(0), _attackTargetAngle(0) {
  for (uint8_t i = 0; i < CC2_SMOOTHING_BUFFER_SIZE; i++) {
    _cc2SmoothingBuffer[i] = cfg.ccBreathDefault;
  }
}

void AirflowController::begin() {
  pinMode(cfg.solenoidPin, OUTPUT);
  closeSolenoid();
  setAirflowToRest();

  if (DEBUG) {
    Serial.println("DEBUG: AirflowController - Initialisation");
    #if SOLENOID_USE_PWM
    Serial.println("DEBUG: AirflowController - Mode PWM active");
    Serial.print("DEBUG:   - PWM activation: ");
    Serial.println(cfg.solenoidPwmActivation);
    Serial.print("DEBUG:   - PWM maintien: ");
    Serial.println(cfg.solenoidPwmHolding);
    #else
    Serial.println("DEBUG: AirflowController - Mode GPIO simple");
    #endif
  }
}

void AirflowController::setAirflowVelocity(byte velocity) {
  uint16_t angle;

  if (velocity == 0) {
    angle = cfg.servoAirflowOff;
  } else {
    angle = map(velocity, 1, MIDI_VELOCITY_MAX, cfg.servoAirflowMin, cfg.servoAirflowMax);
  }

  setAirflowServoAngle(angle);

  if (DEBUG) {
    Serial.print("DEBUG: AirflowController - Velocite: ");
    Serial.print(velocity);
    Serial.print(" -> Angle: ");
    Serial.println(angle);
  }
}

void AirflowController::setAirflowForNote(byte midiNote, byte velocity) {
  const NoteConfig* note = getNoteByMidi(midiNote);

  uint16_t minAngle, maxAngle;
  uint16_t baseAngle;

  if (velocity == 0) {
    setAirflowServoAngle(cfg.servoAirflowOff);
    return;
  }

  if (note != nullptr) {
    uint8_t airMin = note->airflowMinPercent;
    uint8_t airMax = note->airflowMaxPercent;
    minAngle = cfg.servoAirflowMin + ((cfg.servoAirflowMax - cfg.servoAirflowMin) * airMin / 100);
    maxAngle = cfg.servoAirflowMin + ((cfg.servoAirflowMax - cfg.servoAirflowMin) * airMax / 100);
  } else {
    minAngle = cfg.servoAirflowMin;
    maxAngle = cfg.servoAirflowMax;
  }

  _currentMinAngle = minAngle;
  _currentMaxAngle = maxAngle;

  // Stocker velocity pour fallback si CC2 timeout
  _lastVelocity = velocity;

  // 1. CC7 (Volume) reduit la limite haute
  float volumeFactor = _ccVolume / (float)MIDI_CC_MAX;
  uint16_t effectiveMaxAngle = minAngle + (maxAngle - minAngle) * volumeFactor;

  // 2. Determiner source airflow : CC2 ou velocity
  byte airflowSource;

  if (cfg.cc2Enabled) {
    if (_cc2BufferCount > 0) {
      unsigned long timeSinceCC2 = millis() - _lastCC2Time;
      if (cfg.cc2TimeoutMs > 0 && timeSinceCC2 > cfg.cc2TimeoutMs) {
        airflowSource = velocity;
        if (DEBUG) {
          Serial.print("DEBUG: CC2 timeout (");
          Serial.print(timeSinceCC2);
          Serial.print("ms) - Fallback velocity: ");
          Serial.println(velocity);
        }
      } else {
        uint16_t sum = 0;
        for (uint8_t i = 0; i < _cc2BufferCount; i++) {
          sum += _cc2SmoothingBuffer[i];
        }
        byte smoothedCC2 = sum / _cc2BufferCount;

        if (smoothedCC2 < cfg.cc2SilenceThreshold) {
          airflowSource = 0;
        } else {
          float normalizedCC2 = smoothedCC2 / (float)MIDI_CC_MAX;
          float curvedCC2 = pow(normalizedCC2, cfg.cc2ResponseCurve);
          airflowSource = (byte)(curvedCC2 * MIDI_CC_MAX);
        }
      }
    } else {
      airflowSource = velocity;
    }
  } else {
    airflowSource = velocity;
  }

  if (airflowSource == 0) {
    setAirflowServoAngle(cfg.servoAirflowOff);
    closeSolenoid();
    return;
  }

  // 3. Airflow source definit l'angle de base (module par velocityResponse)
  {
    float vr = cfg.airVelocityResponse / 100.0f;
    // vr=0 : angle fixe au milieu de la plage (velocite ignoree)
    // vr=100 : plage complete selon velocite/CC2
    float midAngle = minAngle + (effectiveMaxAngle - minAngle) * 0.5f;
    float rawAngle = map(airflowSource, 1, MIDI_CC_MAX, minAngle, effectiveMaxAngle);
    baseAngle = (uint16_t)(midAngle + (rawAngle - midAngle) * vr + 0.5f);
  }

  // 4. CC11 (Expression) module dans la plage
  float expressionFactor = _ccExpression / (float)MIDI_CC_MAX;
  float finalAngleWithoutVibrato = minAngle + (baseAngle - minAngle) * expressionFactor;

  // 5. Limiter
  if (finalAngleWithoutVibrato < cfg.servoAirflowMin) finalAngleWithoutVibrato = cfg.servoAirflowMin;
  if (finalAngleWithoutVibrato > cfg.servoAirflowMax) finalAngleWithoutVibrato = cfg.servoAirflowMax;

  _baseAngleWithoutVibrato = (uint16_t)(finalAngleWithoutVibrato + 0.5);
  _attackTargetAngle = _baseAngleWithoutVibrato;
  _vibratoActive = (_ccModulation > 0);

  // 6. Mode d'attaque (accent / crescendo)
  _attackActive = false;
  if (cfg.airAttackMode != 0 && cfg.airAttackMs > 0 && cfg.airAttackOffset > 0) {
    int16_t offsetDeg = (int16_t)((_attackTargetAngle - minAngle) * cfg.airAttackOffset / 100);
    if (offsetDeg < 1) offsetDeg = 1;
    if (cfg.airAttackMode == 1) {
      // Accent: demarre plus fort (au-dessus de la cible)
      _attackStartAngle = _attackTargetAngle + offsetDeg;
      if (_attackStartAngle > cfg.servoAirflowMax) _attackStartAngle = cfg.servoAirflowMax;
    } else {
      // Crescendo: demarre plus faible (en-dessous de la cible)
      _attackStartAngle = (_attackTargetAngle > offsetDeg) ? _attackTargetAngle - offsetDeg : minAngle;
    }
    _attackStartTime = millis();
    _attackActive = true;
  }

  if (DEBUG) {
    Serial.print("DEBUG: AirflowController - Note: ");
    Serial.print(midiNote);
    Serial.print(" | Vel: ");
    Serial.print(velocity);
    Serial.print(" | Range: ");
    Serial.print(minAngle);
    Serial.print("-");
    Serial.print(maxAngle);
    Serial.print(" | Final: ");
    Serial.print(_baseAngleWithoutVibrato);
    if (_attackActive) {
      Serial.print(" | Atk: ");
      Serial.print(_attackStartAngle);
      Serial.print("->target ");
      Serial.print(cfg.airAttackMs);
      Serial.print("ms");
    }
    Serial.println("deg");
  }

  if (_attackActive) {
    // Positionner a l'angle de depart, update() fera la transition
    setAirflowServoAngle(_attackStartAngle);
  } else if (_vibratoActive) {
    update();
  } else {
    setAirflowServoAngle(_baseAngleWithoutVibrato);
  }
}

void AirflowController::openValve() {
  if (cfg.valveUseServo) {
    // Mode servo-valve : ouvrir via PCA9685
    setValveServoAngle(true);
  } else {
    // Mode solenoide classique
    #if SOLENOID_USE_PWM
      setSolenoidPWM(cfg.solenoidPwmActivation);
      _solenoidOpenTime = millis();
    #else
      if (SOLENOID_ACTIVE_HIGH) {
        digitalWrite(cfg.solenoidPin, HIGH);
      } else {
        digitalWrite(cfg.solenoidPin, LOW);
      }
    #endif
  }

  _solenoidOpen = true;

  if (DEBUG) {
    Serial.print("DEBUG: AirflowController - Valve OUVERTE (");
    Serial.print(cfg.valveUseServo ? "servo" : "solenoide");
    Serial.println(")");
  }
}

void AirflowController::closeValve() {
  if (cfg.valveUseServo) {
    setValveServoAngle(false);
  } else {
    #if SOLENOID_USE_PWM
      setSolenoidPWM(0);
    #else
      if (SOLENOID_ACTIVE_HIGH) {
        digitalWrite(cfg.solenoidPin, LOW);
      } else {
        digitalWrite(cfg.solenoidPin, HIGH);
      }
    #endif
  }

  _solenoidOpen = false;
  _solenoidOpenTime = 0;

  if (DEBUG) {
    Serial.print("DEBUG: AirflowController - Valve FERMEE (");
    Serial.print(cfg.valveUseServo ? "servo" : "solenoide");
    Serial.println(")");
  }
}

bool AirflowController::isValveOpen() const {
  return _solenoidOpen;
}

void AirflowController::setValveServoAngle(bool open) {
  // Servo-valve : 0° = ferme, 90° = ouvert
  uint16_t angle = open ? 90 : 0;
  uint16_t pwmValue = angleToPWM(angle);
  _pwm.setPWM(cfg.valveServoPcaChannel, 0, pwmValue);
}

void AirflowController::setAirflowToRest() {
  setAirflowServoAngle(cfg.servoAirflowOff);
}

void AirflowController::update() {
  #if SOLENOID_USE_PWM
  if (_solenoidOpen && _solenoidOpenTime > 0) {
    unsigned long elapsed = millis() - _solenoidOpenTime;
    if (elapsed >= cfg.solenoidActivationTimeMs) {
      setSolenoidPWM(cfg.solenoidPwmHolding);
      _solenoidOpenTime = 0;
    }
  }
  #endif

  // Transition d'attaque (accent / crescendo)
  uint16_t currentBase = _baseAngleWithoutVibrato;
  if (_attackActive && _solenoidOpen) {
    unsigned long elapsed = millis() - _attackStartTime;
    if (elapsed >= cfg.airAttackMs) {
      _attackActive = false;
      currentBase = _attackTargetAngle;
    } else {
      float t = (float)elapsed / cfg.airAttackMs;
      currentBase = _attackStartAngle + (int16_t)((_attackTargetAngle - _attackStartAngle) * t);
    }
  }

  // Appliquer vibrato si actif
  if ((_vibratoActive && _ccModulation > 0 && _solenoidOpen) || _attackActive) {
    float vibratoAmplitude = (_vibratoActive && _ccModulation > 0) ?
      (_ccModulation / (float)MIDI_CC_MAX) * cfg.vibratoMaxAmplitudeDeg : 0;
    float vibratoOffset = vibratoAmplitude > 0 ?
      fastSin(millis(), cfg.vibratoFrequencyHz) * vibratoAmplitude : 0;

    int16_t finalAngle = currentBase + (int16_t)(vibratoOffset + 0.5);

    if (finalAngle < (int16_t)_currentMinAngle) finalAngle = _currentMinAngle;
    if (finalAngle > (int16_t)_currentMaxAngle) finalAngle = _currentMaxAngle;

    setAirflowServoAngle((uint16_t)finalAngle);
  }
}

void AirflowController::setAirflowLivePercent(uint8_t percent) {
  if (percent > 100) percent = 100;
  uint16_t angle = cfg.servoAirflowMin + ((cfg.servoAirflowMax - cfg.servoAirflowMin) * percent / 100);
  _baseAngleWithoutVibrato = angle;
  setAirflowServoAngle(angle);
}

void AirflowController::testAirflowAngle(uint16_t angle) {
  if (angle > SERVO_MAX_ANGLE) angle = SERVO_MAX_ANGLE;
  setAirflowServoAngle(angle);

  if (DEBUG) {
    Serial.print("DEBUG: AirflowController - Test angle: ");
    Serial.println(angle);
  }
}

void AirflowController::testSolenoid(bool open) {
  if (open) {
    openSolenoid();
  } else {
    closeSolenoid();
  }
}

void AirflowController::setAirflowServoAngle(uint16_t angle) {
  uint16_t pwmValue = angleToPWM(angle);
  _pwm.setPWM(cfg.airflowPcaChannel, 0, pwmValue);
}

uint16_t AirflowController::angleToPWM(uint16_t angle) {
  if (angle < SERVO_MIN_ANGLE) angle = SERVO_MIN_ANGLE;
  if (angle > SERVO_MAX_ANGLE) angle = SERVO_MAX_ANGLE;

  uint16_t pulse = map(angle, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE,
                       SERVO_PULSE_MIN, SERVO_PULSE_MAX);

  float pulseDuration = (float)pulse / 1000000.0;
  float pwmValue = pulseDuration * SERVO_FREQUENCY * 4096.0;

  return (uint16_t)(pwmValue + 0.5f);
}

void AirflowController::setSolenoidPWM(uint8_t pwmValue) {
  #if SOLENOID_USE_PWM
    // ESP32 : analogWrite() est supporte depuis Arduino core 3.x
    if (SOLENOID_ACTIVE_HIGH) {
      analogWrite(cfg.solenoidPin, pwmValue);
    } else {
      analogWrite(cfg.solenoidPin, PWM_MAX_VALUE - pwmValue);
    }
  #endif
}

void AirflowController::setCCValues(byte ccVolume, byte ccExpression, byte ccModulation) {
  _ccVolume = ccVolume;
  _ccExpression = ccExpression;
  _ccModulation = ccModulation;
}

void AirflowController::setCC73Attack(byte ccValue) {
  // 0-42 = stable, 43-84 = accent, 85-127 = crescendo
  // La position dans chaque plage definit l'intensite (offset %)
  if (ccValue <= 42) {
    cfg.airAttackMode = 0;  // Stable
    cfg.airAttackOffset = 0;
  } else if (ccValue <= 84) {
    cfg.airAttackMode = 1;  // Accent
    cfg.airAttackOffset = 5 + (ccValue - 43) * 45 / 41;  // 5-50%
  } else {
    cfg.airAttackMode = 2;  // Crescendo
    cfg.airAttackOffset = 5 + (ccValue - 85) * 45 / 42;  // 5-50%
  }

  if (DEBUG) {
    const char* modes[] = {"Stable", "Accent", "Crescendo"};
    Serial.print("DEBUG: CC73 Attack -> ");
    Serial.print(modes[cfg.airAttackMode]);
    Serial.print(" offset=");
    Serial.print(cfg.airAttackOffset);
    Serial.println("%");
  }
}

void AirflowController::updateCC2Breath(byte ccBreath) {
  if (!cfg.cc2Enabled) return;

  _cc2SmoothingBuffer[_cc2BufferIndex] = ccBreath;
  _cc2BufferIndex = (_cc2BufferIndex + 1) % CC2_SMOOTHING_BUFFER_SIZE;
  if (_cc2BufferCount < CC2_SMOOTHING_BUFFER_SIZE) {
    _cc2BufferCount++;
  }
  _lastCC2Time = millis();
  _ccBreath = ccBreath;

  if (DEBUG) {
    Serial.print("DEBUG: CC2 (Breath) recu: ");
    Serial.print(ccBreath);
    Serial.print(" | Buffer: ");
    Serial.print(_cc2BufferCount);
    Serial.print("/");
    Serial.println(CC2_SMOOTHING_BUFFER_SIZE);
  }
}
