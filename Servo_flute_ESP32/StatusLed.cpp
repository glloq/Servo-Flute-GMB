#include "StatusLed.h"

StatusLed::StatusLed(uint8_t pin)
  : _pin(pin), _pattern(LED_OFF), _ledState(false),
    _lastToggle(0), _flashCount(0), _inPause(false) {
}

void StatusLed::begin() {
  pinMode(_pin, OUTPUT);
  digitalWrite(_pin, LOW);
  _ledState = false;
  _lastToggle = millis();
}

void StatusLed::update() {
  unsigned long now = millis();
  unsigned long elapsed = now - _lastToggle;

  switch (_pattern) {
    case LED_OFF:
      if (_ledState) {
        digitalWrite(_pin, LOW);
        _ledState = false;
      }
      break;

    case LED_ON:
      if (!_ledState) {
        digitalWrite(_pin, HIGH);
        _ledState = true;
      }
      break;

    case LED_BLINK_FAST:
      if (elapsed >= LED_BLINK_FAST_MS) {
        _ledState = !_ledState;
        digitalWrite(_pin, _ledState ? HIGH : LOW);
        _lastToggle = now;
      }
      break;

    case LED_BLINK_SLOW:
      if (elapsed >= LED_BLINK_SLOW_MS) {
        _ledState = !_ledState;
        digitalWrite(_pin, _ledState ? HIGH : LOW);
        _lastToggle = now;
      }
      break;

    case LED_DOUBLE_FLASH:
      if (_inPause) {
        if (elapsed >= LED_DOUBLE_FLASH_PAUSE_MS) {
          _inPause = false;
          _flashCount = 0;
          _lastToggle = now;
        }
      } else {
        if (elapsed >= LED_DOUBLE_FLASH_MS) {
          _ledState = !_ledState;
          digitalWrite(_pin, _ledState ? HIGH : LOW);
          _lastToggle = now;

          if (!_ledState) {
            _flashCount++;
            if (_flashCount >= 2) {
              _inPause = true;
            }
          }
        }
      }
      break;

    case LED_TRIPLE_FLASH:
      if (_inPause) {
        if (elapsed >= LED_TRIPLE_FLASH_PAUSE_MS) {
          _inPause = false;
          _flashCount = 0;
          _lastToggle = now;
        }
      } else {
        if (elapsed >= LED_TRIPLE_FLASH_MS) {
          _ledState = !_ledState;
          digitalWrite(_pin, _ledState ? HIGH : LOW);
          _lastToggle = now;

          if (!_ledState) {
            _flashCount++;
            if (_flashCount >= 3) {
              _inPause = true;
            }
          }
        }
      }
      break;
  }
}

void StatusLed::setPattern(LedPattern pattern) {
  if (_pattern != pattern) {
    _pattern = pattern;
    _flashCount = 0;
    _inPause = false;
    _lastToggle = millis();
  }
}

LedPattern StatusLed::getPattern() const {
  return _pattern;
}
