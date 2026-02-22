#include "FanController.h"
#include "ConfigStorage.h"

// Rampe de demarrage douce (ms) pour eviter les appels de courant
#define FAN_RAMP_TIME_MS 300

FanController::FanController()
  : _speedPercent(0), _currentPwm(0), _targetPwm(0),
    _ready(false), _rampStartTime(0), _rampStartPwm(0),
    _idleActive(false), _lastNoteOffTime(0), _notePlaying(false) {
}

void FanController::begin() {
  if (cfg.airMode != AIR_MODE_FAN_SERVO) return;

  pinMode(cfg.fanPin, OUTPUT);
  analogWrite(cfg.fanPin, 0);

  if (DEBUG) {
    Serial.print("DEBUG: FanController - GPIO ");
    Serial.print(cfg.fanPin);
    Serial.print(" | PWM min=");
    Serial.print(cfg.fanMinPwm);
    Serial.print(" max=");
    Serial.print(cfg.fanMaxPwm);
    Serial.print(" | idle=");
    Serial.print(cfg.fanIdlePercent);
    Serial.print("% timeout=");
    Serial.print(cfg.fanIdleTimeoutMs);
    Serial.println("ms");
  }
}

uint8_t FanController::percentToPwm(uint8_t percent) {
  if (percent == 0) return 0;
  // Map 1-100% vers fanMinPwm..fanMaxPwm
  return cfg.fanMinPwm + (uint16_t)(cfg.fanMaxPwm - cfg.fanMinPwm) * percent / 100;
}

void FanController::setSpeed(uint8_t percent) {
  if (percent > 100) percent = 100;
  _speedPercent = percent;
  _targetPwm = percentToPwm(percent);

  // Demarrer la rampe
  _rampStartTime = millis();
  _rampStartPwm = _currentPwm;
  _ready = (_currentPwm == _targetPwm);

  if (DEBUG) {
    Serial.print("DEBUG: FanController - Vitesse: ");
    Serial.print(percent);
    Serial.print("% -> PWM ");
    Serial.println(_targetPwm);
  }
}

void FanController::stop() {
  _speedPercent = 0;
  _targetPwm = 0;
  _currentPwm = 0;
  _ready = false;
  _idleActive = false;
  _notePlaying = false;
  analogWrite(cfg.fanPin, 0);
}

void FanController::onNoteOn() {
  _notePlaying = true;
  _idleActive = false;
}

void FanController::onNoteOff() {
  _notePlaying = false;
  _lastNoteOffTime = millis();

  // Passer en idle: reduire a la vitesse idle configuree
  if (cfg.fanIdlePercent > 0) {
    _idleActive = true;
    setSpeed(cfg.fanIdlePercent);
    if (DEBUG) {
      Serial.print("DEBUG: FanController - Idle -> ");
      Serial.print(cfg.fanIdlePercent);
      Serial.println("%");
    }
  } else {
    // Idle = 0% => couper directement
    _idleActive = false;
    setSpeed(0);
  }
}

void FanController::update() {
  if (cfg.airMode != AIR_MODE_FAN_SERVO) return;

  // Gestion du timeout idle: couper apres N secondes sans noteOn
  if (_idleActive && !_notePlaying && cfg.fanIdleTimeoutMs > 0) {
    unsigned long elapsed = millis() - _lastNoteOffTime;
    if (elapsed >= cfg.fanIdleTimeoutMs) {
      _idleActive = false;
      setSpeed(0);
      if (DEBUG) {
        Serial.print("DEBUG: FanController - Idle timeout (");
        Serial.print(cfg.fanIdleTimeoutMs);
        Serial.println("ms) -> ARRET");
      }
    }
  }

  // Rampe PWM
  if (_currentPwm == _targetPwm) {
    _ready = true;
    return;
  }

  // Rampe lineaire vers la cible
  unsigned long elapsed = millis() - _rampStartTime;
  if (elapsed >= FAN_RAMP_TIME_MS) {
    _currentPwm = _targetPwm;
    _ready = true;
  } else {
    float t = (float)elapsed / FAN_RAMP_TIME_MS;
    _currentPwm = _rampStartPwm + (int16_t)((_targetPwm - _rampStartPwm) * t);
  }

  analogWrite(cfg.fanPin, _currentPwm);
}
