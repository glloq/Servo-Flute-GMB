#include "PressureController.h"
#include "ConfigStorage.h"
#include <Wire.h>

// Adresses I2C capteurs ToF
#define VL53L0X_ADDR  0x29
#define VL6180X_ADDR  0x29

// Registres VL6180X (mode simple)
#define VL6180X_REG_SYSTEM_FRESH_OUT_OF_RESET  0x0016
#define VL6180X_REG_SYSRANGE_START             0x0018
#define VL6180X_REG_RESULT_RANGE_STATUS        0x004D
#define VL6180X_REG_RESULT_RANGE_VAL           0x0062
#define VL6180X_REG_SYSTEM_INTERRUPT_CLEAR     0x0015
#define VL6180X_REG_READOUT_AVERAGING_PERIOD   0x010A

// Registres VL53L0X (mode simple)
#define VL53L0X_REG_SYSRANGE_START             0x00
#define VL53L0X_REG_RESULT_RANGE              0x1E

// Helper I2C pour VL6180X (registres 16-bit)
static void writeReg16(uint8_t addr, uint16_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write((reg >> 8) & 0xFF);
  Wire.write(reg & 0xFF);
  Wire.write(val);
  Wire.endTransmission();
}

static uint8_t readReg16(uint8_t addr, uint16_t reg) {
  Wire.beginTransmission(addr);
  Wire.write((reg >> 8) & 0xFF);
  Wire.write(reg & 0xFF);
  Wire.endTransmission(false);
  Wire.requestFrom(addr, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

// Helper I2C pour VL53L0X (registres 8-bit)
static void writeReg8(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static uint16_t readReg16_16(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(addr, (uint8_t)2);
  uint16_t val = 0;
  if (Wire.available() >= 2) {
    val = (Wire.read() << 8) | Wire.read();
  }
  return val;
}

PressureController::PressureController()
  : _sensorDetected(false), _sensorType(0),
    _distanceMm(0), _hallValue(0), _endstopActive(false), _fillPercent(0),
    _targetPercent(0), _currentPumpPwm(0),
    _activePumpCount(0), _bangbangPumpOn(false),
    _pidIntegral(0), _pidLastError(0),
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

  // Capteurs ToF (VL53L0X / VL6180X) : I2C
  uint8_t addr = (_sensorType == SENSOR_TYPE_TOF_VL6180X) ? VL6180X_ADDR : VL53L0X_ADDR;
  Wire.beginTransmission(addr);
  uint8_t err = Wire.endTransmission();

  if (err != 0) {
    if (DEBUG) {
      Serial.print("DEBUG: PressureController - Capteur ToF non detecte a 0x");
      Serial.println(addr, HEX);
    }
    _sensorDetected = false;
    return false;
  }

  _sensorDetected = true;

  // Initialisation VL6180X
  if (_sensorType == SENSOR_TYPE_TOF_VL6180X) {
    uint8_t fresh = readReg16(VL6180X_ADDR, VL6180X_REG_SYSTEM_FRESH_OUT_OF_RESET);
    if (fresh == 1) {
      writeReg16(VL6180X_ADDR, 0x0207, 0x01);
      writeReg16(VL6180X_ADDR, 0x0208, 0x01);
      writeReg16(VL6180X_ADDR, 0x0096, 0x00);
      writeReg16(VL6180X_ADDR, 0x0097, 0xFD);
      writeReg16(VL6180X_ADDR, 0x00E3, 0x00);
      writeReg16(VL6180X_ADDR, 0x00E4, 0x04);
      writeReg16(VL6180X_ADDR, 0x00E5, 0x02);
      writeReg16(VL6180X_ADDR, 0x00E6, 0x01);
      writeReg16(VL6180X_ADDR, 0x00E7, 0x03);
      writeReg16(VL6180X_ADDR, 0x00F5, 0x02);
      writeReg16(VL6180X_ADDR, 0x00D9, 0x05);
      writeReg16(VL6180X_ADDR, 0x00DB, 0xCE);
      writeReg16(VL6180X_ADDR, 0x00DC, 0x03);
      writeReg16(VL6180X_ADDR, 0x00DD, 0xF8);
      writeReg16(VL6180X_ADDR, 0x009F, 0x00);
      writeReg16(VL6180X_ADDR, 0x00A3, 0x3C);
      writeReg16(VL6180X_ADDR, 0x00B7, 0x00);
      writeReg16(VL6180X_ADDR, 0x00BB, 0x3C);
      writeReg16(VL6180X_ADDR, 0x00B2, 0x09);
      writeReg16(VL6180X_ADDR, 0x00CA, 0x09);
      writeReg16(VL6180X_ADDR, 0x0198, 0x01);
      writeReg16(VL6180X_ADDR, 0x01B0, 0x17);
      writeReg16(VL6180X_ADDR, 0x01AD, 0x00);
      writeReg16(VL6180X_ADDR, 0x00FF, 0x05);
      writeReg16(VL6180X_ADDR, 0x0100, 0x05);
      writeReg16(VL6180X_ADDR, 0x0199, 0x05);
      writeReg16(VL6180X_ADDR, 0x01A6, 0x1B);
      writeReg16(VL6180X_ADDR, 0x01AC, 0x3E);
      writeReg16(VL6180X_ADDR, 0x01A7, 0x1F);
      writeReg16(VL6180X_ADDR, 0x0030, 0x00);
      writeReg16(VL6180X_ADDR, VL6180X_REG_READOUT_AVERAGING_PERIOD, 0x30);
      writeReg16(VL6180X_ADDR, VL6180X_REG_SYSTEM_FRESH_OUT_OF_RESET, 0x00);
    }
  }

  if (DEBUG) {
    Serial.print("DEBUG: PressureController - Capteur ");
    Serial.print(_sensorType == SENSOR_TYPE_TOF_VL6180X ? "VL6180X" : "VL53L0X");
    Serial.println(" detecte et initialise");
  }

  return true;
}

uint16_t PressureController::readSensor() {
  if (!_sensorDetected) return 0;

  if (_sensorType == 1) {
    // VL6180X : single-shot range
    writeReg16(VL6180X_ADDR, VL6180X_REG_SYSTEM_INTERRUPT_CLEAR, 0x07);
    writeReg16(VL6180X_ADDR, VL6180X_REG_SYSRANGE_START, 0x01);
    // Attendre resultat (poll status)
    for (int i = 0; i < 50; i++) {
      uint8_t status = readReg16(VL6180X_ADDR, VL6180X_REG_RESULT_RANGE_STATUS);
      if (status & 0x04) break;
      delay(1);
    }
    uint8_t range = readReg16(VL6180X_ADDR, VL6180X_REG_RESULT_RANGE_VAL);
    writeReg16(VL6180X_ADDR, VL6180X_REG_SYSTEM_INTERRUPT_CLEAR, 0x07);
    return (uint16_t)range;
  } else {
    // VL53L0X : single-shot range
    writeReg8(VL53L0X_ADDR, VL53L0X_REG_SYSRANGE_START, 0x01);
    for (int i = 0; i < 50; i++) {
      uint8_t ready = 0;
      Wire.beginTransmission(VL53L0X_ADDR);
      Wire.write(0x13); // RESULT_INTERRUPT_STATUS
      Wire.endTransmission(false);
      Wire.requestFrom(VL53L0X_ADDR, (uint8_t)1);
      if (Wire.available()) ready = Wire.read();
      if (ready & 0x07) break;
      delay(1);
    }
    uint16_t range = readReg16_16(VL53L0X_ADDR, VL53L0X_REG_RESULT_RANGE);
    // Clear interrupt
    writeReg8(VL53L0X_ADDR, 0x0B, 0x01);
    return range;
  }
}

void PressureController::update() {
  if (cfg.airMode < AIR_MODE_PUMP_VALVE) return;

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
        _pidLastError = error;
        if (output <= 0) {
          setPumpPwm(0);
        } else {
          uint8_t pwm = (uint8_t)constrain(output * 2.55f, cfg.pumpMinPwm[0], cfg.pumpMaxPwm[0]);
          setPumpPwm(pwm);
        }
      }
    }
    return;
  }

  // ToF (VL53L0X / VL6180X): lecture I2C periodique
  if (now - _lastReadTime >= PRESSURE_READ_INTERVAL_MS) {
    _distanceMm = readSensor();
    _lastReadTime = now;
    if (_distanceMm <= cfg.sensorMinMm) {
      _fillPercent = 100;
    } else if (_distanceMm >= cfg.sensorMaxMm) {
      _fillPercent = 0;
    } else {
      uint16_t sensorSpan = cfg.sensorMaxMm - cfg.sensorMinMm;
      _fillPercent = (sensorSpan == 0) ? 0 : 100 - (uint8_t)(((uint32_t)(_distanceMm - cfg.sensorMinMm) * 100) / sensorSpan);
    }
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
      _pidLastError = error;
      if (output <= 0) {
        setPumpPwm(0);
      } else {
        uint8_t pwm = (uint8_t)constrain(output * 2.55f, cfg.pumpMinPwm[0], cfg.pumpMaxPwm[0]);
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
  setPumpPwm(0);
  _pidIntegral = 0;
  _pidLastError = 0;
  _bangbangPumpOn = false;
  for (uint8_t i = 0; i < MAX_PUMPS; i++) {
    _pumpActive[i] = false;
    _pumpActivateTime[i] = 0;
  }
  _activePumpCount = 0;
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
