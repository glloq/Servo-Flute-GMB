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
    _distanceMm(0), _fillPercent(0),
    _targetPercent(0), _currentPumpPwm(0),
    _pidIntegral(0), _pidLastError(0),
    _lastPidTime(0), _lastReadTime(0) {
}

bool PressureController::begin() {
  _sensorType = cfg.sensorType;

  // Configurer pin pompe
  if (cfg.pumpEnabled) {
    pinMode(cfg.pumpPin, OUTPUT);
    analogWrite(cfg.pumpPin, 0);
  }

  if (!cfg.reservoirEnabled) {
    if (DEBUG) {
      Serial.println("DEBUG: PressureController - Mode pompe directe (sans capteur)");
    }
    return false;
  }

  // Detecter capteur sur I2C
  uint8_t addr = (_sensorType == 1) ? VL6180X_ADDR : VL53L0X_ADDR;
  Wire.beginTransmission(addr);
  uint8_t err = Wire.endTransmission();

  if (err != 0) {
    if (DEBUG) {
      Serial.print("DEBUG: PressureController - Capteur non detecte a 0x");
      Serial.println(addr, HEX);
    }
    _sensorDetected = false;
    return false;
  }

  _sensorDetected = true;

  // Initialisation VL6180X
  if (_sensorType == 1) {
    uint8_t fresh = readReg16(VL6180X_ADDR, VL6180X_REG_SYSTEM_FRESH_OUT_OF_RESET);
    if (fresh == 1) {
      // Configuration initiale minimale
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
      // Mesure unique mode
      writeReg16(VL6180X_ADDR, VL6180X_REG_READOUT_AVERAGING_PERIOD, 0x30);
      // Clear fresh flag
      writeReg16(VL6180X_ADDR, VL6180X_REG_SYSTEM_FRESH_OUT_OF_RESET, 0x00);
    }
  }

  if (DEBUG) {
    Serial.print("DEBUG: PressureController - Capteur ");
    Serial.print(_sensorType == 1 ? "VL6180X" : "VL53L0X");
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
  if (!cfg.pumpEnabled) return;

  unsigned long now = millis();

  // Mode pompe directe (sans capteur/reservoir)
  if (!cfg.reservoirEnabled || !_sensorDetected) {
    // PWM proportionnel direct a la cible
    if (_targetPercent == 0) {
      setPumpPwm(0);
    } else {
      uint8_t pwm = cfg.pumpMinPwm + (uint16_t)(cfg.pumpMaxPwm - cfg.pumpMinPwm) * _targetPercent / 100;
      setPumpPwm(pwm);
    }
    return;
  }

  // Lecture capteur periodique
  if (now - _lastReadTime >= PRESSURE_READ_INTERVAL_MS) {
    _distanceMm = readSensor();
    _lastReadTime = now;

    // Calculer pourcentage remplissage
    // Distance courte = ballon gonfle = plein, distance grande = vide
    if (_distanceMm <= cfg.sensorMinMm) {
      _fillPercent = 100;
    } else if (_distanceMm >= cfg.sensorMaxMm) {
      _fillPercent = 0;
    } else {
      _fillPercent = 100 - (uint8_t)(((uint32_t)(_distanceMm - cfg.sensorMinMm) * 100) / (cfg.sensorMaxMm - cfg.sensorMinMm));
    }
  }

  // Controle pompe par seuil de hauteur (pas de PID)
  // La pompe s'active quand le remplissage tombe sous la cible,
  // et s'arrete quand le remplissage atteint la cible ou si
  // la distance depasse 300mm (capteur hors portee / pas de ballon)
  if (now - _lastPidTime >= PRESSURE_PID_INTERVAL_MS) {
    _lastPidTime = now;

    if (_targetPercent == 0) {
      setPumpPwm(0);
      return;
    }

    // Securite : distance > 300mm = capteur ne voit pas le ballon, couper pompe
    if (_distanceMm > PUMP_SAFETY_MAX_DIST_MM) {
      setPumpPwm(0);
      return;
    }

    // Securite : distance trop courte (surgonflage), couper pompe
    if (_distanceMm <= PUMP_SAFETY_MIN_MM && _distanceMm > 0) {
      setPumpPwm(0);
      return;
    }

    // Hysteresis: pompe ON si remplissage < cible - 5%, OFF si >= cible
    if (_fillPercent < _targetPercent - 5) {
      // Pompe a PWM max pour gonfler
      setPumpPwm(cfg.pumpMaxPwm);
    } else if (_fillPercent >= _targetPercent) {
      setPumpPwm(0);
    }
    // Entre (cible-5) et cible : maintenir l'etat actuel (hysteresis)
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
}

void PressureController::setPumpPwm(uint8_t pwm) {
  if (pwm != _currentPumpPwm) {
    _currentPumpPwm = pwm;
    if (cfg.pumpEnabled) {
      analogWrite(cfg.pumpPin, pwm);
    }
  }
}
