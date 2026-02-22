/***********************************************************************************************
 * HardwareInputs - Gestion du bouton d'appairage et de l'interrupteur BT/WiFi
 *
 * Bouton (GPIO0 - BOOT) :
 * - Appui court : declenche l'appairage BLE ou affiche l'IP WiFi
 * - Appui long (3s) : force le mode hotspot (en mode WiFi)
 *
 * Interrupteur (GPIO4) :
 * - LOW  = Mode Bluetooth (BLE-MIDI)
 * - HIGH = Mode WiFi (rtpMIDI + serveur web)
 *
 * Lu au demarrage pour determiner le mode. Changement en cours de
 * fonctionnement necessite un redemarrage.
 ***********************************************************************************************/
#ifndef HARDWARE_INPUTS_H
#define HARDWARE_INPUTS_H

#include <Arduino.h>
#include "settings.h"

enum OperatingMode {
  MODE_BLUETOOTH,
  MODE_WIFI
};

enum ButtonEvent {
  BUTTON_NONE,
  BUTTON_SHORT_PRESS,
  BUTTON_LONG_PRESS
};

class HardwareInputs {
public:
  HardwareInputs(uint8_t buttonPin, uint8_t switchPin);

  void begin();
  void update();

  // Mode de fonctionnement (lu au demarrage)
  OperatingMode getMode() const;

  // Evenement bouton (consomme l'evenement apres lecture)
  ButtonEvent getButtonEvent();

  // Etat brut du bouton (true = appuye)
  bool isButtonPressed() const;

private:
  uint8_t _buttonPin;
  uint8_t _switchPin;

  OperatingMode _mode;
  ButtonEvent _pendingEvent;

  // Debounce bouton
  bool _buttonState;
  bool _lastRawState;
  unsigned long _lastDebounceTime;

  // Detection appui long
  bool _buttonHeld;
  unsigned long _pressStartTime;
  bool _longPressTriggered;
};

#endif
