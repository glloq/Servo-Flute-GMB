#include "HardwareInputs.h"

HardwareInputs::HardwareInputs(uint8_t buttonPin, uint8_t switchPin)
  : _buttonPin(buttonPin), _switchPin(switchPin),
    _mode(MODE_BLUETOOTH), _pendingEvent(BUTTON_NONE),
    _buttonState(false), _lastRawState(false),
    _lastDebounceTime(0), _buttonHeld(false),
    _pressStartTime(0), _longPressTriggered(false) {
}

void HardwareInputs::begin() {
  // Bouton avec pull-up interne (actif LOW)
  pinMode(_buttonPin, INPUT_PULLUP);

  // Switch avec pull-up interne
  pinMode(_switchPin, INPUT_PULLUP);

  // Lire le mode au demarrage
  // LOW = Bluetooth, HIGH = WiFi
  // Avec pull-up : switch ferme = LOW = BT, switch ouvert = HIGH = WiFi
  if (digitalRead(_switchPin) == LOW) {
    _mode = MODE_BLUETOOTH;
  } else {
    _mode = MODE_WIFI;
  }

  _buttonState = false;
  _lastRawState = digitalRead(_buttonPin) == LOW;  // Actif LOW

  if (DEBUG) {
    Serial.print("DEBUG: HardwareInputs - Mode: ");
    Serial.println(_mode == MODE_BLUETOOTH ? "BLUETOOTH" : "WIFI");
  }
}

void HardwareInputs::update() {
  // Lire l'etat brut du bouton (actif LOW avec pull-up)
  bool rawPressed = (digitalRead(_buttonPin) == LOW);
  unsigned long now = millis();

  // Debounce
  if (rawPressed != _lastRawState) {
    _lastDebounceTime = now;
  }
  _lastRawState = rawPressed;

  if ((now - _lastDebounceTime) >= BUTTON_DEBOUNCE_MS) {
    // L'etat est stable depuis assez longtemps
    bool prevState = _buttonState;
    _buttonState = rawPressed;

    // Front montant (bouton appuye)
    if (_buttonState && !prevState) {
      _pressStartTime = now;
      _buttonHeld = true;
      _longPressTriggered = false;
    }

    // Bouton maintenu appuye - detecter appui long
    if (_buttonState && _buttonHeld && !_longPressTriggered) {
      if ((now - _pressStartTime) >= BUTTON_LONG_PRESS_MS) {
        _pendingEvent = BUTTON_LONG_PRESS;
        _longPressTriggered = true;

        if (DEBUG) {
          Serial.println("DEBUG: HardwareInputs - Appui LONG detecte");
        }
      }
    }

    // Front descendant (bouton relache)
    if (!_buttonState && prevState) {
      _buttonHeld = false;

      // Si pas d'appui long deja declenche, c'est un appui court
      if (!_longPressTriggered) {
        _pendingEvent = BUTTON_SHORT_PRESS;

        if (DEBUG) {
          Serial.println("DEBUG: HardwareInputs - Appui COURT detecte");
        }
      }
    }
  }
}

OperatingMode HardwareInputs::getMode() const {
  return _mode;
}

ButtonEvent HardwareInputs::getButtonEvent() {
  ButtonEvent event = _pendingEvent;
  _pendingEvent = BUTTON_NONE;
  return event;
}

bool HardwareInputs::isButtonPressed() const {
  return _buttonState;
}
