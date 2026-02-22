#include "FanController.h"
#include "ConfigStorage.h"

// Rampe de demarrage douce (ms) pour eviter les appels de courant
#define FAN_RAMP_TIME_MS 300

FanController::FanController()
  : _speedPercent(0), _currentPwm(0), _targetPwm(0),
    _ready(false), _rampStartTime(0), _rampStartPwm(0) {
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
    Serial.println(cfg.fanMaxPwm);
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
  analogWrite(cfg.fanPin, 0);
}

void FanController::update() {
  if (cfg.airMode != AIR_MODE_FAN_SERVO) return;

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
