#include "PressureController.h"
#include "ConfigStorage.h"
#include <Wire.h>

// Les acces bas niveau aux capteurs ToF (VL53L0X / VL6180X) vivent desormais
// dans TofSensor : identification du composant, initialisation complete, mesure
// single-shot non bloquante, et distinction explicite entre "present sur le bus",
// "initialise", "mesure valide" et "en erreur".

PressureController::PressureController()
  : _sensorDetected(false), _sensorType(0),
    _distanceMm(0), _hallValue(0), _endstopActive(false), _fillPercent(0),
    _tofRangeStartTime(0), _measurementValid(false),
    _lastValidReadTime(0), _tofErrorCount(0),
    _targetPercent(0), _currentPumpPwm(0),
    _enabled(true), _testPumpIndex(-1), _testPumpPercent(0),
    _activePumpCount(0), _bangbangPumpOn(false),
    _pidIntegral(0),
    _lastPidTime(0), _lastReadTime(0) {
  for (uint8_t i = 0; i < MAX_PUMPS; i++) {
    _pumpActive[i] = false;
    _pumpActivateTime[i] = 0;
  }
}

bool PressureController::begin() {
  _sensorType = cfg.sensorType;

  // Configurer pins pompes
  if (cfg.airMode >= AIR_MODE_PUMP_VALVE) {
    for (uint8_t i = 0; i < cfg.numPumps && i < MAX_PUMPS; i++) {
      pinMode(cfg.pumpPins[i], OUTPUT);
      if (cfg.motorType == MOTOR_TYPE_ONOFF) {
        digitalWrite(cfg.pumpPins[i], LOW);
      } else {
        analogWrite(cfg.pumpPins[i], 0);
      }
    }
    if (DEBUG) {
      Serial.print("DEBUG: PressureController - Moteur ");
      Serial.println(cfg.motorType == MOTOR_TYPE_ONOFF ? "On/Off" : "PWM");
    }
  }

  // Mode pompe directe (sans reservoir) - pas de capteur a configurer
  if (cfg.airMode != AIR_MODE_PUMP_RESERVOIR) {
    if (DEBUG) {
      Serial.println("DEBUG: PressureController - Mode pompe directe (sans capteur)");
    }
    return false;
  }

  // Mode reservoir: configurer selon type de capteur
  if (_sensorType == SENSOR_TYPE_ENDSTOP_MECH || _sensorType == SENSOR_TYPE_ENDSTOP_OPT) {
    // Endstop mecanique ou optique: entree digitale
    pinMode(cfg.endstopPin, INPUT_PULLUP);
    _sensorDetected = true;
    if (DEBUG) {
      Serial.print("DEBUG: PressureController - ");
      Serial.print(_sensorType == SENSOR_TYPE_ENDSTOP_MECH ? "Endstop mecanique" : "Endstop optique");
      Serial.print(" GPIO ");
      Serial.print(cfg.endstopPin);
      Serial.println(cfg.endstopActiveHigh ? " (actif HIGH)" : " (actif LOW)");
    }
    return true;
  }

  if (_sensorType == SENSOR_TYPE_HALL_KY024) {
    // Capteur effet Hall KY-024: entree analogique
    pinMode(cfg.hallPin, INPUT);
    _sensorDetected = true;
    if (DEBUG) {
      Serial.print("DEBUG: PressureController - Hall KY-024 GPIO ");
      Serial.print(cfg.hallPin);
      Serial.print(" seuils ");
      Serial.print(cfg.hallThresholdLow);
      Serial.print("-");
      Serial.println(cfg.hallThresholdHigh);
    }
    return true;
  }

  // Capteurs ToF (VL53L0X / VL6180X) : identification + initialisation complete.
  // Le simple fait qu'un composant acquitte a 0x29 ne suffit plus a declarer le
  // capteur fonctionnel : TofSensor verifie le Model ID puis deroule la sequence
  // d'initialisation (SPAD de reference, tuning, calibrations). Sans cela les
  // distances lues n'ont aucune signification metrique.
  bool initialized = _tof.begin(_sensorType == SENSOR_TYPE_TOF_VL6180X ? TOF_MODEL_VL6180X
                                                                       : TOF_MODEL_VL53L0X);
  _sensorDetected = initialized;
  if (!initialized && DEBUG) {
    Serial.print("ERREUR: PressureController - capteur ToF inutilisable (");
    Serial.print(_tof.stateName());
    Serial.println(") : la pompe restera coupee en mode reservoir");
  }
  return initialized;
}

bool PressureController::usesTofSensor() const {
  return _sensorType == SENSOR_TYPE_TOF_VL53L0X || _sensorType == SENSOR_TYPE_TOF_VL6180X;
}

bool PressureController::isMeasurementStale() const {
  // "Perimee" = aucune mesure VALIDE depuis TOF_STALE_MS, que la derniere
  // tentative ait echoue ou que le capteur se soit simplement fige.
  if (!usesTofSensor()) return false;
  return (millis() - _lastValidReadTime) >= TOF_STALE_MS;
}

bool PressureController::serviceTofMeasurement() {
  if (!_sensorDetected || !_tof.isInitialized()) return false;
  unsigned long now = millis();

  if (!_tof.isRanging()) {
    // Start a new single-shot only at the configured read cadence.
    if (now - _lastReadTime < PRESSURE_READ_INTERVAL_MS) return false;
    _lastReadTime = now;
    if (!_tof.startMeasurement()) return false;
    _tofRangeStartTime = now;
    return false;
  }

  // Ranging in progress: poll the status register ONCE per call (no busy-wait,
  // so MIDI / WebSocket / audio / servos are not stalled up to 50 ms).
  if (_tof.pollMeasurement()) {
    _distanceMm = _tof.distanceMm();
    _measurementValid = true;
    _lastValidReadTime = now;
    _tofErrorCount = 0;
    return true;
  }

  // Pas encore prete OU mesure rendue mais rejetee par le capteur : on abandonne
  // seulement au-dela du timeout. Un timeout ne lit jamais une valeur bidon ; la
  // mesure est marquee invalide et les echecs repetes invalident le capteur.
  if (!_tof.isRanging()) {
    // Le capteur a rendu une mesure invalide (range status != 11).
    _measurementValid = false;
    if (_tofErrorCount < 0xFFFF) _tofErrorCount++;
    if (_tofErrorCount >= TOF_MAX_CONSEC_ERRORS) {
      _tof.reportTimeout(1);
      _sensorDetected = false;
    }
    return false;
  }
  if (now - _tofRangeStartTime >= TOF_RANGE_TIMEOUT_MS) {
    _measurementValid = false;
    if (_tofErrorCount < 0xFFFF) _tofErrorCount++;
    _tof.abortMeasurement();
    if (_tofErrorCount >= TOF_MAX_CONSEC_ERRORS) {
      _tof.reportTimeout(1);
      _sensorDetected = false;
    }
  }
  return false;
}

void PressureController::update() {
  if (cfg.airMode < AIR_MODE_PUMP_VALVE) return;

  // Manual single-pump test overrides normal control: drive ONLY the tested pump
  // so a per-pump test never commands the others.
  if (_testPumpIndex >= 0) {
    uint8_t raw = (uint16_t)_testPumpPercent * 255 / 100;
    _activePumpCount = 0;
    for (uint8_t i = 0; i < cfg.numPumps && i < MAX_PUMPS; i++) {
      bool on = (i == _testPumpIndex);
      writePumpHw(i, on ? raw : 0);
      _pumpActive[i] = (on && raw > 0);
      if (_pumpActive[i]) _activePumpCount++;
    }
    _currentPumpPwm = raw;
    return;
  }

  // Global mute: keep the pump off regardless of the requested target.
  if (!_enabled) {
    setPumpPwm(0);
    _bangbangPumpOn = false;
    return;
  }

  unsigned long now = millis();

  // Mode pompe directe (sans reservoir). setPumpPwm() accepts a logical raw demand
  // (0..255) and performs the physical min/max conversion exactly once.
  if (cfg.airMode != AIR_MODE_PUMP_RESERVOIR) {
    setPumpPwm((uint16_t)_targetPercent * 255 / 100);
    return;
  }

  // Reservoir mode must not silently degrade to direct mode if the sensor is absent.
  if (!_sensorDetected) {
    setPumpPwm(0);
    _bangbangPumpOn = false;
    _pidIntegral = 0;
    return;
  }

  // --- Mode reservoir avec capteur ---

  // Endstop (mecanique ou optique): controle ON/OFF simple
  // endstopPumpOn: false = pompe ON quand capteur inactif (remplir), true = pompe ON quand capteur actif (vider)
  if (_sensorType == SENSOR_TYPE_ENDSTOP_MECH || _sensorType == SENSOR_TYPE_ENDSTOP_OPT) {
    _endstopActive = (digitalRead(cfg.endstopPin) == (cfg.endstopActiveHigh ? HIGH : LOW));
    bool shouldPump = cfg.endstopPumpOn ? _endstopActive : !_endstopActive;
    if (_targetPercent == 0 || !shouldPump) {
      setPumpPwm(0);
      _bangbangPumpOn = false;
      _fillPercent = _endstopActive ? 100 : 0;
    } else {
      _bangbangPumpOn = true;
      setPumpPwm(255);
      _fillPercent = cfg.endstopPumpOn ? 100 : 0;
    }
    return;
  }

  // Hall effect: lecture analogique periodique
  if (_sensorType == SENSOR_TYPE_HALL_KY024) {
    if (now - _lastReadTime >= PRESSURE_READ_INTERVAL_MS) {
      _hallValue = analogRead(cfg.hallPin);
      _lastReadTime = now;
      if (_hallValue <= cfg.hallThresholdLow) {
        _fillPercent = 0;
      } else if (_hallValue >= cfg.hallThresholdHigh) {
        _fillPercent = 100;
      } else {
        uint16_t hallSpan = cfg.hallThresholdHigh - cfg.hallThresholdLow;
        _fillPercent = (hallSpan == 0) ? 0 : (uint8_t)(((uint32_t)(_hallValue - cfg.hallThresholdLow) * 100) / hallSpan);
      }
    }
    // Controle selon type moteur
    if (now - _lastPidTime >= PRESSURE_PID_INTERVAL_MS) {
      _lastPidTime = now;
      if (_targetPercent == 0) { setPumpPwm(0); _bangbangPumpOn = false; return; }

      if (cfg.motorType == MOTOR_TYPE_ONOFF) {
        // Bang-bang avec hysteresis pour moteurs On/Off
        uint8_t hyst = cfg.bangbangHysteresis;
        uint8_t lower = (_targetPercent > hyst) ? _targetPercent - hyst : 0;
        uint8_t upper = (_targetPercent + hyst < 100) ? _targetPercent + hyst : 100;
        if (_fillPercent < lower) {
          _bangbangPumpOn = true;
        } else if (_fillPercent > upper) {
          _bangbangPumpOn = false;
        }
        // Multi-pompe On/Off : graduer la demande selon l'ecart
        if (_bangbangPumpOn && cfg.numPumps > 1 && cfg.pumpCascadeThreshold > 0) {
          int16_t error = (int16_t)_targetPercent - (int16_t)_fillPercent;
          // Grand ecart (> 3x hysteresis) : toutes les pompes (cascade)
          // Petit ecart : pompe principale seule (sous le seuil cascade)
          setPumpPwm(error > (int16_t)(hyst * 3) ? 255 : 128);
        } else {
          setPumpPwm(_bangbangPumpOn ? 255 : 0);
        }
      } else {
        // PID pour moteurs PWM
        float error = (float)_targetPercent - (float)_fillPercent;
        float dt = PRESSURE_PID_INTERVAL_MS / 1000.0f;
        float kp = cfg.pidKp / 10.0f;
        float ki = cfg.pidKi / 10.0f;
        _pidIntegral += error * dt;
        if (_pidIntegral > 100.0f) _pidIntegral = 100.0f;
        if (_pidIntegral < -100.0f) _pidIntegral = -100.0f;
        float output = kp * error + ki * _pidIntegral;
        if (output <= 0) {
          setPumpPwm(0);
        } else {
          uint8_t pwm = (uint8_t)constrain(output * 2.55f, 0, 255);
          setPumpPwm(pwm);
        }
      }
    }
    return;
  }

  // ToF (VL53L0X / VL6180X): lecture I2C NON bloquante. _fillPercent n'est mis a
  // jour que sur une mesure fraiche et valide (jamais sur un timeout).
  if (serviceTofMeasurement()) {
    if (_distanceMm <= cfg.sensorMinMm) {
      _fillPercent = 100;
    } else if (_distanceMm >= cfg.sensorMaxMm) {
      _fillPercent = 0;
    } else {
      uint16_t sensorSpan = cfg.sensorMaxMm - cfg.sensorMinMm;
      _fillPercent = (sensorSpan == 0) ? 0 : 100 - (uint8_t)(((uint32_t)(_distanceMm - cfg.sensorMinMm) * 100) / sensorSpan);
    }
  }

  // Securite mesure perimee (§20): sans mesure ToF valide recente, on ne peut plus
  // reguler en securite -> couper la pompe plutot que de piloter sur une donnee
  // obsolete (un timeout lu comme distance 0 aurait sinon fait croire au reservoir plein).
  // Le critere est l'age de la DERNIERE mesure valide : un capteur fige qui ne
  // rend plus rien laisserait sinon la pompe reguler indefiniment sur l'avant-derniere.
  if (isMeasurementStale()) {
    setPumpPwm(0);
    _bangbangPumpOn = false;
    return;
  }

  // Controle pompe selon type moteur
  if (now - _lastPidTime >= PRESSURE_PID_INTERVAL_MS) {
    _lastPidTime = now;

    if (_targetPercent == 0) {
      setPumpPwm(0);
      _bangbangPumpOn = false;
      return;
    }

    // Securite : distance > 300mm = capteur hors portee
    if (_distanceMm > PUMP_SAFETY_MAX_DIST_MM) {
      setPumpPwm(0);
      return;
    }

    // Securite : distance trop courte (surgonflage)
    if (_distanceMm <= PUMP_SAFETY_MIN_MM && _distanceMm > 0) {
      setPumpPwm(0);
      _bangbangPumpOn = false;
      return;
    }

    if (cfg.motorType == MOTOR_TYPE_ONOFF) {
      // Bang-bang avec hysteresis pour moteurs On/Off
      uint8_t hyst = cfg.bangbangHysteresis;
      uint8_t lower = (_targetPercent > hyst) ? _targetPercent - hyst : 0;
      uint8_t upper = (_targetPercent + hyst < 100) ? _targetPercent + hyst : 100;
      if (_fillPercent < lower) {
        _bangbangPumpOn = true;
      } else if (_fillPercent > upper) {
        _bangbangPumpOn = false;
      }
      setPumpPwm(_bangbangPumpOn ? 255 : 0);
    } else {
      // PID pour moteurs PWM
      float error = (float)_targetPercent - (float)_fillPercent;
      float dt = PRESSURE_PID_INTERVAL_MS / 1000.0f;
      float kp = cfg.pidKp / 10.0f;
      float ki = cfg.pidKi / 10.0f;
      _pidIntegral += error * dt;
      if (_pidIntegral > 100.0f) _pidIntegral = 100.0f;
      if (_pidIntegral < -100.0f) _pidIntegral = -100.0f;
      float output = kp * error + ki * _pidIntegral;
      if (output <= 0) {
        setPumpPwm(0);
      } else {
        uint8_t pwm = (uint8_t)constrain(output * 2.55f, 0, 255);
        setPumpPwm(pwm);
      }
    }
  }
}

void PressureController::setTargetPercent(uint8_t percent) {
  if (percent > 100) percent = 100;
  _targetPercent = percent;
}

void PressureController::stop() {
  _targetPercent = 0;
  _testPumpIndex = -1;   // a full stop also ends any single-pump test
  setPumpPwm(0);
  _pidIntegral = 0;
  _bangbangPumpOn = false;
  for (uint8_t i = 0; i < MAX_PUMPS; i++) {
    _pumpActive[i] = false;
    _pumpActivateTime[i] = 0;
  }
  _activePumpCount = 0;
}

void PressureController::setEnabled(bool enabled) {
  _enabled = enabled;
  // Cut the output now (don't wait for the next update). Keep _targetPercent so
  // unmuting resumes the previous demand rather than requiring a fresh note.
  if (!enabled) { setPumpPwm(0); _bangbangPumpOn = false; }
}

void PressureController::testSinglePump(uint8_t index, uint8_t percent) {
  if (index >= cfg.numPumps || index >= MAX_PUMPS) return;
  if (percent > 100) percent = 100;
  _testPumpIndex = (int8_t)index;
  _testPumpPercent = percent;
}

void PressureController::stopSinglePumpTest() {
  _testPumpIndex = -1;
  setPumpPwm(0);
}

void PressureController::writePumpHw(uint8_t index, uint8_t pwm) {
  if (index >= cfg.numPumps || index >= MAX_PUMPS) return;
  if (cfg.motorType == MOTOR_TYPE_ONOFF) {
    digitalWrite(cfg.pumpPins[index], pwm > 0 ? HIGH : LOW);
  } else {
    uint8_t pumpVal = 0;
    if (pwm > 0) {
      pumpVal = cfg.pumpMinPwm[index] + (uint16_t)(cfg.pumpMaxPwm[index] - cfg.pumpMinPwm[index]) * pwm / 255;
      if (pumpVal < cfg.pumpMinPwm[index]) pumpVal = cfg.pumpMinPwm[index];
    }
    analogWrite(cfg.pumpPins[index], pumpVal);
  }
}

void PressureController::setPumpPwm(uint8_t pwm) {
  _currentPumpPwm = pwm;
  if (cfg.airMode < AIR_MODE_PUMP_VALVE) return;

  unsigned long now = millis();

  // === Mode parallele (1 pompe ou cascade desactivee) ===
  if (cfg.numPumps <= 1 || cfg.pumpCascadeThreshold == 0) {
    _activePumpCount = 0;
    for (uint8_t i = 0; i < cfg.numPumps && i < MAX_PUMPS; i++) {
      writePumpHw(i, pwm);
      _pumpActive[i] = (pwm > 0);
      if (pwm > 0) _activePumpCount++;
    }
    return;
  }

  // === Mode cascade : pompe 0 toujours active, les suivantes au seuil ===
  uint8_t cascade = cfg.pumpCascadeThreshold > 99 ? 99 : cfg.pumpCascadeThreshold;
  uint8_t threshPwm = (uint16_t)cascade * 255 / 100;
  _activePumpCount = 0;

  for (uint8_t i = 0; i < cfg.numPumps && i < MAX_PUMPS; i++) {
    if (i == 0) {
      // Pompe principale : toujours proportionnelle a la demande
      writePumpHw(0, pwm);
      _pumpActive[0] = (pwm > 0);
      if (pwm > 0) {
        _activePumpCount++;
        if (!_pumpActivateTime[0]) _pumpActivateTime[0] = now;
      } else {
        _pumpActivateTime[0] = 0;
      }
    } else {
      // Pompes secondaires : cascade
      if (pwm >= threshPwm && pwm > 0) {
        // Demande depasse le seuil -> pompe i doit s'activer
        if (!_pumpActive[i]) {
          _pumpActive[i] = true;
          _pumpActivateTime[i] = now;
        }
        // Delai stagger : attendre avant de demarrer
        unsigned long staggerDelay = (unsigned long)i * cfg.pumpStaggerMs;
        if (now - _pumpActivateTime[i] >= staggerDelay) {
          // PWM proportionnel au depassement du seuil
          uint8_t overflow = pwm - threshPwm;
          uint8_t denom = 255 - threshPwm;
          uint8_t pumpPwm = (denom == 0) ? 255 : (uint8_t)((uint16_t)overflow * 255 / denom);
          writePumpHw(i, pumpPwm);
          _activePumpCount++;
        } else {
          writePumpHw(i, 0); // En attente stagger
        }
      } else {
        // Sous le seuil : pompe i arretee
        _pumpActive[i] = false;
        _pumpActivateTime[i] = 0;
        writePumpHw(i, 0);
      }
    }
  }
}
